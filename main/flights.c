#include "flights.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const cJSON *field(const cJSON *o, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static const char *string(const cJSON *v) {
    return cJSON_IsString(v) ? v->valuestring : "";
}
static bool clock_text(const char *s) {
    return strlen(s) >= 5 && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
        s[2] == ':' && isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4]) &&
        (s[0] < '2' || (s[0] == '2' && s[1] <= '3')) && s[3] <= '5';
}
static time_t hkt(const char *date, const char *clock) {
    int y, m, d;
    if (strlen(date) != 10 || sscanf(date,"%4d-%2d-%2d",&y,&m,&d) != 3 ||
        y < 2020 || y > 2099 || !clock_text(clock)) return 0;
    struct tm t = {.tm_year=y-1900,.tm_mon=m-1,.tm_mday=d,
                   .tm_hour=(clock[0]-'0')*10+clock[1]-'0',.tm_min=(clock[3]-'0')*10+clock[4]-'0'};
    time_t result = mktime(&t);
    return t.tm_mday == d && t.tm_mon == m-1 ? result : 0;
}

static time_t published_time(const char *s) {
    if (strlen(s) != 25 || s[10] != 'T' || strcmp(s+19,"+08:00") != 0) return 0;
    char date[11];
    memcpy(date,s,10);
    date[10] = 0;
    return hkt(date, s+11); // The display shows minute precision.
}

static time_t event_time(const char *status, const char *date, time_t scheduled) {
    // HKIA supplies explicit day/month/year in parentheses for cross-date events.
    for (const char *s = status; *s; ++s) {
        if (!isdigit((unsigned char)*s) || !clock_text(s)) continue;
        int d, m, y;
        char explicit_date[16];
        const char *suffix = strchr(s+5,'(');
        if (suffix && sscanf(suffix,"(%2d/%2d/%4d)",&d,&m,&y) == 3) {
            snprintf(explicit_date,sizeof(explicit_date),"%04d-%02d-%02d",y,m,d);
            date = explicit_date;
        }
        time_t t = hkt(date,s);
        return t ? t : scheduled;
    }
    return scheduled;
}
bool flight_alert(const char *s) {
    return !strncasecmp(s,"cancel",6) || !strncasecmp(s,"delay",5) ||
        !strncasecmp(s,"divert",6) || !strncasecmp(s,"final call",10) ||
        !strncasecmp(s,"gate closed",11);
}
static void routes(char *dst, size_t size, const cJSON *list) {
    const cJSON *item;
    size_t used = 0;
    cJSON_ArrayForEach(item,list) {
        const char *code = string(item);
        if (!*code) continue;
        int added = snprintf(dst+used,size-used,"%s%s",used ? "/" : "",code);
        if (added < 0 || (size_t)added >= size-used) break;
        used += (size_t)added;
    }
    if (!*dst) snprintf(dst,size,"--");
}
static void insert(struct schedule *s, const struct flight *f) {
    // Keep a bounded, stable list even if date buckets arrive out of order.
    int pos = 0;
    while (pos < s->count && s->rows[pos].event <= f->event) ++pos;
    if (pos == FLIGHT_CAP) return;
    if (s->count < FLIGHT_CAP) ++s->count;
    memmove(s->rows+pos+1,s->rows+pos,(s->count-pos-1)*sizeof(*f));
    s->rows[pos] = *f;
}

bool flight_parse(const char *json, size_t length, enum board b, time_t now, struct schedule *out) {
    cJSON *root = cJSON_ParseWithLengthOpts(json,length+1,NULL,true);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return false; }
    struct schedule candidate = {.fetched = now};
    struct tm local;
    localtime_r(&now,&local);
    char today[11];
    strftime(today,sizeof(today),"%Y-%m-%d",&local);
    bool found_today = false, valid = true;
    const cJSON *g;
    cJSON_ArrayForEach(g,root) {
        const cJSON *arr = field(g,"arrival"), *cargo = field(g,"cargo");
        if (!cJSON_IsBool(arr) || !cJSON_IsFalse(cargo) ||
            !!cJSON_IsTrue(arr) != is_arrival(b)) continue;
        const char *date = string(field(g,"date"));
        const cJSON *list = field(g,"list");
        if (!cJSON_IsArray(list) || !hkt(date,"00:00")) { valid = false; break; }
        if (!strcmp(today,date)) {
            found_today = true;
            candidate.published = published_time(string(field(g,"lastUpdatedTime")));
        }
        int usable = 0;
        const cJSON *row;
        cJSON_ArrayForEach(row,list) {
            const char *num = string(field(cJSON_GetArrayItem(field(row,"flight"),0),"no"));
            const char *clock = string(field(row,"time"));
            struct flight f = {.scheduled = hkt(date,clock)};
            if (!*num || !f.scheduled || strlen(clock) != 5) continue;
            ++usable;
            const char *status = string(field(row,"status"));
            f.event = event_time(status,date,f.scheduled);
            time_t minute = now - now % 60;
            bool active = !strncasecmp(status,"board",5) || !strcasecmp(status,"Final Call") ||
                          !strncasecmp(status,"delay",5);
            if (f.event < minute-15*60 && active && f.event >= minute-6*3600) f.event = minute;
            if (f.event < minute-15*60 || f.event > minute+24*3600) continue;
            snprintf(f.number,sizeof(f.number),"%s",num);
            snprintf(f.status,sizeof(f.status),"%s",*status ? status : "Scheduled");
            routes(f.route,sizeof(f.route),field(row,is_arrival(b) ? "origin" : "destination"));
            const char *facility = string(field(row,is_arrival(b) ? "baggage" : "gate"));
            snprintf(f.facility,sizeof(f.facility),"%s",*facility ? facility : "--");
            snprintf(f.terminal,sizeof(f.terminal),"%s",string(field(row,"terminal")));
            insert(&candidate,&f);
        }
        if (cJSON_GetArraySize(list) && !usable) { valid = false; break; }
    }
    cJSON_Delete(root);
    if (!found_today || !valid) return false;
    *out = candidate;
    return true;
}
