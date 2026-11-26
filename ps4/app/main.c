/**
 * Shell Companion BGM Test: a PS4 app (a .pkg) to try the background music
 * module without GoldHEN's BinLoader. It carries sc_bgm.prx, writes it to
 * /data/ShellCompanion/ps4, has GoldHEN load it into SceShellUI, and drives
 * it with the controller, through the files the module reads (bgm.h). It
 * shows the player's status and the end of its log.
 *
 * Its own log is /data/ShellCompanion/ps4/app.log.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../bgm/bgm.h"
#include "font8x8_basic.h"

#define APP_VERSION "0.16"

// ---- the system's functions, declared here ----
// Not from OpenOrbis's orbis/ headers, which differ between its versions
// (the CI image's would not compile): only what is used, as the system has it.

typedef void* OrbisKernelEqueue;
typedef struct {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    int64_t data;
    void* udata;
} OrbisKernelEvent;
typedef struct {
    uint64_t count, processTime, tsc;
    int64_t flipArg;
    uint64_t submitTsc, reserved0;
    int32_t gpuPending, flipPending, currentBuffer;
    uint32_t reserved1;
} OrbisVideoOutFlipStatus;
typedef struct {
    uint8_t bytes[64];
} __attribute__((aligned(8))) OrbisVideoOutBufferAttribute;
typedef struct {
    uint32_t buttons;  // the rest is sticks, touch and motion, unused here
    uint8_t rest[252];
} __attribute__((aligned(16))) OrbisPadData;
typedef struct {
    int32_t type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    uint8_t useIconImageUri;
    char message[1024];
    char iconUri[1024];
    char unk[1024];
} OrbisNotificationRequest;

int sceKernelSendNotificationRequest(int device, OrbisNotificationRequest* req, size_t size,
                                     int blocking);
int sceKernelUsleep(unsigned int us);
int sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name);
int sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                        uint32_t* timeout);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int type,
                                  off_t* out);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, off_t off,
                             size_t align);
int sceVideoOutOpen(int user, int bus, int index, const void* param);
int sceVideoOutAddFlipEvent(OrbisKernelEqueue eq, int handle, void* udata);
void sceVideoOutSetBufferAttribute(void* attr, unsigned pixelFormat, unsigned tiling,
                                   unsigned aspect, unsigned width, unsigned height,
                                   unsigned pitch);
int sceVideoOutRegisterBuffers(int handle, int start, void* const* addrs, int num,
                               const OrbisVideoOutBufferAttribute* attr);
int sceVideoOutSetFlipRate(int handle, int rate);
int sceVideoOutSubmitFlip(int handle, int buffer, unsigned mode, int64_t arg);
int sceVideoOutGetFlipStatus(int handle, OrbisVideoOutFlipStatus* status);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int* user);
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void* param);
int scePadReadState(int handle, OrbisPadData* data);

#define ORBIS_VIDEO_USER_MAIN 0xFF
#define ORBIS_VIDEO_OUT_BUS_MAIN 0
#define ORBIS_VIDEO_OUT_FLIP_VSYNC 1
#define ORBIS_PAD_BUTTON_OPTIONS 0x0008
#define ORBIS_PAD_BUTTON_RIGHT 0x0020
#define ORBIS_PAD_BUTTON_LEFT 0x0080
#define ORBIS_PAD_BUTTON_L1 0x0400
#define ORBIS_PAD_BUTTON_R1 0x0800
#define ORBIS_PAD_BUTTON_TRIANGLE 0x1000
#define ORBIS_PAD_BUTTON_CROSS 0x4000
#define ORBIS_PAD_BUTTON_SQUARE 0x8000

// ---- the module, built into the app ----

__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global g_prxStart\n"
        "g_prxStart:\n"
        ".incbin \"" SC_PRX_PATH "\"\n"
        ".global g_prxEnd\n"
        "g_prxEnd:\n"
        ".text\n");
extern const uint8_t g_prxStart[];
extern const uint8_t g_prxEnd[];

// ---- system calls: GoldHEN's (500) and the clock the module uses ----

// The fourth argument (r10) is 0, as sys_dynlib_load_prx takes four:
// 0.2 and 0.3 left it as it was, so their loads into the app were wrong.
__attribute__((noinline)) static long sysCall(long num, long a, long b, long c) {
    long rax;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile("syscall; jnc 1f; negq %%rax; 1:"
                     : "=a"(rax), "+D"(a), "+S"(b), "+d"(c), "+r"(r10), "+r"(r8), "+r"(r9)
                     : "a"(num)
                     : "rcx", "r11", "cc", "memory");
    return rax;
}

int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags,
                             const void* opt, int* res);

// The monotonic clock, in ms, as the module's heartbeat has it.
static uint64_t nowMs(void) {
    struct {
        int64_t sec, nsec;
    } t = {0, 0};
    sysCall(232, 4 /* CLOCK_MONOTONIC */, (long)&t, 0);
    return (uint64_t)t.sec * 1000u + (uint64_t)t.nsec / 1000000u;
}

