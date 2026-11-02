/*
 * Xbox360PadToPS5 virtual-pad backend.
 *
 * Portions adapted in 2026 from Any-Pad PS5 by elmonomalvad0
 * (GPL-3.0-or-later) and FGG-XSense by FGG STORE (GPL-3.0).
 * See NOTICE.md for upstream links and details.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "ps5_vpad.h"
#include "ps5_log.h"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DIRECT_HID_ADD_DEVICE  0xC018482AUL
#define DIRECT_HID_INSERT_DATA 0x8018482CUL
#define DIRECT_HID_DEL_DEVICE  0x80104850UL
#define DIRECT_HID_CMD_ADD 3u
#define DIRECT_HID_CMD_DEL 5u
#define DIRECT_DESCRIPTOR_SIZE 168u
#define DIRECT_REPORT_SIZE 0xA0u

#define VIRTUAL_DEVICE_DUALSENSE 3
#define IDENTIFY_TIMEOUT_MS 3000L
#define AMBIGUITY_WAIT_MS 300L
#define KLOG_LINE_SIZE 1024u
#define MAX_VPADS 4u

#ifndef PS5_VPAD_LOG_PORT
#define PS5_VPAD_LOG_PORT 3232u
#endif

#define SCE_CREATE   0x000001u
#define SCE_L3       0x000002u
#define SCE_R3       0x000004u
#define SCE_OPTIONS  0x000008u
#define SCE_UP       0x000010u
#define SCE_RIGHT    0x000020u
#define SCE_DOWN     0x000040u
#define SCE_LEFT     0x000080u
#define SCE_L2       0x000100u
#define SCE_R2       0x000200u
#define SCE_L1       0x000400u
#define SCE_R1       0x000800u
#define SCE_TRIANGLE 0x001000u
#define SCE_CIRCLE   0x002000u
#define SCE_CROSS    0x004000u
#define SCE_SQUARE   0x008000u
#define SCE_PS       0x010000u

#define DIRECT_BUTTONS 0x0Cu
#define DIRECT_STICKS  0x10u
#define DIRECT_L2      0x14u
#define DIRECT_R2      0x15u
#define DIRECT_ACTIVE  0x17u
#define DIRECT_MOTION  0x78u
#define TRIGGER_DIGITAL 128u

typedef struct direct_hid_request {
    uint32_t command;
    uint32_t reserved;
    void *data;
    void *out;
} direct_hid_request_t;

typedef struct direct_hid_delete {
    uint8_t command;
    uint8_t padding[7];
    uint32_t handle;
    uint32_t padding2;
} direct_hid_delete_t;

typedef struct sce_pad_touch {
    uint16_t x;
    uint16_t y;
    uint8_t id;
    uint8_t reserved[3];
} sce_pad_touch_t;

typedef struct sce_pad_touch_data {
    uint8_t touch_num;
    uint8_t reserved[3];
    uint32_t reserved2;
    sce_pad_touch_t touch[2];
} sce_pad_touch_data_t;

typedef struct sce_pad_data {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t padding[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    sce_pad_touch_data_t touch_data;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t extension_unit_data[16];
    uint8_t connected_count;
    uint8_t reserved[2];
    uint8_t device_unique_data_len;
    uint8_t device_unique_data[12];
} sce_pad_data_t;

_Static_assert(sizeof(sce_pad_data_t) == 120u,
               "sce_pad_data_t must be 120 bytes");
_Static_assert(offsetof(sce_pad_data_t, timestamp) == 80u,
               "sce_pad_data_t timestamp offset");

int32_t sceUserServiceInitialize(void *params);
int32_t sceUserServiceGetInitialUser(int32_t *user);
int32_t sceUserServiceGetForegroundUser(int32_t *user);
int32_t sceUserServiceGetLoginUserIdList(int32_t users[4]);
int32_t scePadInit(void);
int32_t scePadSetProcessPrivilege(int32_t privilege);
int32_t scePadVirtualDeviceAddDevice(void *params, int32_t device_type);
int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data);
int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);

uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

typedef int32_t (*mbus_bind_fn)(uint64_t device_id, int32_t user);

enum vpad_state {
    VPAD_QUEUED,
    VPAD_PENDING,
    VPAD_READY,
    VPAD_FAILED,
    VPAD_DIRECT
};

struct ps5_vpad {
    unsigned slot;
    enum vpad_state state;
    int direct_fd;
    int remove_when_found;
    int32_t handle;
    uint64_t device_id;
    dualsense_state_t last;
    int has_last;
    long ps_until;
    int ps_active;
};

static ps5_vpad_t *g_pad[MAX_VPADS];
static int g_system_ready;
static int32_t g_user = -1;
static mbus_bind_fn g_bind;
static void *g_mbus_library;

static ps5_vpad_t *g_pending;
static long g_add_time;
static long g_match_time;
static unsigned g_matches;
static uint64_t g_match_device;
static int g_log_fd = -1;
static int g_log_is_device;
static char g_log_line[KLOG_LINE_SIZE];
static size_t g_log_line_length;

static uint64_t g_original_authid;
static uint8_t g_original_caps[16];
static int g_have_original_credentials;
static int g_credentials_raised;

static long now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (long)((uint64_t)value.tv_sec * 1000u +
                  (uint64_t)value.tv_nsec / 1000000u);
}

static uint32_t map_buttons(const dualsense_state_t *state, int include_ps) {
    uint32_t result = 0u;
    if (state->buttons & DS_BTN_SQUARE) result |= SCE_SQUARE;
    if (state->buttons & DS_BTN_CROSS) result |= SCE_CROSS;
    if (state->buttons & DS_BTN_CIRCLE) result |= SCE_CIRCLE;
    if (state->buttons & DS_BTN_TRIANGLE) result |= SCE_TRIANGLE;
    if (state->buttons & DS_BTN_L1) result |= SCE_L1;
    if (state->buttons & DS_BTN_R1) result |= SCE_R1;
    if (state->buttons & DS_BTN_L3) result |= SCE_L3;
    if (state->buttons & DS_BTN_R3) result |= SCE_R3;
    if (state->buttons & DS_BTN_CREATE) result |= SCE_CREATE;
    if (state->buttons & DS_BTN_OPTIONS) result |= SCE_OPTIONS;
    if (include_ps && (state->buttons & DS_BTN_PS)) result |= SCE_PS;
    if (state->l2 >= TRIGGER_DIGITAL) result |= SCE_L2;
    if (state->r2 >= TRIGGER_DIGITAL) result |= SCE_R2;
    switch (state->dpad) {
        case DS_DPAD_UP: result |= SCE_UP; break;
        case DS_DPAD_UP_RIGHT: result |= SCE_UP | SCE_RIGHT; break;
        case DS_DPAD_RIGHT: result |= SCE_RIGHT; break;
        case DS_DPAD_DOWN_RIGHT: result |= SCE_DOWN | SCE_RIGHT; break;
        case DS_DPAD_DOWN: result |= SCE_DOWN; break;
        case DS_DPAD_DOWN_LEFT: result |= SCE_DOWN | SCE_LEFT; break;
        case DS_DPAD_LEFT: result |= SCE_LEFT; break;
        case DS_DPAD_UP_LEFT: result |= SCE_UP | SCE_LEFT; break;
        case DS_DPAD_NEUTRAL: default: break;
    }
    return result;
}

static int direct_submit(ps5_vpad_t *pad, const dualsense_state_t *state) {
    unsigned char report[DIRECT_REPORT_SIZE];
    direct_hid_request_t request;
    uint32_t buttons;
    float identity = 1.0f;
    memset(report, 0, sizeof(report));
    buttons = map_buttons(state, 0);
    memcpy(report + DIRECT_BUTTONS, &buttons, sizeof(buttons));
    report[DIRECT_STICKS + 0u] = state->left_x;
    report[DIRECT_STICKS + 1u] = state->left_y;
    report[DIRECT_STICKS + 2u] = state->right_x;
    report[DIRECT_STICKS + 3u] = state->right_y;
    report[DIRECT_L2] = state->l2;
    report[DIRECT_R2] = state->r2;
    report[DIRECT_ACTIVE] = 1u;
    memcpy(report + DIRECT_MOTION + 12u, &identity, sizeof(identity));
    memset(&request, 0, sizeof(request));
    request.command = (uint32_t)pad->handle;
    request.data = report;
    return ioctl(pad->direct_fd, DIRECT_HID_INSERT_DATA, &request) == 0;
}

static int direct_initialize(ps5_vpad_t *pad, char *why, int why_length) {
    unsigned char descriptor[DIRECT_DESCRIPTOR_SIZE];
    direct_hid_request_t request;
    uint32_t user = 1u;
    pad->direct_fd = open("/dev/hid", O_RDWR);
    if (pad->direct_fd < 0) {
        snprintf(why, (size_t)why_length, "/dev/hid: %s", strerror(errno));
        return 0;
    }
    memset(descriptor, 0, sizeof(descriptor));
    descriptor[0] = 3u;
    memcpy(descriptor + 0x1Cu, &user, sizeof(user));
    descriptor[0x20] = 1u;
    pad->handle = -1;
    memset(&request, 0, sizeof(request));
    request.command = DIRECT_HID_CMD_ADD;
    request.data = descriptor;
    request.out = &pad->handle;
    if (ioctl(pad->direct_fd, DIRECT_HID_ADD_DEVICE, &request) != 0) {
        snprintf(why, (size_t)why_length, "AddDevice: %s", strerror(errno));
        close(pad->direct_fd);
        pad->direct_fd = -1;
        return 0;
    }
    pad->state = VPAD_DIRECT;
    snprintf(why, (size_t)why_length,
             "direct virtual DualSense handle %d (PS unavailable)", pad->handle);
    return 1;
}

static ps5_vpad_t *direct_open(unsigned slot, char *why, int why_length) {
    ps5_vpad_t *pad = calloc(1u, sizeof(*pad));
    if (pad == NULL) return NULL;
    pad->slot = slot;
    pad->direct_fd = -1;
    if (!direct_initialize(pad, why, why_length)) {
        free(pad);
        return NULL;
    }
    return pad;
}

static int set_credentials(uint64_t authid, const uint8_t caps[16]) {
    uint8_t current[16];
    pid_t pid = getpid();
    return kernel_set_ucred_authid(pid, authid) == 0 &&
           kernel_set_ucred_caps(pid, caps) == 0 &&
           kernel_get_ucred_authid(pid) == authid &&
           kernel_get_ucred_caps(pid, current) == 0 &&
           memcmp(current, caps, sizeof(current)) == 0;
}

static int restore_credentials(void) {
    if (!g_have_original_credentials || !g_credentials_raised) return 1;
    if (!set_credentials(g_original_authid, g_original_caps)) return 0;
    g_credentials_raised = 0;
    return 1;
}

static int raise_credentials(void) {
    static const uint8_t caps[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    pid_t pid = getpid();
    if (!g_have_original_credentials) {
        g_original_authid = kernel_get_ucred_authid(pid);
        if (g_original_authid == 0u ||
            kernel_get_ucred_caps(pid, g_original_caps) != 0) return 0;
        g_have_original_credentials = 1;
    }
    if (!set_credentials(0x3800000000010003ull, caps)) {
        (void)set_credentials(g_original_authid, g_original_caps);
        return 0;
    }
    g_credentials_raised = 1;
    return 1;
}

static int32_t choose_user(void) {
    int32_t users[4] = {-1, -1, -1, -1};
    int32_t initial = -1;
    int32_t foreground = -1;
    if (sceUserServiceGetLoginUserIdList(users) != 0)
        users[0] = users[1] = users[2] = users[3] = -1;
    if (sceUserServiceGetInitialUser(&initial) != 0) initial = -1;
    if (sceUserServiceGetForegroundUser(&foreground) != 0) foreground = -1;
    for (unsigned i = 0u; i < 4u; ++i)
        if (users[i] != 0 && users[i] != -1 && users[i] == initial)
            return initial;
    for (unsigned i = 0u; i < 4u; ++i)
        if (users[i] != 0 && users[i] != -1 && users[i] == foreground)
            return foreground;
    for (unsigned i = 0u; i < 4u; ++i)
        if (users[i] != 0 && users[i] != -1) return users[i];
    return -1;
}

int ps5_vpad_system_init(char *why, int why_length) {
    static const char *const mbus_paths[] = {
        "/system/common/lib/libSceMbus.sprx", "libSceMbus.sprx"
    };
    int32_t result;
    if (g_system_ready) {
        snprintf(why, (size_t)why_length, "libScePad/MBus already ready");
        return 1;
    }
    if (!raise_credentials()) {
        snprintf(why, (size_t)why_length, "could not raise system credentials");
        return 0;
    }
    result = sceUserServiceInitialize(NULL);
    if (result != 0)
        ps5_log("vpad: sceUserServiceInitialize returned %#x (already initialized is OK)",
                (unsigned)result);
    for (unsigned i = 0u;
         i < sizeof(mbus_paths) / sizeof(mbus_paths[0]) && g_mbus_library == NULL;
         ++i)
        g_mbus_library = dlopen(mbus_paths[i], RTLD_NOW | RTLD_GLOBAL);
    if (g_mbus_library != NULL)
        g_bind = (mbus_bind_fn)dlsym(g_mbus_library,
                                     "sceMbusBindDeviceWithUserId");
    if (g_bind == NULL) {
        snprintf(why, (size_t)why_length, "libSceMbus/bind symbol unavailable");
        (void)restore_credentials();
        return 0;
    }
    result = scePadInit();
    if (result != 0) {
        snprintf(why, (size_t)why_length, "scePadInit failed: %#x",
                 (unsigned)result);
        (void)restore_credentials();
        return 0;
    }
    result = scePadSetProcessPrivilege(1);
    if (result != 0)
        ps5_log("vpad: scePadSetProcessPrivilege returned %#x, continuing",
                (unsigned)result);
    g_user = choose_user();
    g_system_ready = 1;
    snprintf(why, (size_t)why_length, "libScePad/MBus, user %#x",
             (unsigned)g_user);
    return 1;
}

static int log_source_open(void) {
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons(PS5_VPAD_LOG_PORT);
        address.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            g_log_is_device = 0;
            return fd;
        }
        close(fd);
    }
    fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        g_log_is_device = 1;
        return fd;
    }
    ps5_log("vpad: no kernel log source (TCP 3232 and /dev/klog unavailable: %s)",
            strerror(errno));
    return -1;
}

static void log_source_close(void) {
    if (g_log_fd >= 0) close(g_log_fd);
    g_log_fd = -1;
    g_log_line_length = 0u;
}

static uint64_t hex_after(const char *line, const char *key) {
    const char *position = strstr(line, key);
    uint64_t value = 0u;
    unsigned digits = 0u;
    if (position == NULL) return 0u;
    position += strlen(key);
    while (*position == ':' || *position == '=' || *position == ' ') position++;
    if (position[0] == '0' && (position[1] == 'x' || position[1] == 'X'))
        position += 2;
    while (isxdigit((unsigned char)*position) && digits++ < 16u) {
        char c = *position++;
        value = (value << 4) |
            (uint64_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return value;
}

static uint64_t virtual_device_in(const char *line) {
    const char *battery;
    if (strstr(line, "DEVICE_ADDED") == NULL ||
        strstr(line, "type:1") == NULL ||
        strstr(line, "subType:22") == NULL) return 0u;
    battery = strstr(line, "capabilityBattery:");
    if (battery != NULL && battery[18] != '0') return 0u;
    return hex_after(line, "DeviceId");
}

static void read_log_source(void) {
    char buffer[1024];
    for (unsigned round = 0u; round < 8u && g_log_fd >= 0; ++round) {
        ssize_t count = read(g_log_fd, buffer, sizeof(buffer));
        if (count <= 0) return;
        for (ssize_t i = 0; i < count; ++i) {
            uint64_t device;
            if (buffer[i] != '\n' && g_log_line_length + 1u < sizeof(g_log_line)) {
                g_log_line[g_log_line_length++] = buffer[i];
                continue;
            }
            g_log_line[g_log_line_length] = '\0';
            g_log_line_length = 0u;
            if (g_log_is_device) ps5_log("klog: %s", g_log_line);
            device = virtual_device_in(g_log_line);
            if (device == 0u || g_pending == NULL) continue;
            if (g_matches == 0u) {
                g_match_device = device;
                g_match_time = now_ms();
            }
            if (g_matches == 0u || device != g_match_device) g_matches++;
        }
    }
}

static int send_system_frame(ps5_vpad_t *pad,
                             const dualsense_state_t *state) {
    sce_pad_data_t data;
    struct timespec timestamp;
    memset(&data, 0, sizeof(data));
    data.buttons = map_buttons(state, 1);
    if (pad->ps_until != 0) data.buttons |= SCE_PS;
    data.lx = state->left_x;
    data.ly = state->left_y;
    data.rx = state->right_x;
    data.ry = state->right_y;
    data.l2 = state->l2;
    data.r2 = state->r2;
    data.orientation[3] = 1.0f;
    data.acceleration[1] = -1.0f;
    data.connected = 1u;
    data.connected_count = 1u;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    data.timestamp = (uint64_t)timestamp.tv_sec * 1000000u +
                     (uint64_t)timestamp.tv_nsec / 1000u;
    return scePadVirtualDeviceInsertData(pad->handle, &data) == 0;
}

static void free_pad(ps5_vpad_t *pad) {
    if (pad == NULL) return;
    if (pad->slot < MAX_VPADS && g_pad[pad->slot] == pad)
        g_pad[pad->slot] = NULL;
    free(pad);
}

static void fallback_to_direct(ps5_vpad_t *pad, const char *reason) {
    char why[128];
    if (direct_initialize(pad, why, (int)sizeof(why))) {
        ps5_log("slot %u: %s; fallback: %s", pad->slot + 1u, reason, why);
    } else {
        pad->state = VPAD_FAILED;
        ps5_log("slot %u: %s; direct fallback failed: %s", pad->slot + 1u,
                reason, why);
    }
}

static void start_identification(ps5_vpad_t *pad) {
    int32_t parameters[8];
    int32_t result;
    if (g_user == -1) g_user = choose_user();
    if (g_user == -1) {
        fallback_to_direct(pad, "nobody signed in for MBus binding");
        return;
    }
    if (!g_credentials_raised && !raise_credentials()) {
        fallback_to_direct(pad, "cannot raise MBus credentials");
        return;
    }
    g_log_fd = log_source_open();
    if (g_log_fd < 0) {
        fallback_to_direct(pad, "cannot identify MBus virtual pad");
        return;
    }
    read_log_source();
    g_matches = 0u;
    g_match_device = 0u;
    for (unsigned i = 0u; i < 8u; ++i) parameters[i] = (int32_t)0xdeadbeef;
    parameters[0] = (int32_t)sizeof(parameters);
    parameters[1] = 1;
    result = scePadVirtualDeviceAddDevice(parameters, VIRTUAL_DEVICE_DUALSENSE);
    ps5_log("slot %u: scePad AddDevice %#x", pad->slot + 1u,
            (unsigned)result);
    g_pending = pad;
    g_add_time = now_ms();
    pad->state = VPAD_PENDING;
}

static void finish_identification(long now) {
    ps5_vpad_t *pad = g_pending;
    int32_t result;
    if (pad == NULL) return;
    if (g_matches == 0u && now - g_add_time < IDENTIFY_TIMEOUT_MS) return;
    if (g_matches == 1u && now - g_match_time < AMBIGUITY_WAIT_MS &&
        now - g_add_time < IDENTIFY_TIMEOUT_MS) return;
    g_pending = NULL;
    log_source_close();
    if (g_matches != 1u) {
        ps5_log("slot %u: virtual pad not identified (%u candidates)",
                pad->slot + 1u, g_matches);
        if (pad->remove_when_found) free_pad(pad);
        else fallback_to_direct(pad, "MBus identification was ambiguous");
        return;
    }
    pad->device_id = g_match_device;
    pad->handle = (int32_t)(g_match_device & 0xffffffffu);
    if (pad->remove_when_found) {
        (void)scePadVirtualDeviceDeleteDevice(pad->handle);
        ps5_log("slot %u: pending virtual pad removed", pad->slot + 1u);
        free_pad(pad);
        return;
    }
    result = g_bind(pad->device_id, g_user);
    ps5_log("slot %u: device %#llx bound to user %#x: %#x",
            pad->slot + 1u, (unsigned long long)pad->device_id,
            (unsigned)g_user, (unsigned)result);
    if (result != 0) {
        (void)scePadVirtualDeviceDeleteDevice(pad->handle);
        fallback_to_direct(pad, "MBus user binding failed");
        return;
    }
    pad->state = VPAD_READY;
    ps5_log("slot %u: system virtual DualSense handle %d (PS enabled)",
            pad->slot + 1u, pad->handle);
    if (pad->has_last && !send_system_frame(pad, &pad->last)) {
        ps5_log("slot %u: virtual DualSense refused initial input",
                pad->slot + 1u);
        (void)scePadVirtualDeviceDeleteDevice(pad->handle);
        fallback_to_direct(pad, "system virtual pad refused input");
    }
}

static void ps_tick(long now) {
    for (unsigned i = 0u; i < MAX_VPADS; ++i) {
        ps5_vpad_t *pad = g_pad[i];
        if (pad == NULL || pad->state != VPAD_READY || !pad->ps_active ||
            !pad->has_last) continue;
        if (pad->ps_until != 0 && now >= pad->ps_until) pad->ps_until = 0;
        if (!send_system_frame(pad, &pad->last)) {
            ps5_log("slot %u: virtual DualSense refused PS frame", i + 1u);
            pad->state = VPAD_FAILED;
        }
        if (pad->ps_until == 0) pad->ps_active = 0;
    }
}

void ps5_vpad_system_poll(void) {
    long now;
    if (!g_system_ready) return;
    now = now_ms();
    ps_tick(now);
    if (g_pending != NULL) {
        read_log_source();
        finish_identification(now);
        return;
    }
    for (unsigned i = 0u; i < MAX_VPADS; ++i)
        if (g_pad[i] != NULL && g_pad[i]->state == VPAD_QUEUED) {
            start_identification(g_pad[i]);
            return;
        }
}

ps5_vpad_t *ps5_vpad_open(unsigned slot, char *why, int why_length) {
    ps5_vpad_t *pad;
    if (slot >= MAX_VPADS) {
        snprintf(why, (size_t)why_length, "invalid virtual pad slot %u", slot);
        return NULL;
    }
    if (g_pad[slot] != NULL) {
        snprintf(why, (size_t)why_length, "virtual pad slot %u is busy", slot + 1u);
        return NULL;
    }
    if (!g_system_ready) {
        pad = direct_open(slot, why, why_length);
        if (pad != NULL) g_pad[slot] = pad;
        return pad;
    }
    pad = calloc(1u, sizeof(*pad));
    if (pad == NULL) {
        snprintf(why, (size_t)why_length, "out of memory");
        return NULL;
    }
    pad->slot = slot;
    pad->state = VPAD_QUEUED;
    pad->direct_fd = -1;
    pad->handle = -1;
    g_pad[slot] = pad;
    snprintf(why, (size_t)why_length, "system virtual DualSense queued");
    return pad;
}

enum ps5_vpad_submit_result ps5_vpad_submit(
    ps5_vpad_t *pad, const dualsense_state_t *state) {
    if (pad == NULL || state == NULL) return PS5_VPAD_SUBMIT_FAILED;
    if (pad->state == VPAD_DIRECT)
        return direct_submit(pad, state) ? PS5_VPAD_SUBMIT_DELIVERED
                                         : PS5_VPAD_SUBMIT_FAILED;
    pad->last = *state;
    pad->has_last = 1;
    if (pad->state == VPAD_QUEUED || pad->state == VPAD_PENDING)
        return PS5_VPAD_SUBMIT_COALESCED;
    if (pad->state != VPAD_READY) return PS5_VPAD_SUBMIT_FAILED;
    return send_system_frame(pad, state) ? PS5_VPAD_SUBMIT_DELIVERED
                                         : PS5_VPAD_SUBMIT_FAILED;
}

int ps5_vpad_press_ps(ps5_vpad_t *pad, int milliseconds) {
    if (pad == NULL || pad->state != VPAD_READY || !pad->has_last ||
        milliseconds <= 0) return 0;
    pad->ps_until = now_ms() + milliseconds;
    pad->ps_active = 1;
    return send_system_frame(pad, &pad->last);
}

int ps5_vpad_is_ready(const ps5_vpad_t *pad) {
    return pad != NULL &&
           (pad->state == VPAD_READY || pad->state == VPAD_DIRECT);
}

int ps5_vpad_has_ps(const ps5_vpad_t *pad) {
    return pad != NULL && pad->state == VPAD_READY;
}

void ps5_vpad_close(ps5_vpad_t *pad) {
    if (pad == NULL) return;
    if (pad->state == VPAD_PENDING) {
        pad->remove_when_found = 1;
        return;
    }
    if (pad->state == VPAD_READY) {
        (void)scePadVirtualDeviceDeleteDevice(pad->handle);
        ps5_log("slot %u: system virtual DualSense removed", pad->slot + 1u);
    } else if (pad->state == VPAD_DIRECT && pad->direct_fd >= 0) {
        direct_hid_delete_t request;
        memset(&request, 0, sizeof(request));
        request.command = DIRECT_HID_CMD_DEL;
        request.handle = (uint32_t)pad->handle;
        (void)ioctl(pad->direct_fd, DIRECT_HID_DEL_DEVICE, &request);
        close(pad->direct_fd);
    }
    free_pad(pad);
}

void ps5_vpad_system_close(void) {
    log_source_close();
    g_pending = NULL;
    for (unsigned i = 0u; i < MAX_VPADS; ++i) {
        if (g_pad[i] == NULL) continue;
        if (g_pad[i]->state == VPAD_READY)
            (void)scePadVirtualDeviceDeleteDevice(g_pad[i]->handle);
        else if (g_pad[i]->state == VPAD_DIRECT && g_pad[i]->direct_fd >= 0) {
            direct_hid_delete_t request;
            memset(&request, 0, sizeof(request));
            request.command = DIRECT_HID_CMD_DEL;
            request.handle = (uint32_t)g_pad[i]->handle;
            (void)ioctl(g_pad[i]->direct_fd, DIRECT_HID_DEL_DEVICE, &request);
            close(g_pad[i]->direct_fd);
        }
        free_pad(g_pad[i]);
    }
    if (!restore_credentials()) ps5_log("vpad: failed to restore credentials");
    if (g_mbus_library != NULL) dlclose(g_mbus_library);
    g_mbus_library = NULL;
    g_bind = NULL;
    g_system_ready = 0;
}
