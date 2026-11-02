#include "ps5_log.h"
#include "ps5_vpad.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t g_authid = 0x4800000000000001ull;
static uint8_t g_caps[16];
static int g_bound;
static int g_deleted;
static unsigned g_insertions;
static uint32_t g_last_buttons;

int32_t sceUserServiceInitialize(void *params) { (void)params; return 0; }
int32_t sceUserServiceGetInitialUser(int32_t *user) {
    *user = 0x10000000; return 0;
}
int32_t sceUserServiceGetForegroundUser(int32_t *user) {
    *user = 0x10000000; return 0;
}
int32_t sceUserServiceGetLoginUserIdList(int32_t users[4]) {
    users[0] = 0x10000000; users[1] = users[2] = users[3] = -1; return 0;
}
int32_t scePadInit(void) { return 0; }
int32_t scePadSetProcessPrivilege(int32_t privilege) {
    (void)privilege; return 0;
}
int32_t scePadVirtualDeviceAddDevice(void *params, int32_t device_type) {
    (void)params; assert(device_type == 3); return 0;
}
int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data) {
    assert(handle == (int32_t)0x12345678);
    memcpy(&g_last_buttons, data, sizeof(g_last_buttons));
    g_insertions++;
    return 0;
}
int32_t scePadVirtualDeviceDeleteDevice(int32_t handle) {
    assert(handle == (int32_t)0x12345678); g_deleted++; return 0;
}
uint64_t kernel_get_ucred_authid(pid_t pid) { (void)pid; return g_authid; }
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t authid) {
    (void)pid; g_authid = authid; return 0;
}
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]) {
    (void)pid; memcpy(caps, g_caps, 16u); return 0;
}
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]) {
    (void)pid; memcpy(g_caps, caps, 16u); return 0;
}
int sceKernelSendNotificationRequest(int a, void *b, size_t c, int d) {
    (void)a; (void)b; (void)c; (void)d; return 0;
}

static int32_t fake_bind(uint64_t device, int32_t user) {
    assert(device == 0x12345678u);
    assert(user == 0x10000000);
    g_bound++;
    return 0;
}

void *dlopen(const char *path, int mode) {
    (void)path; (void)mode; return (void *)1;
}
void *dlsym(void *handle, const char *name) {
    (void)handle;
    assert(strcmp(name, "sceMbusBindDeviceWithUserId") == 0);
    return (void *)fake_bind;
}
int dlclose(void *handle) { (void)handle; return 0; }

static int open_log_server(void) {
    struct sockaddr_in address;
    int reuse = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(23232u);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fd, 1) == 0);
    return fd;
}

int main(void) {
    static const char event[] =
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x12345678][type:1]"
        "[subType:22][capabilityBattery:0]\n";
    char why[160];
    dualsense_state_t state = {0};
    ps5_vpad_t *pad;
    int server = open_log_server();
    int client;

    state.dpad = DS_DPAD_NEUTRAL;
    state.left_x = state.left_y = state.right_x = state.right_y = 128u;
    assert(ps5_vpad_system_init(why, (int)sizeof(why)));
    assert(g_authid == 0x3800000000010003ull);
    pad = ps5_vpad_open(0u, why, (int)sizeof(why));
    assert(pad != NULL);
    assert(ps5_vpad_submit(pad, &state));

    ps5_vpad_system_poll();
    client = accept(server, NULL, NULL);
    assert(client >= 0);
    assert(write(client, event, sizeof(event) - 1u) ==
           (ssize_t)(sizeof(event) - 1u));
    usleep(10000);
    ps5_vpad_system_poll();
    usleep(350000);
    ps5_vpad_system_poll();
    assert(ps5_vpad_is_ready(pad));
    assert(g_bound == 1 && g_insertions >= 1u);
    assert((g_last_buttons & 0x00010000u) == 0u);

    state.buttons = DS_BTN_PS;
    assert(ps5_vpad_submit(pad, &state));
    assert((g_last_buttons & 0x00010000u) != 0u);
    state.buttons = 0u;
    assert(ps5_vpad_submit(pad, &state));
    assert(ps5_vpad_press_ps(pad, 50));
    assert((g_last_buttons & 0x00010000u) != 0u);
    usleep(70000);
    ps5_vpad_system_poll();
    assert((g_last_buttons & 0x00010000u) == 0u);

    ps5_vpad_close(pad);
    assert(g_deleted == 1);
    ps5_vpad_system_close();
    assert(g_authid == 0x4800000000000001ull);
    close(client);
    close(server);
    puts("ps5_xb360 vpad tests: OK");
    return 0;
}