struct JailbreakBackup {
    uint32_t uid, ruid, rgid, groups;
    uint64_t paid;
    uint64_t caps[2];
    void* prison;
    void *cdir, *jdir, *rdir;
};  // the SDK's jailbreak_backup

struct ProcPrxLoad {
    char processName[32];
    char prxPath[100];
    uint64_t res;
} __attribute__((packed));  // the SDK's proc_prx_load

// ---- the log, and the message shown at the bottom ----

static FILE* g_log;
static char g_message[160] = "";

static void say(const char* f, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    vsnprintf(g_message, sizeof(g_message), f, ap);
    va_end(ap);
    if (g_log) {
        fprintf(g_log, "%llu %s\n", (unsigned long long)nowMs(), g_message);
        fflush(g_log);
    }
}

static void notify(const char* text) {
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.targetId = -1;
    strncpy(req.message, text, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// ---- files ----

static int writeFile(const char* path, const void* data, size_t size) {
    // Beside, then renamed over, so the module never reads half of it.
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    const size_t wrote = fwrite(data, 1, size, f);
    fclose(f);
    if (wrote != size) return -1;
    if (rename(tmp, path) != 0) return -1;
    // Read, write and execute for all, as ps4-hen-rest creates the module it
    // has SceShellUI load (sceKernelOpen(path, 0x601, 0777)): fopen made
    // them 0666, and the shell answered EACCES wherever it could see them.
    chmod(path, 0777);
    return 0;
}

static int readFile(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    const size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return (int)n;
}

static void sendCommand(const char* cmd) {
    if (writeFile(SC_BGM_CMD, cmd, strlen(cmd)) == 0) say("Sent: %s", cmd);
    else say("Could not write %s", SC_BGM_CMD);
}

// A WAV of a few notes, 30 s, so that there is something to play with
// nothing copied over.
static int writeTestTone(void) {
    const int rate = 48000, seconds = 30;
    const uint32_t frames = (uint32_t)(rate * seconds), bytes = frames * 4;
    uint8_t* wav = (uint8_t*)malloc(44 + bytes);
    if (!wav) return -1;
    uint32_t* u32;
    memcpy(wav, "RIFF", 4);
    u32 = (uint32_t*)(wav + 4);
    *u32 = 36 + bytes;
    memcpy(wav + 8, "WAVEfmt ", 8);
    const uint8_t fmt[16] = {16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0};
    memcpy(wav + 16, fmt, 16);  // PCM, stereo, 48000 Hz, 192000 B/s
    wav[32] = 4;
    wav[33] = 0;
    wav[34] = 16;
    wav[35] = 0;
    memcpy(wav + 36, "data", 4);
    u32 = (uint32_t*)(wav + 40);
    *u32 = bytes;
    // C E G C, half a second each, a triangle wave.
    const uint32_t notes[4] = {262, 330, 392, 523};
    int16_t* pcm = (int16_t*)(wav + 44);
    uint32_t phase = 0;
    for (uint32_t i = 0; i < frames; i++) {
        const uint32_t at = i % (uint32_t)(rate / 2);
        const uint32_t hz = notes[(i / (uint32_t)(rate / 2)) % 4];
        phase += hz * 65536u / (uint32_t)rate;
        const uint16_t p = (uint16_t)phase;
        int v = p < 32768 ? p : 65535 - p;
        v = (v - 16384) / 2;
        if (at > (uint32_t)(rate * 4 / 10)) v = 0;  // a gap between notes
        pcm[2 * i] = pcm[2 * i + 1] = (int16_t)v;
    }
    const int rc = writeFile(SC_BGM_MUSIC "/test-tone.wav", wav, 44 + bytes);
    free(wav);
    return rc;
}

// How many tracks music/ has.
// yoursong.mp3 beside the package on a USB drive (its root), or in
// /data/ShellCompanion/ps4/: copied into music/ and queued first, ahead of
// the test tone. 1 if one was found.
#define YOUR_SONG "yoursong.mp3"
static int copyFile(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return -1;
    char tmp[200];
    snprintf(tmp, sizeof(tmp), "%s.new", to);
    FILE* out = fopen(tmp, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    static char buf[256 * 1024];
    size_t n, total = 0;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
        total += n;
    }
    fclose(in);
    fclose(out);
    if (!ok || !total || rename(tmp, to) != 0) return -1;
    chmod(to, 0666);
    return (int)(total / 1024);
}

static int useYourSong(void) {
    const char* dest = SC_BGM_MUSIC "/" YOUR_SONG;
    char from[96] = "";
    for (int i = 0; i < 8 && !from[0]; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/mnt/usb%d/" YOUR_SONG, i);
        if (access(path, R_OK) == 0) snprintf(from, sizeof(from), "%s", path);
    }
    if (!from[0] && access(SC_BGM_DIR "/" YOUR_SONG, R_OK) == 0)
        snprintf(from, sizeof(from), "%s", SC_BGM_DIR "/" YOUR_SONG);
    if (!from[0]) {
        say("No " YOUR_SONG " on a USB drive or in " SC_BGM_DIR);
        return 0;
    }
    const int kb = copyFile(from, dest);
    if (kb < 0) {
        say("Could not copy %s into music/", from);
        return 0;
    }
    // The queue: it first, then the test tone if it is there.
    char queue[512];
    snprintf(queue, sizeof(queue), "%s\n%s\n", dest, SC_BGM_MUSIC "/test-tone.wav");
    writeFile(SC_BGM_QUEUE, queue, strlen(queue));
    chmod(SC_BGM_QUEUE, 0666);
    say("%s: %d KB copied into music/, queued first", from, kb);
    return 1;
}

static int countMusic(void) {
    DIR* d = opendir(SC_BGM_MUSIC);
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        const size_t len = strlen(e->d_name);
        if (len > 4 && (!strcasecmp(e->d_name + len - 4, ".mp3") ||
                        !strcasecmp(e->d_name + len - 4, ".wav")))
            n++;
    }
    closedir(d);
    return n;
}

// ---- the player's status ----

static struct {
    int running;
    char state[16];
    int index, count;
    unsigned posMs;
    char file[256];
} g_status;

static const char* field(const char* text, const char* key, char* out, size_t cap) {
    const size_t klen = strlen(key);
    for (const char* p = text; *p;) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            while (p[n] && p[n] != '\n' && n + 1 < cap) {
                out[n] = p[n];
                n++;
            }
            out[n] = 0;
            return out;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    out[0] = 0;
    return NULL;
}

static void readStatus(void) {
    char text[1024], v[256];
    memset(&g_status, 0, sizeof(g_status));
    if (readFile(SC_BGM_STATUS, text, sizeof(text)) <= 0) return;
    const uint64_t beat = field(text, "beat_ms", v, sizeof(v)) ? strtoull(v, NULL, 10) : 0;
    const uint64_t now = nowMs();
    g_status.running = beat && now >= beat && now - beat < 4000;
    field(text, "state", g_status.state, sizeof(g_status.state));
    g_status.index = field(text, "index", v, sizeof(v)) ? atoi(v) : 0;
    g_status.count = field(text, "count", v, sizeof(v)) ? atoi(v) : 0;
    g_status.posMs = field(text, "pos_ms", v, sizeof(v)) ? (unsigned)strtoul(v, NULL, 10) : 0;
    field(text, "file", g_status.file, sizeof(g_status.file));
}

// ---- loading the module into the shell ----

// GoldHEN's PROCESS_PRX_LOAD into SceShellUI, the path as the shell sees
// it: 1 if it loaded.
static int processLoad(const char* process, const char* path) {
    static struct ProcPrxLoad args;
    memset(&args, 0, sizeof(args));
    strncpy(args.processName, process, sizeof(args.processName) - 1);
    strncpy(args.prxPath, path, sizeof(args.prxPath) - 1);
    args.res = ~0ULL;  // as ps4-hen-rest sets it
    const long rc = sysCall(500, 6 /* PROCESS_PRX_LOAD */, (long)&args, 0);
    const int ok = rc == 0 && (int32_t)args.res >= 0;
    say("%s load %s: 0x%lX, result 0x%lX%s", process, path, (unsigned long)rc,
        (unsigned long)args.res, ok ? " - loaded" : "");
    return ok;
}

static int shellLoad(const char* path) {
    return processLoad("SceShellUI", path);
}

static int g_loadedFrom = -1;

// Loads path into this app, not started (sys_dynlib_load_prx), and
// unloads it: logged.
static int selfLoad(const char* path) {
    int handle = 0;
    const long rc = sysCall(594, (long)path, 0, (long)&handle);
    say("Load into this app (not started) %s: 0x%lX, handle 0x%X", path, (unsigned long)rc,
        handle);
    if (rc == 0 && handle > 0) sysCall(595 /* sys_dynlib_unload_prx */, handle, 0, 0);
    return rc == 0;
}

#define PKG_DIR "/mnt/sandbox/pfsmnt/SCMP00001-app0"
#define SHELL_SANDBOX "/mnt/sandbox/NPXS20001_000"

// The module in the package (ps4/bgm/build.sh), and whether it loads into
// this app: 0, or -1.
static const char* const g_builds[1] = {"sc_bgm.prx"};
static int g_build = -1;

static void findBuild(void) {
    if (g_build >= 0) return;
    for (int i = 0; i < 1; i++) {
        char path[160];
        snprintf(path, sizeof(path), PKG_DIR "/%s", g_builds[i]);
        if (selfLoad(path)) {
            g_build = i;
            say("Build %s loads", g_builds[i]);
            return;
        }
    }
    say("No build of the module loads into this app");
}

// What the shell's sandbox holds, two levels down: where it might load from.
static void listShellSandbox(void) {
    static int done;
    if (done || !g_log) return;
    done = 1;
    DIR* d = opendir(SHELL_SANDBOX);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        fprintf(g_log, "  shell sandbox: /%s\n", e->d_name);
        char sub[200];
        snprintf(sub, sizeof(sub), SHELL_SANDBOX "/%s", e->d_name);
        DIR* d2 = opendir(sub);
        if (!d2) continue;
        struct dirent* e2;
        int n = 0;
        while ((e2 = readdir(d2)) && n < 40) {
            if (e2->d_name[0] == '.') continue;
            fprintf(g_log, "    /%s/%s\n", e->d_name, e2->d_name);
            n++;
        }
        closedir(d2);
    }
    closedir(d);
    fflush(g_log);
}

