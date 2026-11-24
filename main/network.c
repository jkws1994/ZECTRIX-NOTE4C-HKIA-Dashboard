#include "network.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EventGroupHandle_t net_events;
static SemaphoreHandle_t scan_lock;
static bool sntp_started, clock_synced;
enum { HAS_IP=1, DISCONNECTED=2, MAX_RESPONSE=1024*1024 };
static const char *TAG="HKG";
static void *json_malloc(size_t n) { return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
static void events(void *ctx, esp_event_base_t base, int32_t id, void *payload) {
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(net_events,DISCONNECTED);
        xEventGroupSetBits(net_events,HAS_IP);
    } else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(net_events,HAS_IP);
        xEventGroupSetBits(net_events,DISCONNECTED);
    }
}
void network_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable=0;
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("HK",true));
    net_events=xEventGroupCreate();
    configASSERT(net_events);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,events,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,events,NULL));
    cJSON_Hooks hooks={.malloc_fn=json_malloc,.free_fn=heap_caps_free};
    cJSON_InitHooks(&hooks);
}
bool network_load(wifi_config_t *cfg) {
    nvs_handle_t nvs;
    if (nvs_open("hkg-board",NVS_READONLY,&nvs)!=ESP_OK) return false;
    size_t len=sizeof(cfg->sta);
    esp_err_t err=nvs_get_blob(nvs,"station",&cfg->sta,&len);
    nvs_close(nvs);
    return err==ESP_OK && len==sizeof(cfg->sta) && cfg->sta.ssid[0];
}
void network_station(const wifi_config_t *cfg) {
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t station=*cfg;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&station));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    esp_sntp_config_t ntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ntp.start=false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&ntp));
}

