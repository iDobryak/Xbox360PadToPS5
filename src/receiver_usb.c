#include "receiver_usb.h"

#include <string.h>

bool x360_usb_is_receiver(uint16_t vendor_id, uint16_t product_id) {
    return vendor_id == X360_USB_VENDOR_MICROSOFT &&
           (product_id == X360_USB_PRODUCT_RECEIVER ||
            product_id == X360_USB_PRODUCT_RECEIVER_THIRDPARTY ||
            product_id == X360_USB_PRODUCT_RECEIVER_02A9);
}

bool x360_usb_is_gamepad_interface(uint8_t interface_class,
                                   uint8_t interface_subclass,
                                   uint8_t interface_protocol) {
    return interface_class == 0xffu && interface_subclass == 0x5du &&
           interface_protocol == 0x81u;
}

void x360_usb_build_inquiry(uint8_t command[X360_USB_COMMAND_SIZE]) {
    static const uint8_t inquiry[X360_USB_COMMAND_SIZE] = {
        0x08u, 0x00u, 0x0fu, 0xc0u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };
    if (command != NULL) memcpy(command, inquiry, sizeof(inquiry));
}

void x360_usb_build_ready(uint8_t command[X360_USB_COMMAND_SIZE]) {
    if (command == NULL) return;
    memset(command, 0, X360_USB_COMMAND_SIZE);
    command[2] = 0x02u;
    command[3] = 0x80u;
}

void x360_usb_build_led(uint8_t slot, uint8_t command[X360_USB_COMMAND_SIZE]) {
    if (command == NULL) return;
    memset(command, 0, X360_USB_COMMAND_SIZE);
    command[2] = 0x08u;
    /* 2..5 are the four "blink, then stay on" quadrant patterns. */
    command[3] = (uint8_t)(0x40u | (0x02u + (slot & 0x03u)));
}

void x360_usb_build_rumble(uint8_t strong, uint8_t weak,
                           uint8_t command[X360_USB_COMMAND_SIZE]) {
    static const uint8_t header[X360_USB_COMMAND_SIZE] = {
        0x00u, 0x01u, 0x0fu, 0xc0u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };
    if (command == NULL) return;
    memcpy(command, header, sizeof(header));
    command[5] = strong;
    command[6] = weak;
}