// Copies a file of the package to dest: 0, or -1.
static int copyFromPackage(const char* name, const char* dest) {
    char src[160];
    snprintf(src, sizeof(src), PKG_DIR "/%s", name);
    FILE* f = fopen(src, "rb");
    if (!f) return -1;
    static uint8_t buf[512 * 1024];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n ? writeFile(dest, buf, n) : -1;
}

// The places to ask the shell for a module from, as the projects that do
// it keep theirs: Zenalios/ps4-thumbgrid-ime asks for /user/data/... and
// /data/...; j117mw3/ps4-hen-rest copies its module into the shell's
// sandbox under Temp/. name is copied to each first. 1 if one loaded.
static int intoShell(const char* name) {
    char data[160], temp[200], inShell[160];
    snprintf(data, sizeof(data), SC_BGM_DIR "/%s", name);
    if (copyFromPackage(name, data) == 0) {
        char user[180];
        snprintf(user, sizeof(user), "/user%s", data);
        if (shellLoad(user) || shellLoad(data)) return 1;
    }
    mkdir(SHELL_SANDBOX "/Temp/ShellCompanion", 0777);
    snprintf(temp, sizeof(temp), SHELL_SANDBOX "/Temp/ShellCompanion/%s", name);
    snprintf(inShell, sizeof(inShell), "/Temp/ShellCompanion/%s", name);
    if (copyFromPackage(name, temp) == 0 && (shellLoad(inShell) || shellLoad(temp))) return 1;
    char pkg[160];
    snprintf(pkg, sizeof(pkg), PKG_DIR "/%s", name);
    return shellLoad(pkg);
}

