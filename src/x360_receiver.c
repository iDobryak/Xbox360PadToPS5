#include "x360_receiver.h"

#include <string.h>

static int16_t read_i16_le(const uint8_t *value) {
    return (int16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
}

bool x360_receiver_parse(const uint8_t *packet, size_t length,
                         const x360_state_t *previous, x360_event_t *event) {
    const uint8_t *payload;

    if (packet == NULL || event == NULL || length < 2u) {
        return false;
    }

    memset(event, 0, sizeof(*event));
    if (previous != NULL) {
        event->state = *previous;
    }

    /* Wireless status frame: 08 80 connects, 08 00 disconnects. */
    if (packet[0] == 0x08u) {
        event->type = packet[1] == 0x80u
                          ? X360_EVENT_CONNECTED
                          : X360_EVENT_DISCONNECTED;
        if (event->type == X360_EVENT_DISCONNECTED) {
            memset(&event->state, 0, sizeof(event->state));
        }
        return true;
    }

    /* Controller input is 00 01 xx F0 xx 13 followed by the ordinary
     * 14-byte Xbox 360 state, beginning with the button word at byte 6. */
    if (packet[0] == 0x00u && packet[1] == 0x01u) {
        if (length < 18u) return false;
        /* Longer 00 01 radio/service notifications are valid but do not
         * contain controller state. */
        if (packet[3] != 0xf0u || packet[5] != 0x13u) return true;
        payload = packet + 6u;
        event->state.buttons = (uint16_t)payload[0] |
                               ((uint16_t)payload[1] << 8);
        event->state.left_trigger = payload[2];
        event->state.right_trigger = payload[3];
        event->state.left_x = read_i16_le(payload + 4u);
        event->state.left_y = read_i16_le(payload + 6u);
        event->state.right_x = read_i16_le(payload + 8u);
        event->state.right_y = read_i16_le(payload + 10u);
        event->type = X360_EVENT_INPUT;
        return true;
    }

    /* Battery messages are short radio frames. The high two bits encode the
     * coarse four-level battery value used by wireless Xbox 360 pads. */
    if (length >= 4u && packet[1] == 0x0fu && packet[2] == 0x00u) {
        event->state.battery_level = (uint8_t)((packet[3] >> 6) * 33u);
        event->type = X360_EVENT_BATTERY;
        return true;
    }

    return true;
}
