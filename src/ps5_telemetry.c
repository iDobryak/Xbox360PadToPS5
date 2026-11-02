#include "ps5_telemetry.h"
#include "ps5_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PS5_TELEMETRY_CSV_PATH
#define PS5_TELEMETRY_CSV_PATH "/data/ps5-xb360/telemetry.csv"
#endif

#define HISTOGRAM_BINS 64u
#define INTERVAL_BIN_US 1000u
#define LATENCY_BIN_US 50u
#define CAPTURE_DURATION_US 10000000ull
#define CAPTURE_MAX_RECORDS 8192u
#define EXPORT_RECORDS_PER_POLL 128u

typedef struct telemetry_histogram {
    unsigned long bins[HISTOGRAM_BINS];
    unsigned long count;
    unsigned long maximum;
} telemetry_histogram_t;

typedef struct telemetry_slot {
    unsigned long usb_packets;
    unsigned long input_reports;
    unsigned long malformed_packets;
    unsigned long submitted_reports;
    unsigned long coalesced_reports;
    unsigned long submit_failures;
    unsigned long estimated_gaps;
    uint64_t previous_input_us;
    unsigned long last_interval_us;
    unsigned long nominal_interval_us;
    unsigned nominal_samples;
    telemetry_histogram_t interval;
    telemetry_histogram_t bridge;
    telemetry_histogram_t insert;
} telemetry_slot_t;

typedef struct capture_record {
    uint32_t timestamp_us;
    uint32_t interval_us;
    uint32_t bridge_us;
    uint32_t insert_us;
    uint16_t packet_length;
    uint8_t slot;
    uint8_t result;
} capture_record_t;

static telemetry_slot_t g_slot[X360_RECEIVER_MAX_PADS];
static capture_record_t *g_capture;
static size_t g_capture_count;
static size_t g_capture_dropped;
static size_t g_export_index;
static uint64_t g_capture_start_us;
static uint64_t g_capture_end_us;
static int g_capture_active;
static int g_csv_ready;
static FILE *g_csv;

uint64_t ps5_telemetry_now_us(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000u +
           (uint64_t)value.tv_nsec / 1000u;
}

static unsigned long elapsed_us(uint64_t end, uint64_t begin) {
    uint64_t value = end >= begin ? end - begin : 0u;
    return value > (uint64_t)~0ul ? ~0ul : (unsigned long)value;
}

static void histogram_add(telemetry_histogram_t *histogram,
                          unsigned long value, unsigned long bin_width) {
    unsigned long index = value / bin_width;
    if (index >= HISTOGRAM_BINS) index = HISTOGRAM_BINS - 1u;
    histogram->bins[index]++;
    histogram->count++;
    if (value > histogram->maximum) histogram->maximum = value;
}

static unsigned long histogram_percentile(const telemetry_histogram_t *histogram,
                                          unsigned numerator,
                                          unsigned denominator,
                                          unsigned long bin_width) {
    unsigned long target;
    unsigned long seen = 0u;
    if (histogram->count == 0u) return 0u;
    target = (histogram->count * numerator + denominator - 1u) / denominator;
    for (unsigned long i = 0u; i < HISTOGRAM_BINS; ++i) {
        seen += histogram->bins[i];
        if (seen >= target) return (i + 1u) * bin_width;
    }
    return histogram->maximum;
}

static int is_input_report(const uint8_t *packet, size_t length) {
    return packet != NULL && length >= 18u && packet[0] == 0x00u &&
           packet[1] == 0x01u && packet[3] == 0xf0u && packet[5] == 0x13u;
}

void ps5_telemetry_reset(void) {
    memset(g_slot, 0, sizeof(g_slot));
}

