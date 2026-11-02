/*
 * Xbox360PadToPS5 receiver transport.
 *
 * The PS5 USB_FS transfer lifecycle was adapted in 2026 from FGG-XSense
 * by FGG STORE (GPL-3.0). See NOTICE.md for the upstream link and details.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "ps5_receiver.h"
#include "receiver_usb.h"
#include "x360_receiver.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PS5_RX_MAX_ENDPOINTS (X360_RECEIVER_MAX_PADS * 2u)
#define PS5_RX_TRANSFER_SIZE 64u
#define PS5_RX_QUEUE_DEPTH 4u
#define PS5_RX_TIMEOUT_MS 50u

typedef struct receiver_slot {
    int interface_number;
    int in_index;
    int out_index;
    int max_packet;
} receiver_slot_t;

struct ps5_receiver {
    int fd;
    char node[64];
    unsigned vendor;
    unsigned product;
    unsigned slot_count;
    unsigned endpoint_count;
    receiver_slot_t slot[X360_RECEIVER_MAX_PADS];
    struct usb_fs_endpoint endpoint[PS5_RX_MAX_ENDPOINTS];
    void *buffer_pointer[PS5_RX_MAX_ENDPOINTS][1];
    uint32_t buffer_length[PS5_RX_MAX_ENDPOINTS][1];
    uint8_t data[PS5_RX_MAX_ENDPOINTS][PS5_RX_TRANSFER_SIZE];
    int pending[PS5_RX_MAX_ENDPOINTS];
    int endpoint_slot[PS5_RX_MAX_ENDPOINTS];
    int endpoint_is_input[PS5_RX_MAX_ENDPOINTS];
    uint8_t endpoint_address[PS5_RX_MAX_ENDPOINTS];
    uint8_t queued_data[X360_RECEIVER_MAX_PADS][PS5_RX_QUEUE_DEPTH]
                       [PS5_RX_TRANSFER_SIZE];
    uint8_t queued_length[X360_RECEIVER_MAX_PADS][PS5_RX_QUEUE_DEPTH];
    uint8_t queue_head[X360_RECEIVER_MAX_PADS];
    uint8_t queue_count[X360_RECEIVER_MAX_PADS];
    char last_error[160];
    int last_error_transient;
};

static struct ps5_receiver g_receiver;

static unsigned read_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void reset_layout(ps5_receiver_t *receiver) {
    unsigned i;
    receiver->slot_count = 0u;
    receiver->endpoint_count = 0u;
    for (i = 0u; i < X360_RECEIVER_MAX_PADS; ++i) {
        receiver->slot[i].interface_number = -1;
        receiver->slot[i].in_index = -1;
        receiver->slot[i].out_index = -1;
        receiver->slot[i].max_packet = 0;
    }
}

static int add_endpoint(ps5_receiver_t *receiver, unsigned slot,
                        unsigned address, unsigned max_packet) {
    unsigned index;
    if (receiver->endpoint_count >= PS5_RX_MAX_ENDPOINTS) return 0;
    index = receiver->endpoint_count++;
    receiver->endpoint_slot[index] = (int)slot;
    receiver->endpoint_is_input[index] = (address & UE_DIR_IN) != 0u;
    receiver->endpoint_address[index] = (uint8_t)address;
    if (receiver->endpoint_is_input[index]) {
        if (receiver->slot[slot].in_index >= 0) return 1;
        receiver->slot[slot].in_index = (int)index;
        receiver->slot[slot].max_packet = (int)max_packet;
    } else {
        if (receiver->slot[slot].out_index >= 0) return 1;
        receiver->slot[slot].out_index = (int)index;
    }
    return 1;
}

static int scan_layout(ps5_receiver_t *receiver, const unsigned char *data,
                       int length) {
    int offset = 0;
    int current_slot = -1;
    reset_layout(receiver);

    while (offset + 2 <= length) {
        unsigned descriptor_length = data[offset];
        unsigned descriptor_type = data[offset + 1];
        if (descriptor_length == 0u || offset + (int)descriptor_length > length)
            break;

        if (descriptor_type == UDESC_INTERFACE && descriptor_length >= 9u) {
            current_slot = -1;
            if (data[offset + 3] == 0u &&
                x360_usb_is_gamepad_interface(data[offset + 5],
                                               data[offset + 6],
                                               data[offset + 7]) &&
                receiver->slot_count < X360_RECEIVER_MAX_PADS) {
                current_slot = (int)receiver->slot_count++;
                receiver->slot[current_slot].interface_number = data[offset + 2];
            }
        } else if (descriptor_type == UDESC_ENDPOINT &&
                   descriptor_length >= 7u && current_slot >= 0) {
            unsigned address = data[offset + 2];
            unsigned attributes = data[offset + 3];
            unsigned max_packet = read_u16(data + offset + 4);
            if ((attributes & UE_XFERTYPE) == UE_INTERRUPT &&
                !add_endpoint(receiver, (unsigned)current_slot, address,
                              max_packet)) return 0;
        }
        offset += (int)descriptor_length;
    }

    if (receiver->slot_count == 0u) return 0;
    for (unsigned i = 0u; i < receiver->slot_count; ++i)
        if (receiver->slot[i].in_index < 0) return 0;
    return 1;
}

static int read_descriptors(int fd, unsigned char *data, int capacity,
                            int *actual_length) {
    struct usb_gen_descriptor request;
    memset(&request, 0, sizeof(request));
    request.ugd_data = data;
    request.ugd_maxlen = (uint16_t)capacity;
    request.ugd_config_index = 0;
    if (ioctl(fd, USB_GET_FULL_DESC, &request) != 0 ||
        request.ugd_actlen == 0u) return 0;
    *actual_length = (int)request.ugd_actlen;
    return 1;
}

static int setup_transfers(ps5_receiver_t *receiver) {
    struct usb_fs_init init;
    struct usb_fs_open open_request;
    unsigned i;

    memset(receiver->endpoint, 0, sizeof(receiver->endpoint));
    memset(receiver->pending, 0, sizeof(receiver->pending));
    for (i = 0u; i < receiver->endpoint_count; ++i) {
        receiver->buffer_pointer[i][0] = receiver->data[i];
        receiver->buffer_length[i][0] = PS5_RX_TRANSFER_SIZE;
        receiver->endpoint[i].ppBuffer = receiver->buffer_pointer[i];
        receiver->endpoint[i].pLength = receiver->buffer_length[i];
        receiver->endpoint[i].nFrames = 1;
        receiver->endpoint[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
        /* Short completions keep the single-threaded web UI responsive while
         * leaving one transfer pending per controller slot. */
        receiver->endpoint[i].timeout = PS5_RX_TIMEOUT_MS;
    }

    memset(&init, 0, sizeof(init));
    init.pEndpoints = receiver->endpoint;
    init.ep_index_max = (uint8_t)receiver->endpoint_count;
    if (ioctl(receiver->fd, USB_FS_INIT, &init) != 0) return 0;

    for (i = 0u; i < receiver->endpoint_count; ++i) {
        memset(&open_request, 0, sizeof(open_request));
        open_request.max_bufsize = PS5_RX_TRANSFER_SIZE;
        open_request.max_frames = 1;
        open_request.ep_index = (uint8_t)i;
        open_request.ep_no = receiver->endpoint_address[i];
        if (ioctl(receiver->fd, USB_FS_OPEN, &open_request) != 0) return 0;
    }
    return 1;
}

