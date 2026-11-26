/**
 * The PS4 background music loader: a raw payload, sent to GoldHEN's
 * BinLoader, that writes the module (bgm.c, built into it) to
 * /data/ShellCompanion/ps4/sc_bgm.prx and has GoldHEN load it into
 * SceShellUI (its SDK's system call 500, PROCESS_PRX_LOAD), as the shell
 * probe's loader does. If a player is running already, its heartbeat in
 * status.txt fresh, it is told to play its queue again instead. Its log is
 * ps4/bgm-load.log.
 */

#include "../probe/sys.h"
#include "bgm.h"

__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global g_prxStart\n"
        ".hidden g_prxStart\n"
        "g_prxStart:\n"
        ".incbin \"" SC_PRX_PATH "\"\n"
        ".global g_prxEnd\n"
        ".hidden g_prxEnd\n"
        "g_prxEnd:\n"
        ".text\n");
extern const uint8_t g_prxStart[] __attribute__((visibility("hidden")));
extern const uint8_t g_prxEnd[] __attribute__((visibility("hidden")));

struct ProcPrxLoad {
    char processName[32];
    char prxPath[100];
    uint64_t res;
} __attribute__((packed));  // the SDK's proc_prx_load

static void writeFile(const char* path, const char* text) {
    const int fd = sysOpen(path, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (fd < 0) return;
    sysWrite(fd, text, strlen(text));
    sysClose(fd);
}

// The heartbeat a running player writes in status.txt, in ms, or 0.
static uint64_t heartbeat(void) {
    char text[1024];
    const int fd = sysOpen(SC_BGM_STATUS, SC_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sysRead(fd, text, sizeof(text) - 1);
    sysClose(fd);
    if (n <= 0) return 0;
    text[n] = 0;
    for (int i = 0; i + 8 <= n; i++) {
        if ((i == 0 || text[i - 1] == '\n') && !memcmp(text + i, "beat_ms=", 8)) {
            uint64_t v = 0;
            for (int k = i + 8; text[k] >= '0' && text[k] <= '9'; k++) v = v * 10 + (uint64_t)(text[k] - '0');
            return v;
        }
    }
    return 0;
}

int probeMain(void) {
    sysMkdir("/data/ShellCompanion", 0777);
    sysMkdir(SC_BGM_DIR, 0777);
    sysMkdir(SC_BGM_MUSIC, 0777);
    logOpen("ps4/bgm-load.log");
    char fw[64];
    firmware(fw, sizeof(fw));
    logLine("background music loader %s: firmware %s", SC_BGM_VERSION, fw);

    const uint64_t beat = heartbeat(), now = sysNowUs() / 1000u;
    logLine("heartbeat %lu ms, now %lu ms", beat, now);
    if (beat && now >= beat && now - beat < 4000) {
        writeFile(SC_BGM_CMD ".new", "reload\nplay\n");
        sysRename(SC_BGM_CMD ".new", SC_BGM_CMD);
        logLine("a player is running: told to play its queue again");
        notify("Background music: already running, playing the queue again");
        return 0;
    }

    const long version = sysCall(500, 0 /* VERSION */, 0, 0, 0, 0, 0);
    logLine("GoldHEN SDK version: 0x%lX", (uint64_t)version);
    const uint32_t size = (uint32_t)(g_prxEnd - g_prxStart);
    const int fd = sysOpen(SC_BGM_PRX, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    const int wrote = fd >= 0 ? sysWrite(fd, g_prxStart, size) : fd;
    if (fd >= 0) sysClose(fd);
    logLine("%s: %u bytes, written %d", SC_BGM_PRX, size, wrote);
    if (wrote != (int)size) {
        notify("Background music: the module was not written (%d)", wrote);
        return 1;
    }

    static struct ProcPrxLoad args;
    memset(&args, 0, sizeof(args));
    memcpy(args.processName, "SceShellUI", 11);
    memcpy(args.prxPath, SC_BGM_PRX, sizeof(SC_BGM_PRX));
    const long rc = sysCall(500, 6 /* PROCESS_PRX_LOAD */, (long)&args, 0, 0, 0, 0);
    logLine("PROCESS_PRX_LOAD: 0x%lX, its result 0x%lX", (uint64_t)rc, (uint64_t)args.res);
    if (rc != 0) notify("Background music: GoldHEN would not load it (0x%lX)", (uint64_t)rc);
    return 0;
}
