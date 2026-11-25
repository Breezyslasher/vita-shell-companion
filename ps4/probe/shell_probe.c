/**
 * PS4 probe 3, inside the shell: a module GoldHEN loads into SceShellUI
 * (inject_probe.c asks it to). Payloads were heard on no audio port on
 * GoldHEN 2.4b18.6, firmware 11.50, though each took its output at the
 * hardware's pace, and the system music service never came back from its
 * initialisation in one. The shell is always heard and owns the quick menu,
 * so this asks:
 *
 * 1. Is audio played from inside SceShellUI heard? The same 440 Hz tone,
 *    counted in beeps, 20 s each: 1 beep MAIN, 2 BGM, 3 type 14.
 * 2. Which modules does SceShellUI have loaded, and which has Mono's
 *    mono_get_root_domain? Its quick menu is Mono (C#) code, which a later
 *    step would reach through that runtime.
 *
 * module_start returns at once; the work runs on a thread of its own, which
 * a real process like the shell can start. Its log is
 * /data/ShellCompanion/ps4probe-shell.log.
 */

#include "sys.h"

typedef int32_t (*FnOpen)(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                          uint32_t param);
typedef int32_t (*FnOutput)(int32_t handle, const void* p);
typedef int32_t (*FnClose)(int32_t handle);
typedef int32_t (*FnSetVolume)(int32_t handle, int32_t flag, int32_t* vol);
typedef int32_t (*FnInit)(void);

#define GRAIN 256
#define RATE 48000

static int16_t g_buf[GRAIN * 2];
static uint32_t g_samples;
static uint16_t g_phase;

// A grain of 440 Hz, counted out in `beeps` beeps of 150 ms in each 3 s.
static void fill(int beeps) {
    const uint32_t step = 440u * 65536u / RATE;
    for (int i = 0; i < GRAIN; i++) {
        int16_t v = 0;
        const uint32_t at = g_samples % (RATE * 3);
        const uint32_t slot = at / (RATE * 3 / 10);
        if ((int)slot < beeps && at % (RATE * 3 / 10) < RATE * 15 / 100) {
            g_phase = (uint16_t)(g_phase + step);
            const int tri = g_phase < 32768 ? g_phase : 65535 - g_phase;
            v = (int16_t)((tri - 16384) / 3);
        }
        g_buf[2 * i] = g_buf[2 * i + 1] = v;
        g_samples++;
    }
}

// The loaded modules, by name, and which has mono_get_root_domain.
static void listModules(void) {
    int handles[256], count = 0;
    const long rc = sysCall(592, (long)handles, 256, (long)&count, 0, 0, 0);
    logLine("modules: %d (0x%lX)", count, (uint64_t)rc);
    for (int i = 0; i < count && i < 256; i++) {
        struct {
            uint64_t size;
            char name[256];
            uint8_t rest[0x160 - 8 - 256];
        } info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        const long rcInfo = sysCall(593, handles[i], (long)&info, 0, 0, 0, 0);
        info.name[255] = 0;
        void* mono = libSym(handles[i], "mono_get_root_domain");
        logLine("  0x%X %s%s (0x%lX)", handles[i], rcInfo >= 0 ? info.name : "?",
                mono ? ": has mono_get_root_domain" : "", (uint64_t)rcInfo);
    }
}

static void beepsOn(FnOpen open, FnOutput output, FnClose close, FnSetVolume volume,
                    const char* name, int type, int beeps) {
    const int h = open(255, type, 0, GRAIN, RATE, 1 /* S16 stereo */);
    logLine("%s: port type %d opened: 0x%08X", name, type, (unsigned)h);
    if (h < 0) {
        notify("Shell probe: %s would not open (0x%08X)", name, (unsigned)h);
        sysSleepMs(20000);
        return;
    }
    int32_t vol[8];
    for (int i = 0; i < 8; i++) vol[i] = 32768;
    const int32_t rcVol = volume ? volume(h, 0xFF, vol) : -1;
    notify("Shell probe: %s. Count %d beeps", name, beeps);
    g_samples = 0;
    int errors = 0;
    const uint64_t began = sysNowUs();
    for (int g = 0; g < 20 * RATE / GRAIN; g++) {
        fill(beeps);
        if (output(h, g_buf) < 0) errors++;
    }
    logLine("%s: volume 0x%08X; 20 s in %u ms, %d outputs failed; closed 0x%08X", name,
            (unsigned)rcVol, (unsigned)((sysNowUs() - began) / 1000u), errors, (unsigned)close(h));
}

static void* work(void* arg) {
    (void)arg;
    notify("Shell probe: running inside the shell");
    listModules();
    const int lib = libLoad("libSceAudioOut.sprx");
    FnInit init = (FnInit)libSym(lib, "sceAudioOutInit");
    FnOpen open = (FnOpen)libSym(lib, "sceAudioOutOpen");
    FnOutput output = (FnOutput)libSym(lib, "sceAudioOutOutput");
    FnClose close = (FnClose)libSym(lib, "sceAudioOutClose");
    FnSetVolume volume = (FnSetVolume)libSym(lib, "sceAudioOutSetVolume");
    logLine("libSceAudioOut: 0x%X; init %p, open %p, output %p", lib, (void*)init, (void*)open,
            (void*)output);
    if (init && open && output && close) {
        logLine("sceAudioOutInit: 0x%08X", (unsigned)init());
        beepsOn(open, output, close, volume, "1: MAIN", 0, 1);
        beepsOn(open, output, close, volume, "2: BGM", 1, 2);
        beepsOn(open, output, close, volume, "3: type 14", 14, 3);
    }
    logLine("shell probe: done");
    notify("Shell probe done: send /data/ShellCompanion/ps4probe-shell.log");
    return NULL;
}

// One import through the module's import table, for its .got.plt, which a
// .prx must have (OpenOrbis's link.x; create-lib stopped short without it).
// The system's loader resolves it, inside the shell.
int sceKernelUsleep(unsigned int us);

// The module's entry, from _init (prx_crt.c): log, start the work, return.
__attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    logOpen("ps4probe-shell.log");
    sceKernelUsleep(0);
    char fw[64];
    firmware(fw, sizeof(fw));
    logLine("shell probe: started in pid %ld, firmware %s", sysCall(20, 0, 0, 0, 0, 0, 0), fw);
    const int rc = threadStart(work, NULL, "ScShellProbe");
    logLine("its thread: 0x%08X", (unsigned)rc);
    return 0;
}

__attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}
