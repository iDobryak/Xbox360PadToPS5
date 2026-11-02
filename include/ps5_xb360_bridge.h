#ifndef PS5_XB360_BRIDGE_H
#define PS5_XB360_BRIDGE_H

#include "dualsense_mapper.h"
#include "receiver_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*ps5_xb360_usb_write_fn)(void *context, uint8_t slot,
                                       const uint8_t *data, size_t length);
typedef void (*ps5_xb360_publish_fn)(void *context, uint8_t slot,
                                     const dualsense_state_t *state);
typedef void (*ps5_xb360_disconnect_fn)(void *context, uint8_t slot);

typedef struct ps5_xb360_backend {
    void *context;
    ps5_xb360_usb_write_fn usb_write;
    ps5_xb360_publish_fn publish;
    ps5_xb360_disconnect_fn disconnect;
} ps5_xb360_backend_t;

typedef struct ps5_xb360_slot {
    bool connected;
    x360_state_t state;
} ps5_xb360_slot_t;

typedef struct ps5_xb360_bridge {
    ps5_xb360_backend_t backend;
    ps5_xb360_slot_t slots[X360_RECEIVER_MAX_PADS];
} ps5_xb360_bridge_t;

void ps5_xb360_bridge_init(ps5_xb360_bridge_t *bridge,
                           const ps5_xb360_backend_t *backend);
bool ps5_xb360_bridge_start_slot(ps5_xb360_bridge_t *bridge, uint8_t slot);
bool ps5_xb360_bridge_feed(ps5_xb360_bridge_t *bridge, uint8_t slot,
                           const uint8_t *packet, size_t length);
bool ps5_xb360_bridge_set_rumble(ps5_xb360_bridge_t *bridge, uint8_t slot,
                                 uint8_t strong, uint8_t weak);

#ifdef __cplusplus
}
#endif

#endif
