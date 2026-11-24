#pragma once
#include <stdbool.h>
#include <time.h>

enum weather_series { AIR_TEMPERATURE, HUMIDITY, WIND, WEATHER_SERIES_COUNT };
struct observation {
    time_t at;
    char value[10], direction[24], gust[10];
};
struct airport_weather {
    struct observation readings[WEATHER_SERIES_COUNT];
    bool failed[WEATHER_SERIES_COUNT];
};
// Select the airport row from an English HKO CSV. The input buffer is modified.
// Preserve the previous reading if the report is invalid or older than the cache.
bool weather_parse(char *csv,enum weather_series series,struct observation *out);
