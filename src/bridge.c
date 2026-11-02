#include "ps5_xb360_bridge.h"

#include <string.h>

void ps5_xb360_bridge_init(ps5_xb360_bridge_t *bridge,
                           const ps5_xb360_backend_t *backend) {
    if (bridge == NULL) return;
    memset(bridge, 0, sizeof(*bridge));
    if (backend != NULL) bridge->backend = *backend;
}

bool ps5_xb360_bridge_start_slot(ps5_xb360_bridge_t *bridge, uint8_t slot) {
    uint8_t command[X360_USB_COMMAND_SIZE];
    if (bridge == NULL || slot >= X360_RECEIVER_MAX_PADS ||
        bridge->backend.usb_write == NULL) return false;
    x360_usb_build_inquiry(command);
    return bridge->backend.usb_write(bridge->backend.context, slot, command,
                                     sizeof(command));
}

static void initialize_connected_slot(ps5_xb360_bridge_t *bridge, uint8_t slot) {
    uint8_t command[X360_USB_COMMAND_SIZE];
    if (bridge->backend.usb_write == NULL) return;
    x360_usb_build_ready(command);
    bridge->backend.usb_write(bridge->backend.context, slot, command,
                              sizeof(command));
    x360_usb_build_led(slot, command);
    bridge->backend.usb_write(bridge->backend.context, slot, command,
                              sizeof(command));
}

bool ps5_xb360_bridge_feed(ps5_xb360_bridge_t *bridge, uint8_t slot,
                           const uint8_t *packet, size_t length) {
    x360_event_t event;
    dualsense_state_t ds;
    ps5_xb360_slot_t *target;
    if (bridge == NULL || slot >= X360_RECEIVER_MAX_PADS) return false;
    target = &bridge->slots[slot];
    if (!x360_receiver_parse(packet, length, &target->state, &event)) return false;

    switch (event.type) {
        case X360_EVENT_CONNECTED:
            target->connected = true;
            target->state = event.state;
            initialize_connected_slot(bridge, slot);
            break;
        case X360_EVENT_DISCONNECTED:
            target->connected = false;
            memset(&target->state, 0, sizeof(target->state));
            if (bridge->backend.disconnect != NULL)
                bridge->backend.disconnect(bridge->backend.context, slot);
            break;
        case X360_EVENT_INPUT:
        case X360_EVENT_BATTERY:
            if (!target->connected) initialize_connected_slot(bridge, slot);
            target->connected = true;
            target->state = event.state;
            if (bridge->backend.publish != NULL) {
                dualsense_from_x360(&target->state, &ds);
                bridge->backend.publish(bridge->backend.context, slot, &ds);
            }
            break;
        case X360_EVENT_NONE:
        default:
            break;
    }
    return true;
}

bool ps5_xb360_bridge_set_rumble(ps5_xb360_bridge_t *bridge, uint8_t slot,
                                 uint8_t strong, uint8_t weak) {
    uint8_t command[X360_USB_COMMAND_SIZE];
    if (bridge == NULL || slot >= X360_RECEIVER_MAX_PADS ||
        bridge->backend.usb_write == NULL) return false;
    x360_usb_build_rumble(strong, weak, command);
    return bridge->backend.usb_write(bridge->backend.context, slot, command,
                                     sizeof(command));
}
