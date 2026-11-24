#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

enum board { PAX_ARR, PAX_DEP, BOARD_COUNT };
#define FLIGHT_CAP 30
#define ROWS 5

static inline bool is_arrival(enum board b) { return b == PAX_ARR; }

struct flight {
    time_t scheduled;
    time_t event;
    char number[16], route[32], status[64], facility[16], terminal[12];
};
struct schedule {
    struct flight rows[FLIGHT_CAP];
    int count;
    time_t published;
    time_t fetched;
};
// Caller sets TZ=HKT-8. Pass a NUL-terminated response and its byte length excluding NUL.
// A failed parse leaves the caller's schedule untouched.
bool flight_parse(const char *json, size_t length, enum board b, time_t now, struct schedule *out);
bool flight_alert(const char *status);