// Starts the player inside this app, the way a module is meant to be
// started (sceKernelLoadStartModule), when the shell will not have it: it
// plays while the app is open, which shows whether the module and its
// audio work at all. 1 if it started.
static int g_inApp;
static int startInApp(void) {
    if (g_inApp) return 1;
    if (g_build < 0) g_build = 0;  // its only build: tried anyway
    char paths[2][160];
    snprintf(paths[0], sizeof(paths[0]), PKG_DIR "/%s", g_builds[g_build]);
    snprintf(paths[1], sizeof(paths[1]), "/app0/%s", g_builds[g_build]);
    for (int i = 0; i < 2; i++) {
        int res = 0;
        const int h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
        say("Start in this app %s: 0x%08X, module_start 0x%X", paths[i], (unsigned)h, res);
        if (h >= 0) {
            g_inApp = 1;
            say("Playing inside this app (not the shell): stops when the app closes");
            return 1;
        }
    }
    return 0;
}

// The first runs on a PS4 (11.50, GoldHEN) had the shell answer ENOENT
// (0x80020002) for /data/... and EACCES (0x8002000D) for /user/data/...,
// /system_tmp and its own sandbox, and this app EACCES (-13) loading it
// from /data itself. So: a system module first, which the shell has, to
// see that the call can work at all; then the copy in this app's package,
// a place modules are allowed to come from; then the rest.
static void loadPlayer(void) {
    readStatus();
    if (g_status.running) {
        sendCommand("reload\nplay\n");
        return;
    }
    say("-- loading, app " APP_VERSION " --");
    listShellSandbox();
    // First, once, the test module made as ps4-hen-rest makes its shell
    // module: a notification says it runs in the shell.
    static int helloTried;
    if (!helloTried) {
        helloTried = 1;
        selfLoad(PKG_DIR "/hello.prx");
        selfLoad(PKG_DIR "/no_share_watermark.prx");  // GoldHEN's own: the control
        // The documented route first: ps4-hen-rest's starter asks for its
        // module in SceShellCore from /data/GoldHEN/plugins/, and
        // ps4-thumbgrid-ime's list has /data/GoldHEN/plugins/ and
        // /user/data/GoldHEN/plugins/ for SceShellUI.
        mkdir("/data/GoldHEN", 0777);
        mkdir("/data/GoldHEN/plugins", 0777);
        const int put = copyFromPackage("hello.prx", "/data/GoldHEN/plugins/sc_hello.prx");
        say("hello.prx into /data/GoldHEN/plugins: %d", put);
        processLoad("SceShellCore", "/data/GoldHEN/plugins/sc_hello.prx");
        if (!shellLoad("/data/GoldHEN/plugins/sc_hello.prx"))
            shellLoad("/user/data/GoldHEN/plugins/sc_hello.prx");
        say("Test module into the shell: %s", intoShell("hello.prx") ? "loaded" : "refused");
    }
    // Then the player. Loading it into this app is logged, but does not
    // decide: the projects above never load theirs into an app.
    findBuild();
    if (copyFromPackage("sc_bgm.prx", "/data/GoldHEN/plugins/sc_bgm.prx") == 0 &&
        (shellLoad("/data/GoldHEN/plugins/sc_bgm.prx") ||
         shellLoad("/user/data/GoldHEN/plugins/sc_bgm.prx"))) {
        g_loadedFrom = 5;
        return;
    }
    if (intoShell("sc_bgm.prx")) {
        g_loadedFrom = 1;
        return;
    }
    say("The shell would not load it: send app.log");
    startInApp();
}

