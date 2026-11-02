#ifndef PS5_XB360_PS5_VPAD_H
#define PS5_XB360_PS5_VPAD_H

#include "dualsense_mapper.h"

#include <stdint.h>

typedef struct ps5_vpad ps5_vpad_t;

enum ps5_vpad_submit_result {
    PS5_VPAD_SUBMIT_FAILED = 0,
    PS5_VPAD_SUBMIT_DELIVERED = 1,
    PS5_VPAD_SUBMIT_COALESCED = 2
};

/* Tries the libScePad/MBus backend needed for the system PS button. If it is
 * unavailable, open/submit continue through the proven direct /dev/hid path. */
int ps5_vpad_system_init(char *why, int why_length);
void ps5_vpad_system_poll(void);
void ps5_vpad_system_close(void);

ps5_vpad_t *ps5_vpad_open(unsigned slot, char *why, int why_length);
enum ps5_vpad_submit_result ps5_vpad_submit(
    ps5_vpad_t *pad, const dualsense_state_t *state);
int ps5_vpad_press_ps(ps5_vpad_t *pad, int milliseconds);
int ps5_vpad_is_ready(const ps5_vpad_t *pad);
int ps5_vpad_has_ps(const ps5_vpad_t *pad);
void ps5_vpad_close(ps5_vpad_t *pad);

#endif