extern const char setup_html[] asm("_binary_setup_html_start");
static esp_err_t setup_home(httpd_req_t *r) {
    httpd_resp_set_type(r,"text/html; charset=utf-8");
    return httpd_resp_send(r,setup_html,HTTPD_RESP_USE_STRLEN);
}
static const char *network_security(wifi_auth_mode_t auth) {
    switch (auth) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return "personal";
    case WIFI_AUTH_WPA_ENTERPRISE:
    case WIFI_AUTH_WPA2_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192: return "enterprise";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_OWE: return "owe";
    default: return "other";
    }
}
// Set up a new snapshot only after a successful scan. Each caller owns its copy.
bool network_scan(struct nearby *out) {
    uint16_t count=0;
    wifi_ap_record_t *records=NULL;
    xSemaphoreTake(scan_lock,portMAX_DELAY);
    esp_err_t err=esp_wifi_scan_start(NULL,true);
    if (err==ESP_OK) err=esp_wifi_scan_get_ap_num(&count);
    if (err==ESP_OK && count) {
        records=heap_caps_malloc((size_t)count*sizeof(*records),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        err=records ? esp_wifi_scan_get_ap_records(&count,records) : ESP_ERR_NO_MEM;
    }
    if (err!=ESP_OK || !count) esp_wifi_clear_ap_list();
    xSemaphoreGive(scan_lock);
    if (err!=ESP_OK) { free(records); return false; }

    struct nearby next={0};
    if (count) {
        next.items=heap_caps_malloc((size_t)count*sizeof(*next.items),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!next.items) { free(records); return false; }
    }
    // ESP-IDF returns strongest first. Keep distinct security types for shared names.
    for (int i=0;i<count;++i) {
        const wifi_ap_record_t *ap=&records[i];
        if (!ap->ssid[0]) continue;
        const char *security=network_security(ap->authmode);
        int j=0;
        while (j<next.count && (strncmp((const char *)next.items[j].ssid,(const char *)ap->ssid,32)
                                 || strcmp(next.items[j].security,security))) ++j;
        if (j<next.count) continue;
        struct access_point *item=&next.items[next.count++];
        memcpy(item->ssid,ap->ssid,32);
        item->ssid[32]=0;
        item->security=security;
        item->rssi=ap->rssi;
    }
    free(records);
    free(out->items);
    *out=next;
    return true;
}
static esp_err_t setup_scan(httpd_req_t *r) {
    struct nearby list={0};
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    if (!network_scan(&list))
        return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Wi-Fi scan unavailable.");
    httpd_resp_set_type(r,"application/json");
    esp_err_t err=httpd_resp_send_chunk(r,"[",1);
    for (int i=0;err==ESP_OK && i<list.count;++i) {
        const struct access_point *ap=&list.items[i];
        static const char digits[]="0123456789abcdef";
        char hex[65], entry[128];
        size_t len=strlen((const char *)ap->ssid);
        for (size_t j=0;j<len;++j) {
            hex[2*j]=digits[ap->ssid[j]>>4];
            hex[2*j+1]=digits[ap->ssid[j]&15];
        }
        hex[2*len]=0;
        snprintf(entry,sizeof(entry),"%s{\"ssid\":\"%s\",\"security\":\"%s\",\"rssi\":%d}",
                 i ? "," : "",hex,ap->security,ap->rssi);
        err=httpd_resp_sendstr_chunk(r,entry);
    }
    free(list.items);
    if (err==ESP_OK) err=httpd_resp_send_chunk(r,"]",1);
    if (err==ESP_OK) err=httpd_resp_send_chunk(r,NULL,0);
    return err;
}
static int hex_value(char ch) {
    if (ch>='0' && ch<='9') return ch-'0';
    ch=(char)tolower((unsigned char)ch);
    return ch>='a' && ch<='f' ? ch-'a'+10 : -1;
}
static bool decode_form(const char *src, uint8_t *dst, size_t cap, size_t *used) {
    size_t n=0;
    for (;*src;++src) {
        unsigned char c=(unsigned char)*src;
        if (c=='%') {
            if (!src[1] || !src[2]) return false;
            int a=hex_value(src[1]), b=hex_value(src[2]);
            if (a<0 || b<0) return false;
            c=(a<<4)|b;
            src+=2;
        } else if (c=='+') c=' ';
        if (!c || n>=cap) return false;
        dst[n++]=c;
    }
    *used=n;
    return true;
}
static bool decode_ssid(const char *field, uint8_t *ssid) {
    size_t length=0;
    if (!strncmp(field,"ssid=",5))
        return decode_form(field+5,ssid,32,&length) && length>0;
    if (strncmp(field,"ssid_hex=",9)) return false;
    const char *hex=field+9;
    size_t digits=strlen(hex);
    if (!digits || digits>64 || digits%2) return false;
    for (size_t i=0;i<digits;i+=2) {
        int high=hex_value(hex[i]), low=hex_value(hex[i+1]);
        if (high<0 || low<0 || !(high||low)) return false;
        ssid[i/2]=(high<<4)|low;
    }
    return true;
}
static esp_err_t setup_save(httpd_req_t *r) {
    char body[384];
    if (!r->content_len || r->content_len>=sizeof(body)) goto invalid;
    size_t done=0;
    while (done<r->content_len) {
        int n=httpd_req_recv(r,body+done,r->content_len-done);
        if (n<=0) return ESP_FAIL;
        done+=n;
    }
    body[done]=0;
    char *pass=strstr(body,"&pass=");
    if (!pass) goto invalid;
    *pass=0;
    wifi_config_t cfg={0};
    size_t pass_len;
    if (!decode_ssid(body,cfg.sta.ssid) ||
        !decode_form(pass+6,cfg.sta.password,63,&pass_len) || (pass_len && pass_len<8)) goto invalid;
    cfg.sta.threshold.authmode=pass_len ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("hkg-board",NVS_READWRITE,&nvs);
    if (err==ESP_OK) {
        err=nvs_set_blob(nvs,"station",&cfg.sta,sizeof(cfg.sta));
        if (err==ESP_OK) err=nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err!=ESP_OK) return httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"Storage unavailable.");
    httpd_resp_sendstr(r,"Wi-Fi saved. The board is restarting.");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
invalid:
    return httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"Check network name and password length.");
}
void network_setup(char ssid[33],char password[17]) {
    scan_lock=xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(scan_lock ? ESP_OK : ESP_ERR_NO_MEM);
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP));
    snprintf(ssid,33,"HKG-Board-%02X%02X",mac[4],mac[5]);
    snprintf(password,17,"%08lX%08lX",(unsigned long)esp_random(),(unsigned long)esp_random());
    wifi_config_t ap={0};
    memcpy(ap.ap.ssid,ssid,strlen(ssid));
    memcpy(ap.ap.password,password,strlen(password));
    ap.ap.ssid_len=strlen(ssid);
    ap.ap.channel=1;
    ap.ap.max_connection=2;
    ap.ap.authmode=WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    httpd_handle_t server=NULL;
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server,&config));
    httpd_uri_t home={.uri="/",.method=HTTP_GET,.handler=setup_home};
    httpd_uri_t scan={.uri="/networks",.method=HTTP_GET,.handler=setup_scan};
    httpd_uri_t save={.uri="/save",.method=HTTP_POST,.handler=setup_save};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&home));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&scan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&save));
}
static const char *connect_station(void) {
    if (!(xEventGroupGetBits(net_events)&HAS_IP)) {
        xEventGroupClearBits(net_events,DISCONNECTED);
        if (esp_wifi_connect()!=ESP_OK) return "WI-FI OFFLINE";
        EventBits_t bits=xEventGroupWaitBits(net_events,HAS_IP|DISCONNECTED,pdFALSE,pdFALSE,pdMS_TO_TICKS(12000));
        if (!(bits&HAS_IP)) return "WI-FI OFFLINE";
    }
    if (!sntp_started) {
        if (esp_netif_sntp_start()!=ESP_OK) return "CLOCK SYNC FAILED";
        sntp_started=true;
    }
    if (!clock_synced) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(12000))!=ESP_OK) return "CLOCK SYNC FAILED";
        clock_synced=true;
    }
    return NULL;
}
static const char *download(const char *url,size_t limit,char **result,size_t *bytes) {
    *result=NULL;
    *bytes=0;
    esp_http_client_config_t config={.url=url,.crt_bundle_attach=esp_crt_bundle_attach,
        .timeout_ms=8000,.buffer_size=4096,.user_agent="NOTE4C-HKG/1.0"};
    esp_http_client_handle_t h=esp_http_client_init(&config);
    if (!h) return "HTTP MEMORY LOW";
    char *body=NULL;
    const char *error="FEED OFFLINE";
    if (esp_http_client_open(h,0)!=ESP_OK) goto finish;
    int64_t length=esp_http_client_fetch_headers(h);
    int status=esp_http_client_get_status_code(h);
    if (status!=200 || length<0) { ESP_LOGW(TAG,"Feed HTTP %d",status); goto finish; }
    if ((uint64_t)length>limit) { error="FEED TOO LARGE"; goto finish; }
    size_t cap=length ? (size_t)length : limit;
    body=heap_caps_malloc(cap+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!body) { error="PSRAM LOW"; goto finish; }
    size_t used=0;
    int64_t deadline=esp_timer_get_time()+30000000;
    while (used<cap && esp_timer_get_time()<deadline) {
        int want=(cap-used)>4096 ? 4096 : (int)(cap-used);
        int n=esp_http_client_read(h,body+used,want);
        if (n<0) goto finish;
        if (!n) break;
        used+=(size_t)n;
    }
    if (!esp_http_client_is_complete_data_received(h) || (length && used!=length)) goto finish;
    body[used]=0;
    *result=body;
    *bytes=used;
    body=NULL;
    error=NULL;
finish:
    free(body);
    esp_http_client_cleanup(h);
    return error;
}

