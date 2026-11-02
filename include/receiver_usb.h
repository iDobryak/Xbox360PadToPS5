#ifndef PS5_XB360_RECEIVER_USB_H
#define PS5_XB360_RECEIVER_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X360_USB_VENDOR_MICROSOFT 0x045eu
#define X360_USB_PRODUCT_RECEIVER 0x0719u
#define X360_USB_PRODUCT_RECEIVER_THIRDPARTY 0x0291u
#define X360_USB_PRODUCT_RECEIVER_02A9 0x02a9u
#define X360_USB_COMMAND_SIZE 12u

bool x360_usb_is_receiver(uint16_t vendor_id, uint16_t product_id);
bool x360_usb_is_gamepad_interface(uint8_t interface_class,
                                   uint8_t interface_subclass,
                                   uint8_t interface_protocol);

/* Each controller is exposed through a vendor-specific interface. The USB
 * backend should claim up to four matching interfaces and feed their IN
 * packets to the corresponding bridge slot. */
void x360_usb_build_inquiry(uint8_t command[X360_USB_COMMAND_SIZE]);
void x360_usb_build_ready(uint8_t command[X360_USB_COMMAND_SIZE]);
void x360_usb_build_led(uint8_t slot, uint8_t command[X360_USB_COMMAND_SIZE]);
void x360_usb_build_rumble(uint8_t strong, uint8_t weak,
                           uint8_t command[X360_USB_COMMAND_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
