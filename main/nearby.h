#pragma once
#include <stdint.h>

struct access_point {
    uint8_t ssid[33];
    const char *security;
    int rssi;
};

struct nearby {
    struct access_point *items;
    int count;
};