static int start_transfer(ps5_receiver_t *receiver, unsigned index,
                          unsigned length) {
    struct usb_fs_start start;
    if (index >= receiver->endpoint_count) return 0;
    if (receiver->pending[index]) return 1;
    if (length > PS5_RX_TRANSFER_SIZE) return 0;
    receiver->buffer_length[index][0] = length;
    receiver->endpoint[index].nFrames = 1;
    receiver->endpoint[index].aFrames = 0;
    receiver->endpoint[index].status = 0;
    memset(&start, 0, sizeof(start));
    start.ep_index = (uint8_t)index;
    if (ioctl(receiver->fd, USB_FS_START, &start) != 0) {
        receiver->last_error_transient =
            errno == EBUSY || errno == EAGAIN || errno == EWOULDBLOCK;
        snprintf(receiver->last_error, sizeof(receiver->last_error),
                 "USB_FS_START index %u endpoint %02X: %s", index,
                 receiver->endpoint_address[index], strerror(errno));
        return 0;
    }
    receiver->pending[index] = 1;
    receiver->last_error_transient = 0;
    return 1;
}

static void start_next_output(ps5_receiver_t *receiver, unsigned slot) {
    unsigned head;
    int index;
    if (slot >= receiver->slot_count || receiver->queue_count[slot] == 0u)
        return;
    index = receiver->slot[slot].out_index;
    if (index < 0 || receiver->pending[index]) return;
    head = receiver->queue_head[slot];
    memcpy(receiver->data[index], receiver->queued_data[slot][head],
           receiver->queued_length[slot][head]);
    if (!start_transfer(receiver, (unsigned)index,
                        receiver->queued_length[slot][head])) return;
    receiver->queue_head[slot] =
        (uint8_t)((head + 1u) % PS5_RX_QUEUE_DEPTH);
    receiver->queue_count[slot]--;
}