// ---- the screen: two frame buffers, drawn by the CPU ----

#define W 1920
#define H 1080
#define SCALE 3
#define CH (8 * SCALE)
#define LINE (CH + 10)

static int g_video;
static uint32_t* g_fb[2];
static int g_active;
static OrbisKernelEqueue g_flipQueue;

static int videoInit(void) {
    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g_video < 0) return 0;
    if (sceKernelCreateEqueue(&g_flipQueue, "bgm flip") < 0) return 0;
    sceVideoOutAddFlipEvent(g_flipQueue, g_video, 0);
    const size_t fbSize = (size_t)W * H * 4;
    const size_t align = 0x200000, total = (fbSize * 2 + align - 1) / align * align;
    off_t off = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3, &off) < 0)
        return 0;
    void* mem = NULL;
    if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, off, align) < 0) return 0;
    g_fb[0] = (uint32_t*)mem;
    g_fb[1] = (uint32_t*)((char*)mem + fbSize);
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, W, H, W);
    if (sceVideoOutRegisterBuffers(g_video, 0, (void**)g_fb, 2, &attr) != 0) return 0;
    sceVideoOutSetFlipRate(g_video, 0);
    return 1;
}

static void flip(int64_t frame) {
    sceVideoOutSubmitFlip(g_video, g_active, ORBIS_VIDEO_OUT_FLIP_VSYNC, frame);
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g_video, &st);
        if (st.flipArg == frame) break;
        OrbisKernelEvent ev;
        int count;
        if (sceKernelWaitEqueue(g_flipQueue, &ev, 1, &count, 0) != 0) break;
    }
    g_active ^= 1;
}

