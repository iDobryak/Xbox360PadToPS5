#ifndef PS5_XB360_PS5_RECEIVER_H
#define PS5_XB360_PS5_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ps5_receiver ps5_receiver_t;

/* Finds a USB device exposing Xbox 360 wireless receiver interfaces
 * (vendor-specific FF/5D/81), claims up to four slots and opens their
 * interrupt endpoints through the PS5 USB_FS ioctls. */
ps5_receiver_t *ps5_receiver_open(char *why, int why_length);
void ps5_receiver_close(ps5_receiver_t *receiver);

unsigned ps5_receiver_vendor(const ps5_receiver_t *receiver);
unsigned ps5_receiver_product(const ps5_receiver_t *receiver);
unsigned ps5_receiver_slot_count(const ps5_receiver_t *receiver);
const char *ps5_receiver_node(const ps5_receiver_t *receiver);
const char *ps5_receiver_last_error(const ps5_receiver_t *receiver);
bool ps5_receiver_last_error_is_transient(const ps5_receiver_t *receiver);

/* Returns packet length, 0 for a non-input completion, and -1 on error. */
int ps5_receiver_read(ps5_receiver_t *receiver, uint8_t *slot,
                      const uint8_t **packet);
int ps5_receiver_write(ps5_receiver_t *receiver, uint8_t slot,
                       const uint8_t *data, size_t length);

#endif