ps5_receiver_t *ps5_receiver_open(char *why, int why_length) {
    ps5_receiver_t *receiver = &g_receiver;
    unsigned char descriptors[2048];
    int descriptor_length = 0;
    int found = 0;

    memset(receiver, 0, sizeof(*receiver));
    receiver->fd = -1;
    for (int bus = 0; bus < 8 && !found; ++bus) {
        for (int unit = 1; unit < 32 && !found; ++unit) {
            struct usb_device_descriptor device_descriptor;
            int fd;
            snprintf(receiver->node, sizeof(receiver->node), "/dev/ugen%d.%d",
                     bus, unit);
            if (access(receiver->node, F_OK) != 0) continue;
            fd = open(receiver->node, O_RDWR);
            if (fd < 0) continue;
            if (!read_descriptors(fd, descriptors, (int)sizeof(descriptors),
                                  &descriptor_length) ||
                !scan_layout(receiver, descriptors, descriptor_length)) {
                close(fd);
                continue;
            }
            memset(&device_descriptor, 0, sizeof(device_descriptor));
            ioctl(fd, USB_GET_DEVICE_DESC, &device_descriptor);
            receiver->vendor = read_u16(
                (const unsigned char *)&device_descriptor.idVendor);
            receiver->product = read_u16(
                (const unsigned char *)&device_descriptor.idProduct);
            receiver->fd = fd;
            found = 1;
        }
    }
    if (!found) {
        snprintf(why, (size_t)why_length,
                 "Xbox 360 Wireless Receiver (FF/5D/81) not found");
        return NULL;
    }

    { int configuration = 0; ioctl(receiver->fd, USB_SET_CONFIG, &configuration); }
    for (unsigned i = 0u; i < receiver->slot_count; ++i)
        if (ioctl(receiver->fd, USB_CLAIM_INTERFACE,
                  &receiver->slot[i].interface_number) != 0) {
            snprintf(why, (size_t)why_length, "claim interface %d: %s",
                     receiver->slot[i].interface_number, strerror(errno));
            close(receiver->fd);
            receiver->fd = -1;
            return NULL;
        }

    if (!setup_transfers(receiver)) {
        snprintf(why, (size_t)why_length, "USB transfer setup: %s",
                 strerror(errno));
        ps5_receiver_close(receiver);
        return NULL;
    }
    {
        int used = snprintf(why, (size_t)why_length, "%u slots:",
                            receiver->slot_count);
        for (unsigned i = 0u; i < receiver->slot_count &&
             used > 0 && used < why_length; ++i) {
            int in = receiver->slot[i].in_index;
            int out = receiver->slot[i].out_index;
            used += snprintf(why + used, (size_t)(why_length - used),
                             " #%u if%d %02X/%02X", i + 1u,
                             receiver->slot[i].interface_number,
                             in >= 0 ? receiver->endpoint_address[in] : 0u,
                             out >= 0 ? receiver->endpoint_address[out] : 0u);
        }
    }
    return receiver;
}