static void fill(int x, int y, int w, int h, uint32_t c) {
    uint32_t* fb = g_fb[g_active];
    for (int j = y; j < y + h && j < H; j++)
        for (int i = x; i < x + w && i < W; i++) fb[j * W + i] = c;
}

static void text(int x, int y, uint32_t c, const char* s) {
    uint32_t* fb = g_fb[g_active];
    for (; *s && x + CH <= W; s++, x += CH) {
        const unsigned char ch = (unsigned char)*s < 128 ? (unsigned char)*s : '?';
        for (int row = 0; row < 8; row++) {
            const unsigned char bits = font8x8_basic[ch][row];
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1 << col))) continue;
                for (int dy = 0; dy < SCALE; dy++)
                    for (int dx = 0; dx < SCALE; dx++)
                        fb[(y + row * SCALE + dy) * W + x + col * SCALE + dx] = c;
            }
        }
    }
}

static void textf(int x, int y, uint32_t c, const char* f, ...) __attribute__((format(printf, 4, 5)));
static void textf(int x, int y, uint32_t c, const char* f, ...) {
    char line[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(line, sizeof(line), f, ap);
    va_end(ap);
    text(x, y, c, line);
}

#define BG 0x80181C30
#define FG 0x80E8E8F0
#define DIM 0x808890A8
#define ACCENT 0x8078C8FF
#define GOOD 0x8070E090
#define BAD 0x80FF7070

// The last lines of the module's log.
static char g_logTail[12][120];
static void readLogTail(void) {
    static char text[8192];
    memset(g_logTail, 0, sizeof(g_logTail));
    FILE* f = fopen(SC_BGM_DIR "/bgm.log", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, size > (long)sizeof(text) - 1 ? size - (long)sizeof(text) + 1 : 0, SEEK_SET);
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    // Back from the end, a line at a time.
    int lines = 0;
    int end = (int)n;
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) end--;
    while (end > 0 && lines < 12) {
        int start = end;
        while (start > 0 && text[start - 1] != '\n') start--;
        int len = end - start;
        if (len > 119) len = 119;
        memcpy(g_logTail[11 - lines], text + start, (size_t)len);
        g_logTail[11 - lines][len] = 0;
        lines++;
        end = start - 1;
    }
}