bool ps5_telemetry_usb_packet(uint8_t slot, const uint8_t *packet,
                              size_t length, uint64_t timestamp_us) {
    telemetry_slot_t *metrics;
    unsigned long interval = 0u;
    if (slot >= X360_RECEIVER_MAX_PADS) return false;
    metrics = &g_slot[slot];
    metrics->usb_packets++;
    if (!is_input_report(packet, length)) return false;
    metrics->input_reports++;
    if (metrics->previous_input_us != 0u) {
        interval = elapsed_us(timestamp_us, metrics->previous_input_us);
        metrics->last_interval_us = interval;
        histogram_add(&metrics->interval, interval, INTERVAL_BIN_US);
        if (interval >= 1000u && interval <= 50000u) {
            if (metrics->nominal_interval_us == 0u)
                metrics->nominal_interval_us = interval;
            else if (interval >= metrics->nominal_interval_us / 2u &&
                     interval <= metrics->nominal_interval_us * 3u / 2u)
                metrics->nominal_interval_us =
                    (metrics->nominal_interval_us * 7u + interval) / 8u;
            if (metrics->nominal_samples < 1000u) metrics->nominal_samples++;
        }
        if (metrics->nominal_samples >= 8u && interval <= 250000u &&
            metrics->nominal_interval_us != 0u &&
            interval > metrics->nominal_interval_us * 7u / 4u) {
            unsigned long periods =
                (interval + metrics->nominal_interval_us / 2u) /
                metrics->nominal_interval_us;
            if (periods > 1u) metrics->estimated_gaps += periods - 1u;
        }
    }
    metrics->previous_input_us = timestamp_us;
    return true;
}

void ps5_telemetry_malformed(uint8_t slot) {
    if (slot < X360_RECEIVER_MAX_PADS) g_slot[slot].malformed_packets++;
}

static void capture_add(uint8_t slot, uint64_t receive_us,
                        unsigned long interval_us, unsigned long bridge_us,
                        unsigned long insert_us,
                        enum ps5_telemetry_submit_result result,
                        size_t packet_length) {
    capture_record_t *record;
    if (!g_capture_active || g_capture == NULL) return;
    if (receive_us >= g_capture_end_us) return;
    if (g_capture_count >= CAPTURE_MAX_RECORDS) {
        g_capture_dropped++;
        return;
    }
    record = &g_capture[g_capture_count++];
    record->timestamp_us = (uint32_t)(receive_us - g_capture_start_us);
    record->interval_us = (uint32_t)interval_us;
    record->bridge_us = (uint32_t)bridge_us;
    record->insert_us = (uint32_t)insert_us;
    record->packet_length = (uint16_t)packet_length;
    record->slot = slot;
    record->result = (uint8_t)result;
}

void ps5_telemetry_submit(uint8_t slot, uint64_t receive_us,
                          uint64_t insert_begin_us, uint64_t insert_end_us,
                          enum ps5_telemetry_submit_result result,
                          size_t packet_length) {
    telemetry_slot_t *metrics;
    unsigned long interval = 0u;
    unsigned long bridge;
    unsigned long insert;
    if (slot >= X360_RECEIVER_MAX_PADS) return;
    metrics = &g_slot[slot];
    bridge = receive_us != 0u ? elapsed_us(insert_end_us, receive_us) : 0u;
    insert = elapsed_us(insert_end_us, insert_begin_us);
    if (metrics->previous_input_us != 0u && receive_us != 0u)
        interval = metrics->last_interval_us;
    if (result == PS5_TELEMETRY_SUBMIT_DELIVERED) {
        histogram_add(&metrics->bridge, bridge, LATENCY_BIN_US);
        histogram_add(&metrics->insert, insert, LATENCY_BIN_US);
        metrics->submitted_reports++;
    } else if (result == PS5_TELEMETRY_SUBMIT_COALESCED) {
        insert = 0u;
        metrics->coalesced_reports++;
    } else {
        metrics->submit_failures++;
    }
    if (receive_us != 0u)
        capture_add(slot, receive_us, interval, bridge, insert, result,
                    packet_length);
}

static const char *result_name(unsigned result) {
    if (result == PS5_TELEMETRY_SUBMIT_DELIVERED) return "delivered";
    if (result == PS5_TELEMETRY_SUBMIT_COALESCED) return "coalesced";
    return "failed";
}

int ps5_telemetry_capture_start(uint64_t now_us) {
    if (g_csv != NULL) fclose(g_csv);
    g_csv = NULL;
    free(g_capture);
    g_capture = calloc(CAPTURE_MAX_RECORDS, sizeof(*g_capture));
    if (g_capture == NULL) return 0;
    unlink(PS5_TELEMETRY_CSV_PATH);
    g_capture_count = 0u;
    g_capture_dropped = 0u;
    g_export_index = 0u;
    g_capture_start_us = now_us;
    g_capture_end_us = now_us + CAPTURE_DURATION_US;
    g_capture_active = 1;
    g_csv_ready = 0;
    ps5_log("telemetry: 10 second capture started");
    return 1;
}

