#pragma once
#include <stdbool.h>
#include <stdint.h>
enum button { BTN_FRONT = 0, BTN_DOWN = 18, BTN_UP = 39 };
void note4c_init(void);
int note4c_battery_percent(void); // Voltage-based estimate, or -1 if unavailable.
bool note4c_pressed(enum button button);
bool note4c_show(const uint8_t *frame);
