#include "dualsense_mapper.h"

#include <string.h>

static uint8_t axis_to_u8(int16_t value) {
    uint32_t shifted = (uint32_t)((int32_t)value + 32768);
    return (uint8_t)((shifted * 255u + 32767u) / 65535u);
}

static uint8_t axis_y_to_u8(int16_t value) {
    return (uint8_t)(255u - axis_to_u8(value));
}

static ds_dpad_t map_dpad(uint16_t buttons) {
    const bool up = (buttons & X360_BTN_DPAD_UP) != 0u;
    const bool down = (buttons & X360_BTN_DPAD_DOWN) != 0u;
    const bool left = (buttons & X360_BTN_DPAD_LEFT) != 0u;
    const bool right = (buttons & X360_BTN_DPAD_RIGHT) != 0u;

    if (up && right && !down && !left) return DS_DPAD_UP_RIGHT;
    if (down && right && !up && !left) return DS_DPAD_DOWN_RIGHT;
    if (down && left && !up && !right) return DS_DPAD_DOWN_LEFT;
    if (up && left && !down && !right) return DS_DPAD_UP_LEFT;
    if (up && !down) return DS_DPAD_UP;
    if (down && !up) return DS_DPAD_DOWN;
    if (right && !left) return DS_DPAD_RIGHT;
    if (left && !right) return DS_DPAD_LEFT;
    return DS_DPAD_NEUTRAL;
}

void dualsense_from_x360(const x360_state_t *source, dualsense_state_t *target) {
    uint16_t result = 0u;
    if (source == NULL || target == NULL) return;

    memset(target, 0, sizeof(*target));
    if (source->buttons & X360_BTN_X) result |= DS_BTN_SQUARE;
    if (source->buttons & X360_BTN_A) result |= DS_BTN_CROSS;
    if (source->buttons & X360_BTN_B) result |= DS_BTN_CIRCLE;
    if (source->buttons & X360_BTN_Y) result |= DS_BTN_TRIANGLE;
    if (source->buttons & X360_BTN_LB) result |= DS_BTN_L1;
    if (source->buttons & X360_BTN_RB) result |= DS_BTN_R1;
    if (source->buttons & X360_BTN_L3) result |= DS_BTN_L3;
    if (source->buttons & X360_BTN_R3) result |= DS_BTN_R3;
    if (source->buttons & X360_BTN_BACK) result |= DS_BTN_CREATE;
    if (source->buttons & X360_BTN_START) result |= DS_BTN_OPTIONS;
    if (source->buttons & X360_BTN_GUIDE) result |= DS_BTN_PS;

    target->buttons = result;
    target->dpad = map_dpad(source->buttons);
    target->left_x = axis_to_u8(source->left_x);
    target->left_y = axis_y_to_u8(source->left_y);
    target->right_x = axis_to_u8(source->right_x);
    target->right_y = axis_y_to_u8(source->right_y);
    target->l2 = source->left_trigger;
    target->r2 = source->right_trigger;
    target->battery_level = source->battery_level;
}