static void export_poll(void) {
    size_t end;
    if (g_capture == NULL || g_csv_ready) return;
    if (g_csv == NULL) {
        g_csv = fopen(PS5_TELEMETRY_CSV_PATH, "wb");
        if (g_csv == NULL) {
            ps5_log("telemetry: cannot create %s", PS5_TELEMETRY_CSV_PATH);
            free(g_capture);
            g_capture = NULL;
            return;
        }
        fputs("time_us,slot,packet_length,interval_us,bridge_us,insert_us,result\n",
              g_csv);
    }
    end = g_export_index + EXPORT_RECORDS_PER_POLL;
    if (end > g_capture_count) end = g_capture_count;
    while (g_export_index < end) {
        const capture_record_t *record = &g_capture[g_export_index++];
        fprintf(g_csv, "%u,%u,%u,%u,%u,%u,%s\n",
                record->timestamp_us, (unsigned)record->slot + 1u,
                (unsigned)record->packet_length, record->interval_us,
                record->bridge_us, record->insert_us,
                result_name(record->result));
    }
    if (g_export_index == g_capture_count) {
        fclose(g_csv);
        g_csv = NULL;
        free(g_capture);
        g_capture = NULL;
        g_csv_ready = 1;
        ps5_log("telemetry: CSV ready (%lu records, %lu dropped)",
                (unsigned long)g_capture_count,
                (unsigned long)g_capture_dropped);
    }
}

void ps5_telemetry_poll(uint64_t now_us) {
    if (g_capture_active && now_us >= g_capture_end_us) {
        g_capture_active = 0;
        ps5_log("telemetry: capture finished; exporting CSV");
    }
    if (!g_capture_active) export_poll();
}

void ps5_telemetry_close(void) {
    if (g_csv != NULL) fclose(g_csv);
    g_csv = NULL;
    free(g_capture);
    g_capture = NULL;
    g_capture_active = 0;
}

void ps5_telemetry_slot_snapshot(unsigned slot,
                                 ps5_telemetry_slot_snapshot_t *snapshot) {
    const telemetry_slot_t *metrics;
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (slot >= X360_RECEIVER_MAX_PADS) return;
    metrics = &g_slot[slot];
    snapshot->usb_packets = metrics->usb_packets;
    snapshot->input_reports = metrics->input_reports;
    snapshot->malformed_packets = metrics->malformed_packets;
    snapshot->submitted_reports = metrics->submitted_reports;
    snapshot->coalesced_reports = metrics->coalesced_reports;
    snapshot->submit_failures = metrics->submit_failures;
    snapshot->estimated_gaps = metrics->estimated_gaps;
    snapshot->interval_p50_us = histogram_percentile(&metrics->interval, 50u, 100u, INTERVAL_BIN_US);
    snapshot->interval_p95_us = histogram_percentile(&metrics->interval, 95u, 100u, INTERVAL_BIN_US);
    snapshot->interval_p99_us = histogram_percentile(&metrics->interval, 99u, 100u, INTERVAL_BIN_US);
    snapshot->interval_max_us = metrics->interval.maximum;
    snapshot->bridge_p50_us = histogram_percentile(&metrics->bridge, 50u, 100u, LATENCY_BIN_US);
    snapshot->bridge_p95_us = histogram_percentile(&metrics->bridge, 95u, 100u, LATENCY_BIN_US);
    snapshot->bridge_p99_us = histogram_percentile(&metrics->bridge, 99u, 100u, LATENCY_BIN_US);
    snapshot->bridge_max_us = metrics->bridge.maximum;
    snapshot->insert_p50_us = histogram_percentile(&metrics->insert, 50u, 100u, LATENCY_BIN_US);
    snapshot->insert_p95_us = histogram_percentile(&metrics->insert, 95u, 100u, LATENCY_BIN_US);
    snapshot->insert_p99_us = histogram_percentile(&metrics->insert, 99u, 100u, LATENCY_BIN_US);
    snapshot->insert_max_us = metrics->insert.maximum;
}

void ps5_telemetry_capture_snapshot(uint64_t now_us,
                                    ps5_telemetry_capture_snapshot_t *snapshot) {
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active = g_capture_active != 0;
    snapshot->csv_ready = g_csv_ready != 0;
    snapshot->records = (unsigned long)g_capture_count;
    snapshot->dropped = (unsigned long)g_capture_dropped;
    if (g_capture_active && now_us < g_capture_end_us)
        snapshot->remaining_ms = (unsigned)((g_capture_end_us - now_us) / 1000u);
}
