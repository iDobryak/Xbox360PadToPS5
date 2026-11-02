#include "ps5_log.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct notification_request {
    char reserved[45];
    char message[3075];
} notification_request_t;

int sceKernelSendNotificationRequest(int, notification_request_t *, size_t, int);
static FILE *g_log;
#define LOG_RING_LINES 64u
#define LOG_RING_LINE_SIZE 192u
static char g_ring[LOG_RING_LINES][LOG_RING_LINE_SIZE];
static unsigned g_ring_head;
static unsigned g_ring_count;

int ps5_log_open(const char *directory, const char *path) {
    mkdir(directory, 0755);
    g_log = fopen(path, "ab");
    return g_log != NULL;
}

void ps5_log_close(void) {
    if (g_log != NULL) fclose(g_log);
    g_log = NULL;
}

void ps5_log(const char *format, ...) {
    char stamp[32];
    char message[LOG_RING_LINE_SIZE - 16u];
    time_t now = time(NULL);
    struct tm value;
    va_list arguments;
    gmtime_r(&now, &value);
    if (strftime(stamp, sizeof(stamp), "%H:%M:%S", &value) == 0u)
        strcpy(stamp, "00:00:00");
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    snprintf(g_ring[g_ring_head], LOG_RING_LINE_SIZE, "%s %s", stamp, message);
    g_ring_head = (g_ring_head + 1u) % LOG_RING_LINES;
    if (g_ring_count < LOG_RING_LINES) g_ring_count++;

    if (g_log != NULL) {
        fprintf(g_log, "%s %s\n", stamp, message);
        fflush(g_log);
    }
}

size_t ps5_log_tail(char *output, size_t capacity) {
    size_t used = 0u;
    unsigned first;
    if (output == NULL || capacity == 0u) return 0u;
    first = (g_ring_head + LOG_RING_LINES - g_ring_count) % LOG_RING_LINES;
    for (unsigned i = 0u; i < g_ring_count; ++i) {
        const char *line = g_ring[(first + i) % LOG_RING_LINES];
        size_t length = strlen(line);
        if (used + length + 1u >= capacity) break;
        memcpy(output + used, line, length);
        used += length;
        output[used++] = '\n';
    }
    output[used] = '\0';
    return used;
}

void ps5_notify(const char *format, ...) {
    notification_request_t request;
    va_list arguments;
    memset(&request, 0, sizeof(request));
    va_start(arguments, format);
    vsnprintf(request.message, sizeof(request.message), format, arguments);
    va_end(arguments);
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}
