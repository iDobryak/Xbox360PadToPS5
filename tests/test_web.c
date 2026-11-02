#include "ps5_log.h"
#include "ps5_web.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int sceKernelSendNotificationRequest(int a, void *b, size_t c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    return 0;
}

static int exchange(ps5_web_t *web, const ps5_web_status_t *status,
                    uint16_t port, const char *request,
                    char *response, size_t capacity) {
    struct sockaddr_in address;
    size_t used = 0u;
    int action = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(send(fd, request, strlen(request), 0) == (ssize_t)strlen(request));
    for (int i = 0; i < 2000; ++i) {
        int current = ps5_web_poll(web, status);
        ssize_t count;
        if (current != PS5_WEB_ACTION_NONE) action = current;
        count = recv(fd, response + used, capacity - used - 1u, MSG_DONTWAIT);
        if (count > 0) used += (size_t)count;
        else if (count == 0) break;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) assert(0);
        usleep(1000);
    }
    response[used] = '\0';
    close(fd);
    return action;
}

int main(void) {
    char why[128];
    char response[65536];
    uint16_t port = (uint16_t)(18000 + ((unsigned)getpid() % 1000u));
    ps5_web_status_t status = {0};
    ps5_web_t *web;
    FILE *telemetry_file;

    unlink(PS5_WEB_LOG_PATH);
    assert(ps5_log_open("/tmp", PS5_WEB_LOG_PATH));
    ps5_log("web test line");
    telemetry_file = fopen(PS5_WEB_TELEMETRY_PATH, "wb");
    assert(telemetry_file != NULL);
    fputs("time_us,slot\n100,1\n", telemetry_file);
    fclose(telemetry_file);
    status.pid = 4242;
    status.uptime_seconds = 17u;
    status.receiver_node = "/dev/ugen1.2";
    status.receiver_vendor = 0x045eu;
    status.receiver_product = 0x0291u;
    status.slot_count = 4u;
    status.last_usb_error = "none";
    status.slots[0].connected = true;
    status.slots[0].ps_available = true;
    status.slots[0].reports = 99u;
    status.slots[0].telemetry.usb_packets = 101u;
    status.slots[0].telemetry.input_reports = 100u;
    status.slots[0].telemetry.submitted_reports = 98u;
    status.slots[0].telemetry.bridge_p95_us = 250u;
    status.capture.csv_ready = true;
    status.capture.records = 2u;

    web = ps5_web_open(port, why, (int)sizeof(why));
    assert(web != NULL);
    assert(exchange(web, &status, port,
                    "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
                    response, sizeof(response)) == PS5_WEB_ACTION_NONE);
    assert(strstr(response, "200 OK") != NULL);
    assert(strstr(response, "Xbox 360 Receiver") != NULL);
    assert(strstr(response, "Нажать PS") != NULL);

    assert(exchange(web, &status, port,
                    "GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n",
                    response, sizeof(response)) == PS5_WEB_ACTION_NONE);
    assert(strstr(response, "\"pid\":4242") != NULL);
    assert(strstr(response, "\"reports\":99") != NULL);
    assert(strstr(response, "\"psAvailable\":true") != NULL);
    assert(strstr(response, "\"bridgeP95\":250") != NULL);
    assert(strstr(response, "\"csvReady\":true") != NULL);

    assert(exchange(web, &status, port,
                    "GET /api/log HTTP/1.1\r\nHost: localhost\r\n\r\n",
                    response, sizeof(response)) == PS5_WEB_ACTION_NONE);
    assert(strstr(response, "web test line") != NULL);

    for (unsigned i = 0u; i < 220u; ++i)
        ps5_log("download filler %03u 0123456789012345678901234567890123456789",
                i);
    assert(exchange(web, &status, port,
                    "GET /api/log/download HTTP/1.1\r\n"
                    "Host: localhost\r\n\r\n",
                    response, sizeof(response)) == PS5_WEB_ACTION_NONE);
    assert(strstr(response, "Content-Disposition: attachment") != NULL);
    assert(strstr(response, "filename=\"ps5-xb360-receiver.log\"") != NULL);
    assert(strstr(response, "web test line") != NULL);
    assert(strstr(response, "download filler 219") != NULL);

    assert(exchange(web, &status, port,
                    "GET /api/telemetry/download HTTP/1.1\r\n"
                    "Host: localhost\r\n\r\n",
                    response, sizeof(response)) == PS5_WEB_ACTION_NONE);
    assert(strstr(response, "filename=\"ps5-xb360-telemetry.csv\"") != NULL);
    assert(strstr(response, "time_us,slot") != NULL);

    assert(exchange(web, &status, port,
                    "POST /api/telemetry/start HTTP/1.1\r\n"
                    "Host: localhost\r\nContent-Length: 0\r\n\r\n",
                    response, sizeof(response)) ==
           PS5_WEB_ACTION_TELEMETRY_START);

    assert(exchange(web, &status, port,
                    "POST /api/ps/1 HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Length: 0\r\n\r\n",
                    response, sizeof(response)) == PS5_WEB_ACTION_PS_SLOT_1);

    assert(exchange(web, &status, port,
                    "POST /api/inquiry HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Length: 0\r\n\r\n",
                    response, sizeof(response)) == PS5_WEB_ACTION_INQUIRY);
    ps5_web_close(web);
    ps5_log_close();
    unlink(PS5_WEB_LOG_PATH);
    unlink(PS5_WEB_TELEMETRY_PATH);
    puts("ps5_xb360 web tests: OK");
    return 0;
}
