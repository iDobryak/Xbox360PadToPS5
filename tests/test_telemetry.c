#include "ps5_log.h"
#include "ps5_telemetry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sceKernelSendNotificationRequest(int a, void *b, size_t c, int d) {
    (void)a; (void)b; (void)c; (void)d; return 0;
}

int main(void) {
    uint8_t packet[18] = {0};
    ps5_telemetry_slot_snapshot_t slot;
    ps5_telemetry_capture_snapshot_t capture;
    char csv[4096];
    FILE *file;
    size_t length;

    packet[1] = 0x01u;
    packet[3] = 0xf0u;
    packet[5] = 0x13u;
    unlink(PS5_TELEMETRY_CSV_PATH);
    ps5_telemetry_reset();

    assert(ps5_telemetry_usb_packet(0u, packet, sizeof(packet), 1000000u));
    ps5_telemetry_submit(0u, 1000000u, 1000100u, 1000150u,
                         PS5_TELEMETRY_SUBMIT_DELIVERED, sizeof(packet));
    assert(ps5_telemetry_usb_packet(0u, packet, sizeof(packet), 1008000u));
    ps5_telemetry_submit(0u, 1008000u, 1008100u, 1008200u,
                         PS5_TELEMETRY_SUBMIT_COALESCED, sizeof(packet));
    for (unsigned i = 2u; i < 9u; ++i) {
        uint64_t at = 1000000u + (uint64_t)i * 8000u;
        assert(ps5_telemetry_usb_packet(0u, packet, sizeof(packet), at));
        ps5_telemetry_submit(0u, at, at + 100u, at + 150u,
                             PS5_TELEMETRY_SUBMIT_DELIVERED, sizeof(packet));
    }
    assert(ps5_telemetry_usb_packet(0u, packet, sizeof(packet), 1080000u));
    ps5_telemetry_submit(0u, 1080000u, 1080100u, 1080300u,
                         PS5_TELEMETRY_SUBMIT_FAILED, sizeof(packet));
    ps5_telemetry_malformed(0u);
    ps5_telemetry_slot_snapshot(0u, &slot);
    assert(slot.usb_packets == 10u && slot.input_reports == 10u);
    assert(slot.submitted_reports == 8u && slot.coalesced_reports == 1u);
    assert(slot.submit_failures == 1u && slot.malformed_packets == 1u);
    assert(slot.estimated_gaps == 1u);
    assert(slot.interval_p50_us == 9000u);
    assert(slot.interval_p95_us == 17000u);
    assert(slot.interval_max_us == 16000u);
    assert(slot.bridge_max_us == 150u && slot.insert_max_us == 50u);

    assert(ps5_telemetry_capture_start(2000000u));
    assert(ps5_telemetry_usb_packet(0u, packet, sizeof(packet), 2008000u));
    ps5_telemetry_submit(0u, 2008000u, 2008100u, 2008150u,
                         PS5_TELEMETRY_SUBMIT_DELIVERED, sizeof(packet));
    assert(ps5_telemetry_usb_packet(0u, packet, sizeof(packet), 2016000u));
    ps5_telemetry_submit(0u, 2016000u, 2016100u, 2016200u,
                         PS5_TELEMETRY_SUBMIT_COALESCED, sizeof(packet));
    ps5_telemetry_poll(12000001u);
    ps5_telemetry_capture_snapshot(12000001u, &capture);
    assert(!capture.active && capture.csv_ready && capture.records == 2u);

    file = fopen(PS5_TELEMETRY_CSV_PATH, "rb");
    assert(file != NULL);
    length = fread(csv, 1u, sizeof(csv) - 1u, file);
    fclose(file);
    csv[length] = '\0';
    assert(strstr(csv, "time_us,slot,packet_length") != NULL);
    assert(strstr(csv, "8000,1,18,928000") != NULL);
    assert(strstr(csv, "delivered") != NULL);
    assert(strstr(csv, "coalesced") != NULL);

    ps5_telemetry_close();
    unlink(PS5_TELEMETRY_CSV_PATH);
    puts("ps5_xb360 telemetry tests: OK");
    return 0;
}
