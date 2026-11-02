#ifndef PS5_XB360_LOG_H
#define PS5_XB360_LOG_H

#include <stddef.h>

int ps5_log_open(const char *directory, const char *path);
void ps5_log_close(void);
void ps5_log(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
void ps5_notify(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
size_t ps5_log_tail(char *output, size_t capacity);

#endif
