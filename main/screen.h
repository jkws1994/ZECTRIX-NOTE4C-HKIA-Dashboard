#pragma once
#include "flights.h"
#include "nearby.h"
#include "weather.h"
#include <stdint.h>
#define SCREEN_W 400
#define SCREEN_H 300
#define FRAME_BYTES (SCREEN_W * SCREEN_H / 4)

enum page { PAGE_DEPARTURES, PAGE_ARRIVALS, PAGE_MAPS, PAGE_WEATHER, PAGE_COUNT };
enum airport_map { MAP_GATES, MAP_TRANSPORT };
void screen_battery(uint8_t *fb,int percent);

void screen_board(uint8_t *fb, enum board b, const struct schedule *s, int first,
                  const char *error, time_t now, time_t footer_stamp);
void screen_map(uint8_t *fb, enum airport_map map);
void screen_weather(uint8_t *fb,const struct airport_weather *weather,const char *error,time_t now);
void screen_setup(uint8_t *fb, const char *ssid, const char *password);
enum { NETWORK_ROWS=5 };
void screen_networks(uint8_t *fb,const struct nearby *list,int first,bool failed);
