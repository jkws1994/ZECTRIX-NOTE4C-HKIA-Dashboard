#include "weather.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static time_t csv_time(const char *s) {
    if (strlen(s)!=12 || strspn(s,"0123456789")!=12) return 0;
    int y,m,d,h,min;
    if (sscanf(s,"%4d%2d%2d%2d%2d",&y,&m,&d,&h,&min)!=5 || y<2020 || y>2099) return 0;
    struct tm t={.tm_year=y-1900,.tm_mon=m-1,.tm_mday=d,.tm_hour=h,.tm_min=min};
    time_t at=mktime(&t);  // The dashboard sets TZ to HKT-8 at boot.
    char check[16];
    if (!strftime(check,sizeof(check),"%Y%m%d%H%M",&t) || strcmp(check,s)) return 0;
    return at;
}
static bool numeric(const char *s,float min,float max) {
    if (!*s || strlen(s)>=10 || strspn(s,"-.0123456789")!=strlen(s)) return false;
    char *end;
    float value=strtof(s,&end);
    return *end==0 && isfinite(value) && value>=min && value<=max;
}
bool weather_parse(char *csv,enum weather_series series,struct observation *out) {
    static const char *const columns[]={
        "Air Temperature(degree Celsius)","Relative Humidity(percent)",
        "10-Minute Mean Wind Direction(Compass points),10-Minute Mean Speed(km/hour),10-Minute Maximum Gust(km/hour)"
    };
    const char prefix[]="Date time,Automatic Weather Station,";
    char *rest=NULL;
    char *header=strtok_r(csv,"\r\n",&rest);
    if (!header || (unsigned)series>=WEATHER_SERIES_COUNT) return false;
    if (!strncmp(header,"\xEF\xBB\xBF",3)) header+=3;
    if (strncmp(header,prefix,sizeof(prefix)-1) || strcmp(header+sizeof(prefix)-1,columns[series])) return false;
    for (char *row=strtok_r(NULL,"\r\n",&rest);row;row=strtok_r(NULL,"\r\n",&rest)) {
        char *fields[5];
        int count=1;
        fields[0]=row;
        for (char *p=row;*p;++p) {
            if (*p!=',') continue;
            *p=0;
            if (count==5) { ++count; break; }
            fields[count++]=p+1;
        }
        if (count!=(series==WIND ? 5 : 3) || strcmp(fields[1],"Chek Lap Kok")) continue;
        struct observation next={.at=csv_time(fields[0])};
        if (!next.at || next.at<out->at) return false;
        const char *value=fields[series==WIND ? 3 : 2];
        float low=series==AIR_TEMPERATURE ? -50 : 0;
        float high=series==HUMIDITY ? 100 : (series==WIND ? 500 : 65);
        if (!numeric(value,low,high)) return false;
        snprintf(next.value,sizeof(next.value),"%s",value);
        if (series==WIND) {
            const char *direction=fields[2];
            if (*direction && strcmp(direction,"N/A") && strlen(direction)<sizeof(next.direction))
                snprintf(next.direction,sizeof(next.direction),"%s",direction);
            if (numeric(fields[4],0,500)) snprintf(next.gust,sizeof(next.gust),"%s",fields[4]);
        }
        *out=next;
        return true;
    }
    return false;
}