static int g_portType;

static void draw(void) {
    fill(0, 0, W, H, BG);
    int y = 60;
    const int x = 90;
    text(x, y, ACCENT, "Shell Companion - PS4 background music test " APP_VERSION);
    y += LINE * 2;
    if (g_status.running) {
        textf(x, y, GOOD, "Player: running %s (%s)", g_inApp ? "INSIDE THIS APP" : "in the shell",
              g_status.state);
        y += LINE;
        textf(x, y, FG, "Track %d of %d   %u:%02u", g_status.count ? g_status.index + 1 : 0,
              g_status.count, g_status.posMs / 60000, g_status.posMs / 1000 % 60);
        y += LINE;
        const char* base = strrchr(g_status.file, '/');
        textf(x, y, FG, "%s", base ? base + 1 : g_status.file);
    } else {
        text(x, y, BAD, "Player: not running. Press X to load it.");
        y += LINE * 2;
    }
    y += LINE;
    textf(x, y, DIM, "Audio port type: %d   Tracks in music/: from %s", g_portType, SC_BGM_MUSIC);
    y += LINE * 2;

    text(x, y, FG, "X      load the player / play");
    y += LINE;
    text(x, y, FG, "SQUARE pause / resume      TRIANGLE stop");
    y += LINE;
    text(x, y, FG, "R1     next track          L1 previous track");
    y += LINE;
    text(x, y, FG, "LEFT / RIGHT  audio port type, then restart the queue");
    y += LINE;
    text(x, y, FG, "OPTIONS  write a test tone into music/ again");
    y += LINE;
    text(x, y, DIM, "Leave with the PS button: the music should keep playing.");
    y += LINE * 2;

    textf(x, y, ACCENT, "%s", g_message);
    y += LINE * 2;
    text(x, y, DIM, "bgm.log:");
    y += LINE;
    for (int i = 0; i < 12 && y < H - LINE; i++, y += LINE - 6) text(x, y, DIM, g_logTail[i]);
}

