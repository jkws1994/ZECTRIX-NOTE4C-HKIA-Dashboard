#include "screen.h"
#include "qr/qrcodegen.h"
#include <stdio.h>
#include <string.h>
#include "font.inc"

enum ink { BLACK, WHITE, YELLOW, RED };
static const char *const tabs[] = {[PAGE_DEPARTURES]="PAX DEP",[PAGE_ARRIVALS]="PAX ARR",
                                   [PAGE_MAPS]="MAPS",[PAGE_WEATHER]="WEATHER"};

static void pixel(uint8_t *fb, int x, int y, enum ink ink) {
    if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H) return;
    uint8_t *v = &fb[(y*SCREEN_W+x)/4];
    int shift = 6-2*(x%4);
    *v = (*v & ~(3u<<shift)) | (ink<<shift);
}
static void rect(uint8_t *fb,int x,int y,int w,int h,enum ink ink) {
    for (int j=y; j<y+h; ++j)
        for (int i=x; i<x+w; ++i) pixel(fb,i,j,ink);
}
static int width(const struct font *f, const char *s) {
    int w = 0;
    for (;*s;++s) {
        unsigned c = (unsigned char)*s;
        w += f->glyphs[(c>=32 && c<=126 ? c : '?')-32].advance;
    }
    return w;
}
// Coordinates are baseline aligned. Columns clip at their own right edge.
static int label(uint8_t *fb,int x,int y,int end,const struct font *f,enum ink ink,const char *s) {
    for (;*s;++s) {
        unsigned c = (unsigned char)*s;
        const struct glyph *g = &f->glyphs[(c>=32 && c<=126 ? c : '?')-32];
        if (x+g->advance > end) break;
        int stride = (g->w+7)/8;
        for (int j=0;j<g->h;++j)
            for (int i=0;i<g->w;++i)
                if (f->data[g->offset+j*stride+i/8] & (0x80>>(i%8)))
                    pixel(fb,x+i+g->left,y+j+g->top,ink);
        x += g->advance;
    }
    return x;
}
static void masthead(uint8_t *fb) {
    memset(fb,0x55,FRAME_BYTES);
    label(fb,10,22,322,&f18,BLACK,"Hong Kong International Airport Flight Info");
}
void screen_battery(uint8_t *fb,int percent) {
    char text[8]="--%";
    rect(fb,325,0,75,30,WHITE);
    rect(fb,329,10,22,12,BLACK);
    rect(fb,351,13,3,6,BLACK);
    rect(fb,331,12,18,8,WHITE);
    if (percent>=0 && percent<=100) {
        int fill=(percent*16+50)/100;
        rect(fb,332,13,fill,6,percent<=15 ? RED : BLACK);
        snprintf(text,sizeof(text),"%d%%",percent);
    }
    label(fb,391-width(&f16,text),22,393,&f16,BLACK,text);
}
static void local_format(char *buf,size_t n,const char *fmt,time_t t) {
    struct tm local;
    localtime_r(&t,&local);
    strftime(buf,n,fmt,&local);
}
static void page_header(uint8_t *fb,enum page page) {
    masthead(fb);
    rect(fb,0,32,SCREEN_W,28,BLACK);
    for (int i=0;i<PAGE_COUNT;++i) {
        int left=i*SCREEN_W/PAGE_COUNT, right=(i+1)*SCREEN_W/PAGE_COUNT;
        bool active=(int)page==i;
        if (active) rect(fb,left,32,right-left,27,YELLOW);
        label(fb,(left+right-width(&f16,tabs[i]))/2,51,right,&f16,active ? BLACK : WHITE,tabs[i]);
    }
}
static void setup_header(uint8_t *fb,const char *title) {
    masthead(fb);
    rect(fb,0,32,SCREEN_W,28,BLACK);
    label(fb,10,52,391,&f18,YELLOW,title);
}
static void footer(uint8_t *fb,const char *left,const char *right,bool warning) {
    rect(fb,0,277,SCREEN_W,23,warning ? YELLOW : BLACK);
    enum ink ink=warning ? BLACK : WHITE;
    int right_x=390-width(&f14,right);
    label(fb,10,293,right_x-8,&f14,ink,left);
    label(fb,right_x,293,392,&f14,ink,right);
}
// Switch to the compact font for long route lists, combined gates or belt identifiers.
static void fit_label(uint8_t *fb,int x,int y,int end,const struct font *primary,
                      enum ink ink,const char *text) {
    if (width(primary,text)<=end-x) {
        label(fb,x,y,end,primary,ink,text);
    } else if (width(&f14,text)<=end-x) {
        label(fb,x,y,end,&f14,ink,text);
    } else {
        int pen=label(fb,x,y,end-width(&f14,"..."),&f14,ink,text);
        label(fb,pen,y,end,&f14,ink,"...");
    }
}
void screen_board(uint8_t *fb,enum board b,const struct schedule *s,int first,
                  const char *error,time_t now,time_t footer_stamp) {
    bool arrival=is_arrival(b);
    page_header(fb,arrival ? PAGE_ARRIVALS : PAGE_DEPARTURES);
    char text[100],stamp[32],range[56]="";
    if (s->count) {
        // Put a shared date above the time column; mixed-date groups keep a date on each row.
        int end=first+ROWS<s->count ? first+ROWS : s->count;
        char day[16],row_day[16];
        local_format(day,sizeof(day),"%d %b",s->rows[first].scheduled);
        bool shared_day=true;
        for (int k=first+1;k<end;++k) {
            local_format(row_day,sizeof(row_day),"%d %b",s->rows[k].scheduled);
            if (strcmp(day,row_day)) { shared_day=false; break; }
        }
        if (shared_day) snprintf(text,sizeof(text),"Sched  %s",day);
        else snprintf(text,sizeof(text),"Scheduled");
        label(fb,10,77,95,&f14,BLACK,text);
        label(fb,98,77,214,&f14,BLACK,"Flight / Status");
        label(fb,221,77,329,&f14,BLACK,arrival ? "Origin" : "Destination");
        label(fb,342,77,393,&f14,BLACK,arrival ? "Belt" : "Gate");
        rect(fb,9,82,382,2,BLACK);
        for (int i=first;i<end;++i) {
            const struct flight *r=&s->rows[i];
            int top=85+(i-first)*38;
            bool alert=flight_alert(r->status);
            local_format(text,sizeof(text),"%H:%M",r->scheduled);
            label(fb,10,top+(shared_day ? 26 : 19),96,&f22,BLACK,text);
            if (!shared_day) {
                local_format(text,sizeof(text),"%d %b",r->scheduled);
                label(fb,11,top+33,96,&f14,BLACK,text);
            }
            fit_label(fb,98,top+19,217,&f22,BLACK,r->number);
            fit_label(fb,221,top+19,332,&f20,BLACK,r->route);
            if (alert) rect(fb,98,top+24,3,10,RED);
            fit_label(fb,alert ? 107 : 98,top+33,331,&f14,alert ? RED : BLACK,r->status);
            fit_label(fb,342,top+19,393,&f22,BLACK,r->facility);
            fit_label(fb,343,top+33,393,&f14,BLACK,r->terminal);
            if (i+1<end) rect(fb,9,top+37,382,1,BLACK);
        }
        const char *next=s->count>ROWS && !error ? "   |   Front for more" : "";
        snprintf(range,sizeof(range),"%d-%d of %d%s",first+1,end,s->count,next);
    } else {
        bool waiting=!s->fetched;
        label(fb,14,155,391,&f27,BLACK,waiting ? "Waiting for flight data" : "No flights in this window");
        label(fb,15,181,391,&f18,BLACK,"The board refreshes automatically.");
    }
    bool aged=s->published && (now-s->published>15*60 || s->published>now+5*60);
    const char *state=error ? error : (aged ? "STALE FEED" : (s->published ? "HKIA" : "FETCHED"));
    snprintf(text,sizeof(text),"%s",state);
    if (s->fetched) {
        local_format(stamp,sizeof(stamp),"%d %b %H:%M HKT",footer_stamp);
        snprintf(text,sizeof(text),"%s   |   %s",state,stamp);
    }
    footer(fb,text,range,error || aged);
}
// Topological maps derived from HKIA's passenger guide. Coordinates are in display pixels.
static void map_stop(uint8_t *fb,int x,int y) {
    rect(fb,x-5,y-5,11,11,BLACK);
    rect(fb,x-2,y-2,5,5,WHITE);
}
static void map_center(uint8_t *fb,int x,int y,const struct font *font,const char *name) {
    label(fb,x-width(font,name)/2,y,SCREEN_W,font,BLACK,name);
}
static void gate_map(uint8_t *fb) {
    // West to east: the APM links Midfield, T1 West Hall, T1 East Hall and T2.
    const int rail_y=175;
    rect(fb,51,rail_y-2,297,5,RED);
    for (int y=135;y<rail_y;y+=8) rect(fb,235,y,3,5,BLACK);
    map_stop(fb,236,134);
    label(fb,249,134,397,&f20,BLACK,"Satellite");
    label(fb,249,149,397,&f14,BLACK,"Gates 13-21");
    label(fb,157,153,232,&f16,BLACK,"Sky Bridge");
    const int stops[]={51,142,236,348};
    for (unsigned i=0;i<sizeof(stops)/sizeof(stops[0]);++i) map_stop(fb,stops[i],rail_y);
    // Departing passengers travel from T2 towards the T1 gates.
    for (int i=0;i<7;++i) rect(fb,310+i,rail_y-i,1,2*i+1,RED);
    map_center(fb,51,200,&f20,"Midfield");
    map_center(fb,51,218,&f14,"Gates 201-230");
    map_center(fb,142,200,&f20,"T1 West");
    map_center(fb,142,218,&f14,"Gates 40-80");
    map_center(fb,236,200,&f20,"T1 East");
    map_center(fb,236,218,&f14,"Gates 1-12, 22-36");
    map_center(fb,348,200,&f20,"From T2");
    map_center(fb,348,218,&f14,"After security");
    rect(fb,12,252,24,4,RED);
    label(fb,44,259,243,&f16,BLACK,"Automated People Mover");
    for (int x=245;x<270;x+=8) rect(fb,x,253,5,3,BLACK);
    label(fb,278,259,397,&f16,BLACK,"Walk");
}
static void transport_map(uint8_t *fb) {
    // The train platform gives access to departures on either side.
    const int platform_y=146;
    rect(fb,48,platform_y-1,304,3,BLACK);
    map_stop(fb,48,platform_y);
    map_stop(fb,200,platform_y);
    map_stop(fb,352,platform_y);
    map_center(fb,48,133,&f20,"Terminal 1");
    map_center(fb,200,133,&f20,"Airport Express");
    map_center(fb,352,133,&f20,"Terminal 2");
    map_center(fb,200,167,&f14,"Departures platform");
    // Arriving passengers reach the bus and taxi areas from the T1 arrivals hall.
    rect(fb,47,platform_y+6,3,53,BLACK);
    rect(fb,48,202,122,3,BLACK);
    rect(fb,168,192,3,47,BLACK);
    rect(fb,169,191,33,3,BLACK);
    rect(fb,169,237,33,3,BLACK);
    map_stop(fb,48,203);
    map_stop(fb,204,192);
    map_stop(fb,204,238);
    label(fb,14,229,160,&f20,BLACK,"T1 Arrivals");
    label(fb,14,247,169,&f14,BLACK,"Halls A / B   L5");
    label(fb,218,197,396,&f20,BLACK,"Bus terminus");
    label(fb,218,213,396,&f14,BLACK,"Level 3");
    label(fb,218,243,396,&f20,BLACK,"Taxi station");
    label(fb,218,259,396,&f14,BLACK,"Level 3");
}
void screen_map(uint8_t *fb,enum airport_map map) {
    page_header(fb,PAGE_MAPS);
    bool gates=map==MAP_GATES;
    label(fb,10,89,286,&f27,BLACK,gates ? "Boarding gates" : "Ground transport");
    label(fb,390-width(&f16,"Schematic"),87,392,&f16,BLACK,"Schematic");
    rect(fb,10,97,380,2,BLACK);
    if (gates) gate_map(fb);
    else transport_map(fb);
    footer(fb,"Front button   Switch map",gates ? "Map 1 of 2" : "Map 2 of 2",false);
}
static bool old_observation(time_t at,time_t now) {
    return now-at>25*60 || at>now+5*60;
}
static void observation_stamp(uint8_t *fb,int x,int y,const struct observation *r,bool failed,time_t now) {
    if (!r->at) {
        label(fb,x,y,x+182,&f14,BLACK,"No observation");
        return;
    }
    char stamp[24],text[48];
    local_format(stamp,sizeof(stamp),"%d %b %H:%M",r->at);
    bool stale=old_observation(r->at,now);
    snprintf(text,sizeof(text),"%s%s",failed ? "Cached  " : (stale ? "Stale  " : ""),stamp);
    label(fb,x,y,x+182,&f14,(failed || stale) ? RED : BLACK,text);
}
static void weather_value(uint8_t *fb,int x,int y,const char *value,const char *unit,bool degrees) {
    // Leave room for the units even if the feed adds decimal places.
    int reserve=width(&f18,unit)+(degrees ? 13 : 6);
    const struct font *font=width(&f38,value)+reserve<=179 ? &f38 : &f22;
    if (width(font,value)+reserve>179) font=&f16;
    label(fb,x,y,x+180-reserve,font,BLACK,value);
    int pos=x+width(font,value)+4;
    if (degrees) {
        rect(fb,pos,y-18,5,5,BLACK);
        rect(fb,pos+1,y-17,3,3,WHITE);
        pos+=6;
    }
    label(fb,pos,y,x+181,&f18,BLACK,unit);
}
void screen_weather(uint8_t *fb,const struct airport_weather *weather,const char *error,time_t now) {
    page_header(fb,PAGE_WEATHER);
    label(fb,10,89,234,&f27,BLACK,"Chek Lap Kok");
    const char *source="Hong Kong Observatory";
    label(fb,390-width(&f16,source),87,392,&f16,BLACK,source);
    rect(fb,10,97,380,2,BLACK);
    rect(fb,199,105,1,163,BLACK);
    rect(fb,10,182,380,1,BLACK);
    const struct observation *temp=&weather->readings[AIR_TEMPERATURE];
    const struct observation *humidity=&weather->readings[HUMIDITY];
    const struct observation *wind=&weather->readings[WIND];
    bool shared=temp->at && !old_observation(temp->at,now);
    for (int i=0;i<WEATHER_SERIES_COUNT;++i)
        if (weather->failed[i] || weather->readings[i].at!=temp->at) shared=false;

    label(fb,12,121,191,&f16,BLACK,"Air temperature");
    label(fb,211,121,391,&f16,BLACK,"Relative humidity");
    weather_value(fb,12,shared ? 163 : 154,temp->at ? temp->value : "--","C",true);
    weather_value(fb,211,shared ? 163 : 154,humidity->at ? humidity->value : "--","%",false);
    label(fb,12,203,191,&f16,BLACK,"Mean wind (10 min)");
    label(fb,211,203,391,&f16,BLACK,"Peak gust (10 min)");
    weather_value(fb,12,241,wind->at ? wind->value : "--","km/h",false);
    weather_value(fb,211,241,wind->gust[0] ? wind->gust : "--","km/h",false);
    if (wind->direction[0]) label(fb,12,shared ? 263 : 256,195,&f16,BLACK,wind->direction);

    char observed[48]="HKO station observations";
    if (shared) {
        local_format(observed,sizeof(observed),"Observed   %d %b %H:%M HKT",temp->at);
    } else {
        observation_stamp(fb,12,174,temp,weather->failed[AIR_TEMPERATURE],now);
        observation_stamp(fb,211,174,humidity,weather->failed[HUMIDITY],now);
        observation_stamp(fb,12,272,wind,weather->failed[WIND],now);
        observation_stamp(fb,211,272,wind,weather->failed[WIND],now);
    }
    footer(fb,error ? error : observed,shared ? "" : "Hong Kong time",error!=NULL);
}
void screen_setup(uint8_t *fb,const char *ssid,const char *password) {
    setup_header(fb,"Wi-Fi setup");
    // Firmware generates both fields from ASCII letters, digits and hyphens.
    char payload[80];
    snprintf(payload,sizeof(payload),"WIFI:T:WPA;S:%s;P:%s;;",ssid,password);
    enum { max_version=4, scale=4 };
    uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(max_version)];
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(max_version)];
    bool encoded=qrcodegen_encodeText(payload,scratch,qr,qrcodegen_Ecc_MEDIUM,
        1,max_version,qrcodegen_Mask_AUTO,true);
    label(fb,12,85,394,&f22,BLACK,encoded ? "1. Scan to join the setup Wi-Fi" : "1. Join the setup Wi-Fi");
    if (encoded) {
        int size=qrcodegen_getSize(qr);
        // Center within a white 164px square including at least four quiet modules.
        int offset=(164-size*scale)/2;
        for (int y=0;y<size;++y)
            for (int x=0;x<size;++x)
                if (qrcodegen_getModule(qr,x,y))
                    rect(fb,6+offset+x*scale,85+offset+y*scale,scale,scale,BLACK);
    }
    label(fb,181,123,394,&f16,BLACK,"Network name");
    label(fb,181,148,395,&f20,BLACK,ssid);
    rect(fb,181,161,206,1,BLACK);
    label(fb,181,184,394,&f16,BLACK,"Setup password");
    label(fb,181,209,395,&f20,BLACK,password);
    rect(fb,0,249,400,28,YELLOW);
    label(fb,12,269,178,&f18,BLACK,"2. Open on your phone");
    label(fb,185,269,392,&f20,BLACK,"http://192.168.4.1");
    footer(fb,"Up / Down   Browse nearby networks","",false);
}
void screen_networks(uint8_t *fb,const struct nearby *list,int first,bool failed) {
    setup_header(fb,"Wi-Fi setup   /   Nearby networks");
    label(fb,10,86,396,&f22,BLACK,"Choose your network on your phone");
    char text[64];
    if (failed) snprintf(text,sizeof(text),"%s",list->count ? "Scan failed. Showing previous results." : "Scan failed. Press the front button to retry.");
    else if (list->count) snprintf(text,sizeof(text),"%d-%d of %d   /   Strongest first",first+1,
        first+NETWORK_ROWS<list->count ? first+NETWORK_ROWS : list->count,list->count);
    else snprintf(text,sizeof(text),"No networks found. Press the front button to retry.");
    label(fb,10,107,397,&f14,failed ? RED : BLACK,text);
    for (int i=0;i<NETWORK_ROWS && first+i<list->count;++i) {
        const struct access_point *ap=&list->items[first+i];
        int y=113+i*32;
        rect(fb,10,y,380,1,BLACK);
        // The panel font is ASCII. The phone displays the original network name.
        char name[33];
        int n=0;
        for (const uint8_t *p=ap->ssid;*p;++p) {
            if ((*p & 0xc0)==0x80) continue;
            name[n++]=*p>=32 && *p<=126 ? (char)*p : '?';
        }
        name[n]=0;

        const char *security=!strcmp(ap->security,"personal") ? "Password" :
                             (!strcmp(ap->security,"open") ? "Open" : "Unsupported");
        snprintf(text,sizeof(text),"%s   %d dBm",security,ap->rssi);
        int detail_x=390-width(&f14,text);
        fit_label(fb,10,y+22,detail_x-8,&f20,BLACK,n ? name : "?");
        label(fb,detail_x,y+21,393,&f14,BLACK,text);
    }
    footer(fb,"Up / Down   Browse or return to QR","Front   Scan again",false);
}
