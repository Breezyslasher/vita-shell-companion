/**
 * A module built exactly as j117mw3/ps4-hen-rest builds the one it loads
 * into SceShellUI (keeper/Makefile): GoldHEN_Plugins_SDK's crt/crtprx.c,
 * its flags, -lSceLibcInternal -lkernel, -e _init, 16 KB pages. It only
 * shows a notification as it starts, so that the test app can tell
 * whether the shell loads a module made this way at all.
 */

#include <stdint.h>
#include <string.h>

typedef struct {
    int32_t type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    uint8_t useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
} NotifyRequest;

int sceKernelSendNotificationRequest(int device, NotifyRequest* req, size_t size, int blocking);

int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    static NotifyRequest req;
    memset(&req, 0, sizeof(req));
    req.targetId = -1;
    strcpy(req.message, "Shell Companion: test module running");
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    return 0;
}

int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}
