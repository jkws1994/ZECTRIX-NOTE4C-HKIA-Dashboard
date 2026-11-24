#pragma once
#include "flights.h"
#include "nearby.h"
#include "weather.h"
#include "esp_wifi.h"

void network_init(void);
bool network_load(wifi_config_t *cfg);
void network_station(const wifi_config_t *cfg);
// Starts a temporary WPA2 setup network, and returns its on-screen credentials.
void network_setup(char ssid[33], char password[17]);
bool network_scan(struct nearby *out);
// NULL means success. On failure the previous schedule is retained.
const char *network_fetch(enum board b, struct schedule *out);
const char *network_weather(struct airport_weather *out);
