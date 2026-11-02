#ifndef PS5_XB360_WEB_H
#define PS5_XB360_WEB_H

#include <stdbool.h>
#include <stdint.h>

#include "ps5_telemetry.h"
#include "x360_receiver.h"

typedef struct ps5_web ps5_web_t;

typedef struct ps5_web_slot_status {
    bool connected;
    bool ps_available;
    unsigned long reports;
    ps5_telemetry_slot_snapshot_t telemetry;
} ps5_web_slot_status_t;

typedef struct ps5_web_status {
    int pid;
    unsigned long uptime_seconds;
    const char *receiver_node;
    unsigned receiver_vendor;
    unsigned receiver_product;
    unsigned slot_count;
    unsigned long usb_errors;
    const char *last_usb_error;
    ps5_telemetry_capture_snapshot_t capture;
    ps5_web_slot_status_t slots[X360_RECEIVER_MAX_PADS];
} ps5_web_status_t;

enum {
    PS5_WEB_ACTION_NONE = 0,
    PS5_WEB_ACTION_INQUIRY = 1,
    PS5_WEB_ACTION_STOP = 2,
    PS5_WEB_ACTION_PS_SLOT_1 = 10,
    PS5_WEB_ACTION_TELEMETRY_START = 20
};

#define PS5_WEB_ACTION_PS_SLOT(slot) \
    (PS5_WEB_ACTION_PS_SLOT_1 + (int)(slot))

ps5_web_t *ps5_web_open(uint16_t port, char *why, int why_length);
int ps5_web_poll(ps5_web_t *web, const ps5_web_status_t *status);
void ps5_web_close(ps5_web_t *web);

#endif
