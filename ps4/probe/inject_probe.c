/**
 * PS4 probe 3's loader: a raw payload, sent to GoldHEN's BinLoader, that
 * has GoldHEN load the shell probe (shell_probe.c, built into this payload)
 * into SceShellUI, through its SDK's system call: 500, command 6,
 * GOLDHEN_SDK_CMD_PROCESS_PRX_LOAD (GoldHEN/GoldHEN_Plugins_SDK,
 * include/GoldHEN.h). It writes the module to /data/ShellCompanion first.
 * Its log is ps4probe-inject.log there; the module's, ps4probe-shell.log.
 */

#include "sys.h"

// The module, built first, inside this payload: reached relative to the
// code, as a raw payload has no relocations.
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
// Hidden, so reached relative to the code, not through the GOT, whose
// entries a raw payload would have nobody to fill in.
extern const uint8_t g_prxStart[] __attribute__((visibility("hidden")));
extern const uint8_t g_prxEnd[] __attribute__((visibility("hidden")));

#define PRX "/data/ShellCompanion/sc_shellprobe.prx"

struct ProcPrxLoad {
    char processName[32];
    char prxPath[100];
    uint64_t res;
} __attribute__((packed));  // the SDK's proc_prx_load

static long sdk(uint64_t cmd, void* data) {
    return sysCall(500, (long)cmd, (long)data, 0, 0, 0, 0);
}

int probeMain(void) {
    logOpen("ps4probe-inject.log");
    char fw[64];
    firmware(fw, sizeof(fw));
    logLine("inject probe: firmware %s", fw);
    const long version = sdk(0 /* VERSION */, NULL);
    logLine("GoldHEN SDK version: 0x%lX", (uint64_t)version);

    const uint32_t size = (uint32_t)(g_prxEnd - g_prxStart);
    sysMkdir("/data/ShellCompanion", 0777);
    const int fd = sysOpen(PRX, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    const int wrote = fd >= 0 ? sysWrite(fd, g_prxStart, size) : fd;
    if (fd >= 0) sysClose(fd);
    logLine("%s: %u bytes, written %d", PRX, size, wrote);
    if (wrote != (int)size) {
        notify("Inject probe: the module was not written (%d)", wrote);
        return 1;
    }

    static struct ProcPrxLoad args;
    memset(&args, 0, sizeof(args));
    memcpy(args.processName, "SceShellUI", 11);
    memcpy(args.prxPath, PRX, sizeof(PRX));
    logLine("asking GoldHEN to load it into SceShellUI");
    const long rc = sdk(6 /* PROCESS_PRX_LOAD */, &args);
    logLine("PROCESS_PRX_LOAD: 0x%lX, its result 0x%lX", (uint64_t)rc, (uint64_t)args.res);
    notify("Inject probe: loaded into the shell? 0x%lX / 0x%lX", (uint64_t)rc, (uint64_t)args.res);
    return 0;
}
