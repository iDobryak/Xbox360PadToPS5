#ifndef TEST_PS5_USB_IOCTL_H
#define TEST_PS5_USB_IOCTL_H

#include <stdint.h>

#define USB_FS_FLAG_SINGLE_SHORT_OK 1u
#define USB_FS_INIT 0x1001u
#define USB_FS_OPEN 0x1002u
#define USB_FS_START 0x1003u
#define USB_FS_COMPLETE 0x1004u
#define USB_FS_UNINIT 0x1005u
#define USB_GET_FULL_DESC 0x1006u
#define USB_GET_DEVICE_DESC 0x1007u
#define USB_SET_CONFIG 0x1008u
#define USB_CLAIM_INTERFACE 0x1009u

struct usb_fs_endpoint {
    void **ppBuffer;
    uint32_t *pLength;
    uint32_t nFrames;
    uint32_t aFrames;
    uint32_t flags;
    uint32_t timeout;
    int status;
};

struct usb_fs_init {
    struct usb_fs_endpoint *pEndpoints;
    uint8_t ep_index_max;
};

struct usb_fs_open {
    uint32_t max_bufsize;
    uint32_t max_frames;
    uint8_t ep_index;
    uint8_t ep_no;
};

struct usb_fs_start { uint8_t ep_index; };
struct usb_fs_complete { uint8_t ep_index; };
struct usb_fs_uninit { uint8_t reserved; };

struct usb_gen_descriptor {
    void *ugd_data;
    uint16_t ugd_maxlen;
    uint8_t ugd_config_index;
    uint16_t ugd_actlen;
};

#endif
