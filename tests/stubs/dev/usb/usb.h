#ifndef TEST_PS5_USB_H
#define TEST_PS5_USB_H

#include <stdint.h>

#define UDESC_INTERFACE 4u
#define UDESC_ENDPOINT 5u
#define UE_DIR_IN 0x80u
#define UE_XFERTYPE 0x03u
#define UE_INTERRUPT 0x03u

struct usb_device_descriptor {
    uint8_t padding[8];
    uint16_t idVendor;
    uint16_t idProduct;
};

#endif
