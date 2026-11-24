#include "flights.h"
#include "network.h"
#include "note4c.h"
#include "screen.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

enum action { PREVIOUS_PAGE, NEXT_PAGE, DETAILS, REFRESH };
static QueueHandle_t keys;
static struct view {
    struct schedule schedule;
    const char *error;
    int first;
    int64_t due;
    unsigned quiet_polls;
    unsigned failures;
    bool manual_refresh;
} views[BOARD_COUNT];

enum { ACTIVE_SECONDS=60, QUIET_SECONDS=120, STAMP_SECONDS=300 };
static const int64_t SECOND_US=1000000;

static int next_poll_seconds(struct view *v, bool changed) {
    if (v->error) {
        static const int retry_seconds[]={60,120,300};
        v->quiet_polls=0;
        if (v->failures<3) ++v->failures;
        return retry_seconds[v->failures-1];
    }
    if (changed || v->manual_refresh || v->failures) v->quiet_polls=0;
    else if (v->quiet_polls<3) ++v->quiet_polls;
    v->failures=0;
    return v->quiet_polls==3 ? QUIET_SECONDS : ACTIVE_SECONDS;
}

static int pressed_key(void) {
    if (note4c_pressed(BTN_FRONT)) return BTN_FRONT;
    if (note4c_pressed(BTN_UP)) return BTN_UP;
    if (note4c_pressed(BTN_DOWN)) return BTN_DOWN;
    return -1;
}
static enum action button_action(int key,int64_t held_us) {
    if (key==BTN_UP) return PREVIOUS_PAGE;
    if (key==BTN_DOWN) return NEXT_PAGE;
    return held_us>=1000000 ? REFRESH : DETAILS;
}
static void read_keys(void *unused) {
    // Act on release. A separate sampler also captures presses during a panel refresh.
    int raw=pressed_key(), stable=raw;
    int64_t changed=esp_timer_get_time(), down=changed;
    bool armed=stable<0;
    for (;;) {
        int key=pressed_key();
        int64_t now=esp_timer_get_time();
        if (key!=raw) { raw=key; changed=now; }
        if (raw!=stable && now-changed>=40000) {
            if (raw<0 && armed) {
                enum action action=button_action(stable,now-down);
                xQueueSend(keys,&action,0);
            }
            if (raw>=0) down=now;
            if (raw<0) armed=true;
            stable=raw;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static int last_offset(const struct view *v) {
    return v->schedule.count ? (v->schedule.count-1)/ROWS*ROWS : 0;
}
static enum page page=PAGE_DEPARTURES;
static enum airport_map map=MAP_GATES;
static struct airport_weather weather;
static int64_t weather_due;
static const char *weather_error;

static enum board selected_flight_board(void) {
    return page==PAGE_ARRIVALS ? PAX_ARR : PAX_DEP;
}
static void apply(enum action a) {
    if (a==PREVIOUS_PAGE || a==NEXT_PAGE) {
        page=(page+(a==NEXT_PAGE ? 1 : PAGE_COUNT-1))%PAGE_COUNT;
        return;
    }
    if (page==PAGE_MAPS) {
        if (a==DETAILS) map=map==MAP_GATES ? MAP_TRANSPORT : MAP_GATES;
        return;
    }
    if (page==PAGE_WEATHER) {
        if (a==REFRESH) weather_due=0;
        return;
    }
    struct view *v=&views[selected_flight_board()];
    if (a==REFRESH) {
        v->manual_refresh=true;
        v->due=0;
    } else if (a==DETAILS) v->first=v->first<last_offset(v) ? v->first+ROWS : 0;
}

struct panel {
    uint8_t *frame, *shown;
    bool valid;
    int battery;
    time_t stamp;
    int64_t last_paint;
};
static void present(struct panel *p,time_t stamp) {
    screen_battery(p->frame,p->battery);
    if (p->valid && !memcmp(p->shown,p->frame,FRAME_BYTES)) return;
    p->battery=note4c_battery_percent();
    screen_battery(p->frame,p->battery);
    p->valid=note4c_show(p->frame);
    if (p->valid) {
        memcpy(p->shown,p->frame,FRAME_BYTES);
        p->stamp=stamp;
        p->last_paint=esp_timer_get_time();
    }
}
static TickType_t wait_until(int64_t due,bool valid) {
    int64_t wait_us=due-esp_timer_get_time();
    if (!valid && wait_us>30*SECOND_US) wait_us=30*SECOND_US;
    const int64_t tick_us=portTICK_PERIOD_MS*1000;
    return wait_us>0 ? (TickType_t)((wait_us+tick_us-1)/tick_us) : 0;
}
static TickType_t update_flights(struct panel *p) {
    enum board b=selected_flight_board();
    struct view *v=&views[b];
    bool polled=esp_timer_get_time()>=v->due;
    if (polled) {
        v->error=network_fetch(b,&v->schedule);
        if (v->error) ESP_LOGW("HKG","%s on board %d",v->error,b);
        if (v->first>last_offset(v)) v->first=last_offset(v);
    }
    time_t now=time(NULL);
    time_t latest=v->schedule.published ? v->schedule.published : v->schedule.fetched;
    time_t compare_stamp=p->valid && p->stamp ? p->stamp : latest;
    screen_board(p->frame,b,&v->schedule,v->first,v->error,now,compare_stamp);
    screen_battery(p->frame,p->battery);
    bool changed=!p->valid || memcmp(p->shown,p->frame,FRAME_BYTES)!=0;
    int64_t checked_at=esp_timer_get_time();
    if (polled) v->due=checked_at+next_poll_seconds(v,changed)*SECOND_US;
    bool stamp_due=v->manual_refresh || checked_at-p->last_paint>=STAMP_SECONDS*SECOND_US;
    v->manual_refresh=false;
    if (changed || (stamp_due && compare_stamp!=latest)) {
        if (compare_stamp!=latest)
            screen_board(p->frame,b,&v->schedule,v->first,v->error,now,latest);
        present(p,latest);
    }
    return wait_until(v->due,p->valid);
}
static TickType_t update_page(struct panel *p) {
    if (page==PAGE_MAPS) {
        screen_map(p->frame,map);
        present(p,0);
        // A successful map paint stays on e-paper until the next navigation event.
        return p->valid ? portMAX_DELAY : pdMS_TO_TICKS(30000);
    }
    if (page==PAGE_WEATHER) {
        if (esp_timer_get_time()>=weather_due) {
            weather_error=network_weather(&weather);
            weather_due=esp_timer_get_time()+(weather_error ? 120 : 600)*SECOND_US;
        }
        screen_weather(p->frame,&weather,weather_error,time(NULL));
        present(p,0);
        return wait_until(weather_due,p->valid);
    }
    return update_flights(p);
}

static int setup_page_count(int networks) {
    int sheets=(networks+NETWORK_ROWS-1)/NETWORK_ROWS;
    return 1+(sheets ? sheets : 1); // QR plus at least one results screen.
}
static void run_setup(struct panel *panel) {
    char ssid[33], password[17];
    network_setup(ssid,password);
    struct nearby list={0};
    int index=0;
    bool scanned=false,failed=false;
    for (;;) {
        if (index==0) screen_setup(panel->frame,ssid,password);
        else screen_networks(panel->frame,&list,(index-1)*NETWORK_ROWS,failed);
        present(panel,0);
        enum action a;
        if (xQueueReceive(keys,&a,panel->valid ? portMAX_DELAY : pdMS_TO_TICKS(30000))!=pdTRUE)
            continue;
        if (a==PREVIOUS_PAGE || a==NEXT_PAGE) {
            if (!scanned) { failed=!network_scan(&list); scanned=true; }
            int pages=setup_page_count(list.count);
            index=(index+(a==NEXT_PAGE ? 1 : pages-1))%pages;
        } else if (index) {
            failed=!network_scan(&list);
            int last=setup_page_count(list.count)-1;
            if (index>last) index=last;
        }
    }
}
void app_main(void) {
    note4c_init();
    bool setup_requested=note4c_pressed(BTN_UP);
    setenv("TZ","HKT-8",1);
    tzset();
    network_init();
    uint8_t *frame=heap_caps_malloc(FRAME_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint8_t *shown=heap_caps_malloc(FRAME_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(frame && shown ? ESP_OK : ESP_ERR_NO_MEM);
    keys=xQueueCreate(12,sizeof(enum action));
    configASSERT(keys);
    BaseType_t started=xTaskCreate(read_keys,"buttons",3072,NULL,4,NULL);
    ESP_ERROR_CHECK(started==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    struct panel panel={.frame=frame,.shown=shown,.battery=-1};
    wifi_config_t station={0};
    if (setup_requested || !network_load(&station)) {
        run_setup(&panel);
        return;
    }
    network_station(&station);
    enum board first=selected_flight_board();
    screen_board(frame,first,&views[first].schedule,0,"CONNECTING",time(NULL),0);
    present(&panel,0);
    for (;;) {
        TickType_t ticks=update_page(&panel);
        enum action a;
        if (xQueueReceive(keys,&a,ticks)==pdTRUE) {
            do { apply(a); } while (xQueueReceive(keys,&a,0)==pdTRUE);
        }
    }
}