// ---- the app ----

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Before the jailbreak: the shell asked for the test module 0.10 left in
    // the GoldHEN plugins folder, as neither project that loads modules into
    // the shell jailbreaks first. Logged once the log is open.
    static struct ProcPrxLoad pre[3];
    static const char* const preProc[3] = {"SceShellCore", "SceShellUI", "SceShellUI"};
    static const char* const prePath[3] = {"/data/GoldHEN/plugins/sc_hello.prx",
                                           "/user/data/GoldHEN/plugins/sc_hello.prx",
                                           "/data/GoldHEN/plugins/sc_hello.prx"};
    long preRc[3];
    for (int i = 0; i < 3; i++) {
        memset(&pre[i], 0, sizeof(pre[i]));
        strncpy(pre[i].processName, preProc[i], sizeof(pre[i].processName) - 1);
        strncpy(pre[i].prxPath, prePath[i], sizeof(pre[i].prxPath) - 1);
        pre[i].res = ~0ULL;
        preRc[i] = sysCall(500, 6 /* PROCESS_PRX_LOAD */, (long)&pre[i], 0);
    }
    // And GoldHEN's own plugin into this app, not started: the control.
    int ctlHandle = 0;
    const long ctlRc = sysCall(594, (long)"/app0/no_share_watermark.prx", 0, (long)&ctlHandle);
    if (ctlRc == 0 && ctlHandle > 0) sysCall(595, ctlHandle, 0, 0);
    int ownHandle = 0;
    const long ownRc = sysCall(594, (long)"/app0/hello.prx", 0, (long)&ownHandle);
    if (ownRc == 0 && ownHandle > 0) sysCall(595, ownHandle, 0, 0);

    // Out of the sandbox, to reach /data. GoldHEN gives this to homebrew.
    static struct JailbreakBackup jb;
    const long jbrc = sysCall(500, 2 /* JAILBREAK */, (long)&jb, 0);

    mkdir("/data/ShellCompanion", 0777);
    mkdir(SC_BGM_DIR, 0777);
    mkdir(SC_BGM_MUSIC, 0777);
    g_log = fopen(SC_BGM_DIR "/app.log", "w");
    say("App %s; jailbreak 0x%lX; GoldHEN SDK 0x%lX", APP_VERSION, (unsigned long)jbrc,
        (unsigned long)sysCall(500, 0 /* VERSION */, 0, 0));
    for (int i = 0; i < 3; i++)
        say("Before the jailbreak, %s load %s: 0x%lX, result 0x%lX%s", preProc[i], prePath[i],
            (unsigned long)preRc[i], (unsigned long)pre[i].res,
            preRc[i] == 0 && (int32_t)pre[i].res >= 0 ? " - loaded" : "");
    say("Before the jailbreak, into this app (not started): GoldHEN's no_share_watermark.prx "
        "0x%lX handle 0x%X; our hello.prx 0x%lX handle 0x%X",
        (unsigned long)ctlRc, ctlHandle, (unsigned long)ownRc, ownHandle);

    char portText[16];
    if (readFile(SC_BGM_PORT, portText, sizeof(portText)) > 0) g_portType = atoi(portText);

    const int yourSong = useYourSong();
    if (!yourSong && !countMusic() && access(SC_BGM_QUEUE, F_OK) != 0) {
        const int rc = writeTestTone();
        say("No tracks yet: test-tone.wav written (%d)", rc);
    }

    if (!videoInit()) {
        notify("BGM test: the screen would not start");
        say("video init failed");
        for (;;) sceKernelUsleep(1000000);
    }

    const int32_t userParams[1] = {700};  // its thread priority, the default
    sceUserServiceInitialize(userParams);
    int user = 0;
    sceUserServiceGetInitialUser(&user);
    scePadInit();
    const int pad = scePadOpen(user, 0, 0, NULL);
    if (pad < 0) say("No controller (0x%08X)", (unsigned)pad);

    // Load it at once: the main thing the app is for.
    loadPlayer();

    unsigned prev = 0;
    for (int64_t frame = 1;; frame++) {
        if ((frame % 30) == 1) {
            readStatus();
            readLogTail();
        }
        OrbisPadData data;
        memset(&data, 0, sizeof(data));
        if (pad >= 0) scePadReadState(pad, &data);
        const unsigned down = data.buttons & ~prev;
        prev = data.buttons;

        if (down & ORBIS_PAD_BUTTON_CROSS) {
            if (!g_status.running) loadPlayer();
            else sendCommand("play\n");
        }
        if (down & ORBIS_PAD_BUTTON_SQUARE) sendCommand("toggle\n");
        if (down & ORBIS_PAD_BUTTON_TRIANGLE) sendCommand("stop\n");
        if (down & ORBIS_PAD_BUTTON_R1) sendCommand("next\n");
        if (down & ORBIS_PAD_BUTTON_L1) sendCommand("prev\n");
        if (down & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
            // MAIN 0, BGM 1, VOICE 2: heard on a PS4 (11.50) from the player
            // in this app; 3 (PERSONAL), 14 and 127 (AUX) were not.
            static const int types[] = {0, 1, 2};
            const int nTypes = (int)(sizeof(types) / sizeof(types[0]));
            int at = 0;
            for (int i = 0; i < nTypes; i++)
                if (types[i] == g_portType) at = i;
            at = (at + ((down & ORBIS_PAD_BUTTON_RIGHT) ? 1 : nTypes - 1)) % nTypes;
            g_portType = types[at];
            char t[16];
            snprintf(t, sizeof(t), "%d\n", g_portType);
            writeFile(SC_BGM_PORT, t, strlen(t));
            sendCommand("reload\nplay\n");
            say("Port type %d: restarting the queue", g_portType);
        }
        if (down & ORBIS_PAD_BUTTON_OPTIONS) {
            say("test-tone.wav written (%d)", writeTestTone());
            sendCommand("reload\nplay\n");
        }

        draw();
        flip(frame);
    }
    return 0;
}
