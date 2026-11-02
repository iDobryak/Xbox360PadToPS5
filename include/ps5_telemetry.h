#ifndef PS5_XB360_TELEMETRY_H
#define PS5_XB360_TELEMETRY_H

#include "x360_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ps5_telemetry_submit_result {
    PS5_TELEMETRY_SUBMIT_FAILED = 0,
    PS5_TELEMETRY_SUBMIT_DELIVERED = 1,
    PS5_TELEMETRY_SUBMIT_COALESCED = 2
};

typedef struct ps5_telemetry_slot_snapshot {
    unsigned long usb_packets;
    unsigned long input_reports;
    unsigned long malformed_packets;
    unsigned long submitted_reports;
    unsigned long coalesced_reports;
    unsigned long submit_failures;
    unsigned long estimated_gaps;
    unsigned long interval_p50_us;
    unsigned long interval_p95_us;
    unsigned long interval_p99_us;
    unsigned long interval_max_us;
    unsigned long bridge_p50_us;
    unsigned long bridge_p95_us;
    unsigned long bridge_p99_us;
    unsigned long bridge_max_us;
    unsigned long insert_p50_us;
    unsigned long insert_p95_us;
    unsigned long insert_p99_us;
    unsigned long insert_max_us;
} ps5_telemetry_slot_snapshot_t;

typedef struct ps5_telemetry_capture_snapshot {
    bool active;
    bool csv_ready;
    unsigned remaining_ms;
    unsigned long records;
    unsigned long dropped;
} ps5_telemetry_capture_snapshot_t;

uint64_t ps5_telemetry_now_us(void);
void ps5_telemetry_reset(void);

/* Returns true for a controller-state report and supplies its receive time. */
bool ps5_telemetry_usb_packet(uint8_t slot, const uint8_t *packet,
                              size_t length, uint64_t timestamp_us);
void ps5_telemetry_malformed(uint8_t slot);
void ps5_telemetry_submit(uint8_t slot, uint64_t receive_us,
                          uint64_t insert_begin_us, uint64_t insert_end_us,
                          enum ps5_telemetry_submit_result result,
                          size_t packet_length);

int ps5_telemetry_capture_start(uint64_t now_us);
void ps5_telemetry_poll(uint64_t now_us);
void ps5_telemetry_close(void);

void ps5_telemetry_slot_snapshot(unsigned slot,
                                 ps5_telemetry_slot_snapshot_t *snapshot);
void ps5_telemetry_capture_snapshot(uint64_t now_us,
                                    ps5_telemetry_capture_snapshot_t *snapshot);

#endif
