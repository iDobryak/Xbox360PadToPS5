#ifndef PS5_XB360_RECEIVER_H
#define PS5_XB360_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X360_RECEIVER_MAX_PADS 4u
#define X360_RECEIVER_PACKET_SIZE 32u

typedef enum x360_button {
    X360_BTN_DPAD_UP    = 1u << 0,
    X360_BTN_DPAD_DOWN  = 1u << 1,
    X360_BTN_DPAD_LEFT  = 1u << 2,
    X360_BTN_DPAD_RIGHT = 1u << 3,
    X360_BTN_START      = 1u << 4,
    X360_BTN_BACK       = 1u << 5,
    X360_BTN_L3         = 1u << 6,
    X360_BTN_R3         = 1u << 7,
    X360_BTN_LB         = 1u << 8,
    X360_BTN_RB         = 1u << 9,
    X360_BTN_GUIDE      = 1u << 10,
    X360_BTN_A          = 1u << 12,
    X360_BTN_B          = 1u << 13,
    X360_BTN_X          = 1u << 14,
    X360_BTN_Y          = 1u << 15
} x360_button_t;

typedef struct x360_state {
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    uint8_t battery_level;
} x360_state_t;

typedef enum x360_event_type {
    X360_EVENT_NONE = 0,
    X360_EVENT_CONNECTED,
    X360_EVENT_DISCONNECTED,
    X360_EVENT_INPUT,
    X360_EVENT_BATTERY
} x360_event_type_t;

typedef struct x360_event {
    x360_event_type_t type;
    x360_state_t state;
} x360_event_t;

/* Parse one interrupt-IN packet belonging to a receiver slot/interface. */
bool x360_receiver_parse(const uint8_t *packet, size_t length,
                         const x360_state_t *previous, x360_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
