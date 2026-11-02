#ifndef PS5_XB360_DUALSENSE_MAPPER_H
#define PS5_XB360_DUALSENSE_MAPPER_H

#include "x360_receiver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ds_button {
    DS_BTN_SQUARE  = 1u << 0,
    DS_BTN_CROSS   = 1u << 1,
    DS_BTN_CIRCLE  = 1u << 2,
    DS_BTN_TRIANGLE= 1u << 3,
    DS_BTN_L1      = 1u << 4,
    DS_BTN_R1      = 1u << 5,
    DS_BTN_L3      = 1u << 6,
    DS_BTN_R3      = 1u << 7,
    DS_BTN_CREATE  = 1u << 8,
    DS_BTN_OPTIONS = 1u << 9,
    DS_BTN_PS      = 1u << 10
} ds_button_t;

typedef enum ds_dpad {
    DS_DPAD_UP = 0,
    DS_DPAD_UP_RIGHT = 1,
    DS_DPAD_RIGHT = 2,
    DS_DPAD_DOWN_RIGHT = 3,
    DS_DPAD_DOWN = 4,
    DS_DPAD_DOWN_LEFT = 5,
    DS_DPAD_LEFT = 6,
    DS_DPAD_UP_LEFT = 7,
    DS_DPAD_NEUTRAL = 8
} ds_dpad_t;

/* Backend-neutral DualSense state. A PS5 hook should translate this structure
 * to the exact structure expected by the chosen pad-injection API. */
typedef struct dualsense_state {
    uint16_t buttons;
    ds_dpad_t dpad;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
    uint8_t l2;
    uint8_t r2;
    uint8_t battery_level;
} dualsense_state_t;

void dualsense_from_x360(const x360_state_t *source, dualsense_state_t *target);

#ifdef __cplusplus
}
#endif

#endif
