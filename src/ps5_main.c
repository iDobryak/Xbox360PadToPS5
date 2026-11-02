#include "ps5_log.h"
#include "ps5_receiver.h"
#include "ps5_telemetry.h"
#include "ps5_vpad.h"
#include "ps5_web.h"
#include "ps5_xb360_bridge.h"

#include <sys/stat.h>
#include <sys/time.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIRECTORY "/data/ps5-xb360"
#define LOG_PATH STATE_DIRECTORY "/receiver.log"
#define LOCK_PATH STATE_DIRECTORY "/receiver.lock"
#define STOP_PATH STATE_DIRECTORY "/receiver.stop"
#define WEB_PORT_PATH STATE_DIRECTORY "/receiver.port"
#define LOCK_STALE_SECONDS 15
#define DISCOVERY_SECONDS 5
#define QUIT_HOLD_SECONDS 2
#define WEB_PORT_FIRST 8096u
#define WEB_PORT_LAST 8105u
#define PS_HOLD_MS 700

typedef struct app_context {
    ps5_receiver_t *receiver;
    ps5_vpad_t *pad[X360_RECEIVER_MAX_PADS];
    time_t quit_since[X360_RECEIVER_MAX_PADS];
    unsigned long reports[X360_RECEIVER_MAX_PADS];
    uint64_t current_receive_us[X360_RECEIVER_MAX_PADS];
    size_t current_packet_length[X360_RECEIVER_MAX_PADS];
    unsigned raw_status_logged[X360_RECEIVER_MAX_PADS];
    unsigned raw_input_logged[X360_RECEIVER_MAX_PADS];
    unsigned raw_other_logged[X360_RECEIVER_MAX_PADS];
    unsigned long usb_errors;
} app_context_t;

