#include "ps5_xb360_bridge.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    unsigned writes;
    unsigned publishes;
    unsigned disconnects;
    uint8_t last_slot;
    uint8_t last_write[X360_USB_COMMAND_SIZE];
    dualsense_state_t last_state;
} fixture_t;

static bool usb_write(void *context, uint8_t slot, const uint8_t *data,
                      size_t length) {
    fixture_t *f = context;
    assert(length == X360_USB_COMMAND_SIZE);
    f->writes++;
    f->last_slot = slot;
    memcpy(f->last_write, data, length);
    return true;
}

static void publish(void *context, uint8_t slot,
                    const dualsense_state_t *state) {
    fixture_t *f = context;
    f->publishes++;
    f->last_slot = slot;
    f->last_state = *state;
}

static void disconnect_pad(void *context, uint8_t slot) {
    fixture_t *f = context;
    f->disconnects++;
    f->last_slot = slot;
}

static void test_mapping(void) {
    x360_state_t x = {0};
    dualsense_state_t ds;
    x.buttons = X360_BTN_A | X360_BTN_X | X360_BTN_LB |
                X360_BTN_START | X360_BTN_GUIDE |
                X360_BTN_DPAD_UP | X360_BTN_DPAD_RIGHT;
    x.left_x = -32768;
    x.left_y = 32767;
    x.right_x = 32767;
    x.right_y = -32768;
    x.left_trigger = 17u;
    x.right_trigger = 231u;
    dualsense_from_x360(&x, &ds);
    assert((ds.buttons & DS_BTN_CROSS) != 0u);
    assert((ds.buttons & DS_BTN_SQUARE) != 0u);
    assert((ds.buttons & DS_BTN_L1) != 0u);
    assert((ds.buttons & DS_BTN_OPTIONS) != 0u);
    assert((ds.buttons & DS_BTN_PS) != 0u);
    assert(ds.dpad == DS_DPAD_UP_RIGHT);
    assert(ds.left_x == 0u && ds.left_y == 0u);
    assert(ds.right_x == 255u && ds.right_y == 255u);
    assert(ds.l2 == 17u && ds.r2 == 231u);
}

static void test_wireless_flow(void) {
    fixture_t f = {0};
    ps5_xb360_backend_t backend = {&f, usb_write, publish, disconnect_pad};
    ps5_xb360_bridge_t bridge;
    uint8_t connected[2] = {0x08u, 0x80u};
    uint8_t disconnected[2] = {0x08u, 0x00u};
    uint8_t input[32] = {0};

    ps5_xb360_bridge_init(&bridge, &backend);
    assert(ps5_xb360_bridge_start_slot(&bridge, 2u));
    assert(f.writes == 1u && f.last_slot == 2u);
    assert(f.last_write[0] == 0x08u && f.last_write[2] == 0x0fu &&
           f.last_write[3] == 0xc0u);

    assert(ps5_xb360_bridge_feed(&bridge, 2u, connected, sizeof(connected)));
    assert(bridge.slots[2].connected);
    assert(f.writes == 3u);
    assert(f.last_write[2] == 0x08u && f.last_write[3] == 0x44u);

    input[1] = 0x01u;
    input[3] = 0xf0u;
    input[5] = 0x13u;
    input[6] = X360_BTN_DPAD_LEFT | X360_BTN_BACK;
    input[7] = (uint8_t)((X360_BTN_B | X360_BTN_RB) >> 8);
    input[8] = 100u;
    input[9] = 200u;
    input[10] = 0x00u; input[11] = 0x80u;
    input[12] = 0xffu; input[13] = 0x7fu;
    input[14] = 0x00u; input[15] = 0x00u;
    input[16] = 0x00u; input[17] = 0x00u;
    assert(ps5_xb360_bridge_feed(&bridge, 2u, input, sizeof(input)));
    assert(f.publishes == 1u);
    assert(f.last_state.dpad == DS_DPAD_LEFT);
    assert((f.last_state.buttons & DS_BTN_CIRCLE) != 0u);
    assert((f.last_state.buttons & DS_BTN_R1) != 0u);
    assert((f.last_state.buttons & DS_BTN_CREATE) != 0u);
    assert(f.last_state.l2 == 100u && f.last_state.r2 == 200u);

    assert(ps5_xb360_bridge_set_rumble(&bridge, 2u, 0x55u, 0xaau));
    assert(f.last_write[5] == 0x55u && f.last_write[6] == 0xaau);

    assert(ps5_xb360_bridge_feed(&bridge, 2u, disconnected,
                                 sizeof(disconnected)));
    assert(!bridge.slots[2].connected);
    assert(f.disconnects == 1u);
}

static void test_rejects_truncated_input(void) {
    uint8_t packet[8] = {0};
    uint8_t service[29] = {0};
    x360_event_t event;
    packet[1] = 0x01u;
    assert(!x360_receiver_parse(packet, sizeof(packet), NULL, &event));
    service[1] = 0x01u;
    assert(x360_receiver_parse(service, sizeof(service), NULL, &event));
    assert(event.type == X360_EVENT_NONE);
}

static void test_receiver_commands(void) {
    uint8_t command[X360_USB_COMMAND_SIZE];
    assert(x360_usb_is_receiver(0x045eu, 0x0719u));
    assert(x360_usb_is_receiver(0x045eu, 0x02a9u));
    assert(!x360_usb_is_receiver(0x054cu, 0x0ce6u));
    assert(x360_usb_is_gamepad_interface(0xffu, 0x5du, 0x81u));

    x360_usb_build_inquiry(command);
    assert(command[0] == 0x08u && command[1] == 0u);
    assert(command[2] == 0x0fu && command[3] == 0xc0u);

    x360_usb_build_ready(command);
    assert(command[0] == 0u && command[1] == 0u);
    assert(command[2] == 0x02u && command[3] == 0x80u);

    x360_usb_build_led(3u, command);
    assert(command[2] == 0x08u && command[3] == 0x45u);

    x360_usb_build_rumble(0x12u, 0x34u, command);
    assert(command[0] == 0u && command[1] == 0x01u);
    assert(command[2] == 0x0fu && command[3] == 0xc0u);
    assert(command[5] == 0x12u && command[6] == 0x34u);
}

int main(void) {
    test_mapping();
    test_wireless_flow();
    test_rejects_truncated_input();
    test_receiver_commands();
    puts("ps5_xb360 tests: OK");
    return 0;
}