const char *network_fetch(enum board b,struct schedule *out) {
    const char *error=connect_station();
    if (error) return error;
    time_t now=time(NULL);
    struct tm local;
    localtime_r(&now,&local);
    char day[11], url[212];
    strftime(day,sizeof(day),"%Y-%m-%d",&local);
    snprintf(url,sizeof(url),"https://www.hongkongairport.com/flightinfo-rest/rest/flights"
        "?date=%s&lang=en&cargo=false&arrival=%s&span=2",day,is_arrival(b)?"true":"false");
    char *body;
    size_t length;
    error=download(url,MAX_RESPONSE,&body,&length);
    if (error) return error;
    bool ok=flight_parse(body,length,b,time(NULL),out);
    free(body);
    return ok ? NULL : "INVALID AIRPORT DATA";
}
const char *network_weather(struct airport_weather *out) {
    static const char *const files[]={"latest_1min_temperature.csv","latest_1min_humidity.csv","latest_10min_wind.csv"};
    const char *connection=connect_station();
    int failures=0;
    for (int i=0;i<WEATHER_SERIES_COUNT;++i) {
        bool ok=false;
        if (!connection) {
            char url[128], *body;
            size_t length;
            snprintf(url,sizeof(url),"https://data.weather.gov.hk/weatherAPI/hko_data/regional-weather/%s",files[i]);
            if (!download(url,16384,&body,&length)) {
                ok=length && weather_parse(body,i,&out->readings[i]);
                free(body);
            }
        }
        out->failed[i]=!ok;
        if (!ok) ++failures;
    }
    if (connection) return connection;
    if (failures==WEATHER_SERIES_COUNT) return "HKO DATA UNAVAILABLE";
    return failures ? "PARTIAL HKO DATA" : NULL;
}