static int lock_take(void) {
    struct stat status;
    int fd;
    if (stat(LOCK_PATH, &status) == 0 &&
        time(NULL) - status.st_mtime < LOCK_STALE_SECONDS) {
        time_t original_mtime = status.st_mtime;
        int refreshed = 0;

        /* A second launch asks a compatible running copy to shut down before
         * claiming USB itself. Never remove a live lock: two readers on the
         * receiver consume each other's endpoint completions. */
        fd = open(STOP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        for (int i = 0; i < 50; ++i) {
            if (stat(LOCK_PATH, &status) != 0) break;
            if (status.st_mtime != original_mtime) refreshed = 1;
            usleep(100000);
        }
        if (stat(LOCK_PATH, &status) == 0 && refreshed) return 0;
        /* No heartbeat during the wait: the lock belongs to a dead process. */
    }
    unlink(STOP_PATH);
    fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    {
        char pid_text[32];
        int length = snprintf(pid_text, sizeof(pid_text), "%d\n", (int)getpid());
        if (length > 0) (void)write(fd, pid_text, (size_t)length);
    }
    close(fd);
    return 1;
}

static void lock_refresh(void) { utimes(LOCK_PATH, NULL); }
static void lock_release(void) {
    unlink(WEB_PORT_PATH);
    unlink(STOP_PATH);
    unlink(LOCK_PATH);
}

static void save_web_port(uint16_t port) {
    char text[16];
    int fd = open(WEB_PORT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int length;
    if (fd < 0) return;
    length = snprintf(text, sizeof(text), "%u\n", (unsigned)port);
    if (length > 0) (void)write(fd, text, (size_t)length);
    close(fd);
}

static bool backend_write(void *opaque, uint8_t slot, const uint8_t *data,
                          size_t length) {
    app_context_t *app = opaque;
    if (ps5_receiver_write(app->receiver, slot, data, length) != 0) return true;
    ps5_log("slot %u: USB OUT failed: %s", slot + 1u,
            ps5_receiver_last_error(app->receiver));
    return false;
}

static void log_raw_packet(app_context_t *app, uint8_t slot,
                           const uint8_t *packet, int length) {
    char line[160];
    int used;
    int shown;
    unsigned *counter;
    unsigned limit;
    if (slot >= X360_RECEIVER_MAX_PADS) return;
    if (length >= 2 && packet[0] == 0x08u) {
        counter = &app->raw_status_logged[slot]; limit = 4u;
    } else if (length >= 2 && packet[0] == 0x00u && packet[1] == 0x01u) {
        counter = &app->raw_input_logged[slot]; limit = 8u;
    } else {
        counter = &app->raw_other_logged[slot]; limit = 4u;
    }
    if (*counter >= limit) return;
    (*counter)++;
    used = snprintf(line, sizeof(line), "slot %u raw %d:", slot + 1u, length);
    shown = length < 32 ? length : 32;
    for (int i = 0; i < shown && used > 0 && used < (int)sizeof(line); ++i)
        used += snprintf(line + used, sizeof(line) - (size_t)used,
                         " %02X", packet[i]);
    ps5_log("%s", line);
}

static void backend_publish(void *opaque, uint8_t slot,
                            const dualsense_state_t *state) {
    app_context_t *app = opaque;
    enum ps5_vpad_submit_result result;
    enum ps5_telemetry_submit_result telemetry_result;
    uint64_t insert_begin;
    uint64_t insert_end;
    char why[128];
    if (app->pad[slot] == NULL) {
        app->pad[slot] = ps5_vpad_open(slot, why, (int)sizeof(why));
        if (app->pad[slot] == NULL) {
            uint64_t failed_at = ps5_telemetry_now_us();
            ps5_telemetry_submit(slot, app->current_receive_us[slot],
                                 failed_at, failed_at,
                                 PS5_TELEMETRY_SUBMIT_FAILED,
                                 app->current_packet_length[slot]);
            ps5_log("slot %u: virtual DualSense failed: %s", slot + 1u, why);
            return;
        }
        ps5_log("slot %u: %s", slot + 1u, why);
        ps5_notify("Xbox 360 controller %u connected", slot + 1u);
    }
    insert_begin = ps5_telemetry_now_us();
    result = ps5_vpad_submit(app->pad[slot], state);
    insert_end = ps5_telemetry_now_us();
    telemetry_result = result == PS5_VPAD_SUBMIT_DELIVERED
                           ? PS5_TELEMETRY_SUBMIT_DELIVERED
                       : result == PS5_VPAD_SUBMIT_COALESCED
                           ? PS5_TELEMETRY_SUBMIT_COALESCED
                           : PS5_TELEMETRY_SUBMIT_FAILED;
    ps5_telemetry_submit(slot, app->current_receive_us[slot], insert_begin,
                         insert_end, telemetry_result,
                         app->current_packet_length[slot]);
    if (result == PS5_VPAD_SUBMIT_DELIVERED) {
        app->reports[slot]++;
    } else if (result == PS5_VPAD_SUBMIT_FAILED) {
        ps5_log("slot %u: virtual DualSense refused input", slot + 1u);
        ps5_vpad_close(app->pad[slot]);
        app->pad[slot] = NULL;
    }
}

static void backend_disconnect(void *opaque, uint8_t slot) {
    app_context_t *app = opaque;
    if (app->pad[slot] != NULL) {
        ps5_vpad_close(app->pad[slot]);
        app->pad[slot] = NULL;
        ps5_log("slot %u: controller disconnected", slot + 1u);
        ps5_notify("Xbox 360 controller %u disconnected", slot + 1u);
    }
    app->quit_since[slot] = 0;
}

static int wants_to_quit(app_context_t *app, const ps5_xb360_bridge_t *bridge,
                         uint8_t slot) {
    uint16_t combo = X360_BTN_BACK | X360_BTN_START;
    if ((bridge->slots[slot].state.buttons & combo) == combo) {
        if (app->quit_since[slot] == 0) app->quit_since[slot] = time(NULL);
        return time(NULL) - app->quit_since[slot] >= QUIT_HOLD_SECONDS;
    }
    app->quit_since[slot] = 0;
    return 0;
}

static void fill_web_status(ps5_web_status_t *status, const app_context_t *app,
                            const ps5_xb360_bridge_t *bridge,
                            time_t started_at) {
    memset(status, 0, sizeof(*status));
    status->pid = (int)getpid();
    status->uptime_seconds = (unsigned long)(time(NULL) - started_at);
    status->receiver_node = ps5_receiver_node(app->receiver);
    status->receiver_vendor = ps5_receiver_vendor(app->receiver);
    status->receiver_product = ps5_receiver_product(app->receiver);
    status->slot_count = ps5_receiver_slot_count(app->receiver);
    status->usb_errors = app->usb_errors;
    status->last_usb_error = app->usb_errors != 0u
                                 ? ps5_receiver_last_error(app->receiver)
                                 : "none";
    ps5_telemetry_capture_snapshot(ps5_telemetry_now_us(), &status->capture);
    for (unsigned i = 0u; i < status->slot_count; ++i) {
        status->slots[i].connected = bridge->slots[i].connected;
        status->slots[i].ps_available = ps5_vpad_has_ps(app->pad[i]);
        status->slots[i].reports = app->reports[i];
        ps5_telemetry_slot_snapshot(i, &status->slots[i].telemetry);
    }
}

int main(void) {
    ps5_xb360_bridge_t bridge;
    ps5_xb360_backend_t backend;
    ps5_web_status_t web_status;
    ps5_web_t *web = NULL;
    app_context_t app;
    char why[160];
    time_t last_discovery = 0;
    time_t last_lock = 0;
    time_t started_at = time(NULL);
    uint16_t web_port = 0u;
    int running = 1;

    memset(&app, 0, sizeof(app));
    ps5_telemetry_reset();
    if (!ps5_log_open(STATE_DIRECTORY, LOG_PATH)) return 1;
    ps5_log("========================================");
    ps5_log("PS5 Xbox 360 Wireless Receiver bridge");
    if (!lock_take()) {
        ps5_notify("Receiver bridge still running; stop it or reboot once");
        ps5_log_close();
        return 1;
    }

    if (ps5_vpad_system_init(why, (int)sizeof(why)))
        ps5_log("vpad backend: %s", why);
    else
        ps5_log("vpad backend: %s; direct fallback active (PS unavailable)", why);

    app.receiver = ps5_receiver_open(why, (int)sizeof(why));
    if (app.receiver == NULL) {
        ps5_log("receiver: %s", why);
        ps5_notify("Xbox 360 Receiver: %s", why);
        running = 0;
    } else {
        ps5_log("receiver %s %04X:%04X: %s",
                ps5_receiver_node(app.receiver),
                ps5_receiver_vendor(app.receiver),
                ps5_receiver_product(app.receiver), why);
        ps5_notify("Xbox 360 Receiver ready; pair a controller");
    }

    backend.context = &app;
    backend.usb_write = backend_write;
    backend.publish = backend_publish;
    backend.disconnect = backend_disconnect;
    ps5_xb360_bridge_init(&bridge, &backend);

    if (running) {
        for (uint16_t port = WEB_PORT_FIRST;
             web == NULL && port <= WEB_PORT_LAST; ++port) {
            web = ps5_web_open(port, why, (int)sizeof(why));
            if (web != NULL) web_port = port;
        }
        if (web != NULL) {
            save_web_port(web_port);
            ps5_log("web: %s (http://PS5-IP:%u/)", why, web_port);
            ps5_notify("Xbox 360 Receiver UI: http://PS5-IP:%u", web_port);
        } else {
            ps5_log("web disabled: no free port in %u-%u (last error: %s)",
                    WEB_PORT_FIRST, WEB_PORT_LAST, why);
        }
    }

    for (uint8_t i = 0u; running &&
         i < ps5_receiver_slot_count(app.receiver); ++i)
        ps5_xb360_bridge_start_slot(&bridge, i);
    last_discovery = time(NULL);

    while (running) {
        const uint8_t *packet = NULL;
        uint8_t slot = 0u;
        int web_action;
        uint64_t loop_us = ps5_telemetry_now_us();
        ps5_vpad_system_poll();
        ps5_telemetry_poll(loop_us);
        fill_web_status(&web_status, &app, &bridge, started_at);
        web_action = ps5_web_poll(web, &web_status);
        if (web_action == PS5_WEB_ACTION_STOP) {
            ps5_log("stop requested from web UI");
            running = 0;
            continue;
        }
        if (web_action == PS5_WEB_ACTION_INQUIRY) {
            ps5_log("manual inquiry requested from web UI");
            for (uint8_t i = 0u; i < ps5_receiver_slot_count(app.receiver); ++i)
                ps5_xb360_bridge_start_slot(&bridge, i);
        }
        if (web_action == PS5_WEB_ACTION_TELEMETRY_START) {
            if (!ps5_telemetry_capture_start(ps5_telemetry_now_us()))
                ps5_log("telemetry: capture start failed (out of memory)");
        }
        if (web_action >= PS5_WEB_ACTION_PS_SLOT_1 &&
            web_action < PS5_WEB_ACTION_PS_SLOT_1 +
                         (int)X360_RECEIVER_MAX_PADS) {
            unsigned ps_slot = (unsigned)(web_action - PS5_WEB_ACTION_PS_SLOT_1);
            if (app.pad[ps_slot] != NULL &&
                ps5_vpad_press_ps(app.pad[ps_slot], PS_HOLD_MS))
                ps5_log("slot %u: PS pressed from web UI", ps_slot + 1u);
            else
                ps5_log("slot %u: PS request ignored (virtual pad not ready)",
                        ps_slot + 1u);
        }
        int length = ps5_receiver_read(app.receiver, &slot, &packet);
        time_t now = time(NULL);
        if (access(STOP_PATH, F_OK) == 0) {
            ps5_log("stop requested by a newer payload launch");
            running = 0;
            continue;
        }
        if (length > 0 && slot < X360_RECEIVER_MAX_PADS) {
            uint64_t receive_us = ps5_telemetry_now_us();
            if (ps5_telemetry_usb_packet(slot, packet, (size_t)length,
                                         receive_us)) {
                app.current_receive_us[slot] = receive_us;
                app.current_packet_length[slot] = (size_t)length;
            } else {
                app.current_receive_us[slot] = 0u;
                app.current_packet_length[slot] = (size_t)length;
            }
            log_raw_packet(&app, slot, packet, length);
            if (!ps5_xb360_bridge_feed(&bridge, slot, packet,
                                       (size_t)length)) {
                ps5_telemetry_malformed(slot);
                ps5_log("slot %u: malformed receiver packet (%d bytes)",
                        slot + 1u, length);
            } else if (wants_to_quit(&app, &bridge, slot))
                running = 0;
        } else if (length < 0) {
            if (ps5_receiver_last_error_is_transient(app.receiver)) {
                usleep(5000);
            } else {
                app.usb_errors++;
                if (app.usb_errors == 1u || app.usb_errors % 100u == 0u)
                    ps5_log("USB read error #%lu: %s", app.usb_errors,
                            ps5_receiver_last_error(app.receiver));
                usleep(5000);
            }
        }
        if (now != last_lock) {
            last_lock = now;
            lock_refresh();
        }
        if (now - last_discovery >= DISCOVERY_SECONDS) {
            last_discovery = now;
            for (uint8_t i = 0u; i < ps5_receiver_slot_count(app.receiver); ++i)
                if (!bridge.slots[i].connected)
                    ps5_xb360_bridge_start_slot(&bridge, i);
        }
    }

    ps5_web_close(web);
    for (uint8_t i = 0u; i < X360_RECEIVER_MAX_PADS; ++i) {
        if (app.pad[i] != NULL) ps5_vpad_close(app.pad[i]);
        if (app.reports[i] != 0u)
            ps5_log("slot %u: %lu input reports delivered", i + 1u,
                    app.reports[i]);
    }
    ps5_vpad_system_close();
    ps5_telemetry_close();
    ps5_receiver_close(app.receiver);
    lock_release();
    ps5_notify("Xbox 360 Receiver bridge stopped");
    ps5_log_close();
    return 0;
}