int ps5_receiver_read(ps5_receiver_t *receiver, uint8_t *slot,
                      const uint8_t **packet) {
    struct usb_fs_complete completion;
    int index;
    if (receiver == NULL || receiver->fd < 0 || slot == NULL || packet == NULL)
        return -1;
    for (unsigned i = 0u; i < receiver->slot_count; ++i) {
        int input = receiver->slot[i].in_index;
        unsigned length = receiver->slot[i].max_packet > 0
                              ? (unsigned)receiver->slot[i].max_packet
                              : X360_RECEIVER_PACKET_SIZE;
        if (length > PS5_RX_TRANSFER_SIZE) length = PS5_RX_TRANSFER_SIZE;
        if (input >= 0 && !start_transfer(receiver, (unsigned)input, length))
            return -1;
    }
    memset(&completion, 0, sizeof(completion));
    if (ioctl(receiver->fd, USB_FS_COMPLETE, &completion) != 0) {
        receiver->last_error_transient =
            errno == EBUSY || errno == EAGAIN || errno == EWOULDBLOCK;
        snprintf(receiver->last_error, sizeof(receiver->last_error),
                 "USB_FS_COMPLETE: %s", strerror(errno));
        return -1;
    }
    index = (int)completion.ep_index;
    if (index < 0 || (unsigned)index >= receiver->endpoint_count) {
        receiver->last_error_transient = 0;
        snprintf(receiver->last_error, sizeof(receiver->last_error),
                 "USB_FS_COMPLETE returned invalid index %d", index);
        return -1;
    }
    receiver->pending[index] = 0;
    if (!receiver->endpoint_is_input[index]) {
        start_next_output(receiver, (unsigned)receiver->endpoint_slot[index]);
        return 0;
    }
    if (receiver->endpoint[index].status != 0) {
        receiver->last_error_transient =
            receiver->endpoint[index].status == USB_ERR_TIMEOUT;
        snprintf(receiver->last_error, sizeof(receiver->last_error),
                 "slot %d IN endpoint %02X status %d",
                 receiver->endpoint_slot[index] + 1,
                 receiver->endpoint_address[index],
                 receiver->endpoint[index].status);
        return -1;
    }
    if (receiver->endpoint[index].aFrames == 0) {
        receiver->last_error_transient = 1;
        snprintf(receiver->last_error, sizeof(receiver->last_error),
                 "slot %d IN endpoint %02X completed without frames",
                 receiver->endpoint_slot[index] + 1,
                 receiver->endpoint_address[index]);
        return -1;
    }
    *slot = (uint8_t)receiver->endpoint_slot[index];
    *packet = receiver->data[index];
    receiver->last_error_transient = 0;
    return (int)receiver->buffer_length[index][0];
}

int ps5_receiver_write(ps5_receiver_t *receiver, uint8_t slot,
                       const uint8_t *data, size_t length) {
    int index;
    if (receiver == NULL || receiver->fd < 0 || data == NULL ||
        slot >= receiver->slot_count || length > PS5_RX_TRANSFER_SIZE) return 0;
    index = receiver->slot[slot].out_index;
    if (index < 0) return 0;
    if (!receiver->pending[index] && receiver->queue_count[slot] == 0u) {
        memcpy(receiver->data[index], data, length);
        return start_transfer(receiver, (unsigned)index, (unsigned)length);
    }
    if (receiver->queue_count[slot] >= PS5_RX_QUEUE_DEPTH) return 0;
    {
        unsigned tail = (receiver->queue_head[slot] +
                         receiver->queue_count[slot]) % PS5_RX_QUEUE_DEPTH;
        memcpy(receiver->queued_data[slot][tail], data, length);
        receiver->queued_length[slot][tail] = (uint8_t)length;
        receiver->queue_count[slot]++;
    }
    return 1;
}

void ps5_receiver_close(ps5_receiver_t *receiver) {
    struct usb_fs_uninit uninit;
    if (receiver == NULL || receiver->fd < 0) return;
    memset(&uninit, 0, sizeof(uninit));
    ioctl(receiver->fd, USB_FS_UNINIT, &uninit);
    close(receiver->fd);
    receiver->fd = -1;
}

unsigned ps5_receiver_vendor(const ps5_receiver_t *r) { return r->vendor; }
unsigned ps5_receiver_product(const ps5_receiver_t *r) { return r->product; }
unsigned ps5_receiver_slot_count(const ps5_receiver_t *r) { return r->slot_count; }
const char *ps5_receiver_node(const ps5_receiver_t *r) { return r->node; }
const char *ps5_receiver_last_error(const ps5_receiver_t *r) {
    return r != NULL && r->last_error[0] != '\0' ? r->last_error : "unknown USB error";
}
bool ps5_receiver_last_error_is_transient(const ps5_receiver_t *r) {
    return r != NULL && r->last_error_transient != 0;
}
