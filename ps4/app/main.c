/**
 * Shell Companion BGM Test: a PS4 app (a .pkg) to try the background music
 * module without GoldHEN's BinLoader. It carries sc_bgm.prx, writes it to
 * /data/ShellCompanion/ps4, has GoldHEN load it into SceShellUI, and drives
 * it with the controller, through the files the module reads (bgm.h). It
 * shows the player's status and the end of its log.
 *
 * Its own log is /user/data/ShellCompanion/ps4/app.log.
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
#include "../client/shell_companion.h"
#include "font8x8_basic.h"

#define APP_VERSION "1.34"

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
#define ORBIS_PAD_BUTTON_L3 0x0002
#define ORBIS_PAD_BUTTON_TOUCH_PAD 0x00100000
#define ORBIS_PAD_BUTTON_R3 0x0004
#define ORBIS_PAD_BUTTON_UP 0x0010
#define ORBIS_PAD_BUTTON_DOWN 0x0040
#define ORBIS_PAD_BUTTON_RIGHT 0x0020
#define ORBIS_PAD_BUTTON_LEFT 0x0080
#define ORBIS_PAD_BUTTON_L2 0x0100
#define ORBIS_PAD_BUTTON_R2 0x0200
#define ORBIS_PAD_BUTTON_L1 0x0400
#define ORBIS_PAD_BUTTON_R1 0x0800
#define ORBIS_PAD_BUTTON_TRIANGLE 0x1000
#define ORBIS_PAD_BUTTON_CROSS 0x4000
#define ORBIS_PAD_BUTTON_SQUARE 0x8000

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

int sceKernelDlsym(int handle, const char* symbol, void** addr);
int sceSysmoduleLoadModule(uint16_t id);
int sceSystemServiceReenableMusicPlayer(void);
int sceSystemServiceIsBgmPlaying(void* playing);
int scePthreadCreate(void* thread, const void* attr, void* (*fn)(void*), void* arg,
                     const char* name);
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
    say("%s: %d KB copied into music/", from, kb);
    return 1;
}

// The queue, queue.txt: yoursong.mp3 first, then every other track in
// music/ by name, the test tone, then streams.txt's lines (0.72: URLs, each
// URL<tab>title<tab>artist<tab>album<tab>cover URL, the last four optional).
#define STREAMS SC_BGM_DIR "/streams.txt"
static void writeQueue(void) {
    static char queue[256 * 1024];
    static char names[256][128];
    int n = 0;
    DIR* d = opendir(SC_BGM_MUSIC);
    struct dirent* e;
    while (d && n < 256 && (e = readdir(d))) {
        const size_t len = strlen(e->d_name);
        if (len < 5 || len >= sizeof(names[0]) || e->d_name[0] == '.') continue;
        if (strcasecmp(e->d_name + len - 4, ".mp3") && strcasecmp(e->d_name + len - 4, ".wav")) continue;
        if (!strcasecmp(e->d_name, YOUR_SONG) || !strcasecmp(e->d_name, "test-tone.wav")) continue;
        snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    if (d) closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), (int (*)(const void*, const void*))strcasecmp);
    size_t at = 0;
    if (access(SC_BGM_MUSIC "/" YOUR_SONG, R_OK) == 0)
        at += (size_t)snprintf(queue + at, sizeof(queue) - at, SC_BGM_MUSIC "/" YOUR_SONG "\n");
    for (int i = 0; i < n && at + 200 < sizeof(queue); i++)
        at += (size_t)snprintf(queue + at, sizeof(queue) - at, SC_BGM_MUSIC "/%s\n", names[i]);
    if (access(SC_BGM_MUSIC "/test-tone.wav", R_OK) == 0)
        at += (size_t)snprintf(queue + at, sizeof(queue) - at, SC_BGM_MUSIC "/test-tone.wav\n");
    int streams = 0, lineNo = 0;
    // streams.txt on a USB drive's root, or in ShellCompanion/, is copied
    // here first, the player reading it here too (0.74).
    char from[96] = "";
    for (int i = 0; i < 8 && !from[0]; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/mnt/usb%d/streams.txt", i);
        if (access(path, R_OK) == 0) snprintf(from, sizeof(from), "%s", path);
    }
    if (!from[0] && access("/user/data/ShellCompanion/streams.txt", R_OK) == 0)
        snprintf(from, sizeof(from), "/user/data/ShellCompanion/streams.txt");
    if (from[0]) {
        const int kb = copyFile(from, STREAMS);
        say("streams.txt: copied from %s (%d KB)", from, kb);
    }
    FILE* f = fopen(STREAMS, "r");
    if (!f) say("streams.txt: not there (%s, nor a USB drive's root, nor /user/data/ShellCompanion/)", STREAMS);
    static char line[4096];
    while (f && fgets(line, sizeof(line), f)) {
        lineNo++;
        char* p = line;
        // A UTF-8 byte-order mark (Notepad's), and spaces, before it (0.73).
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        while (*p == ' ' || *p == '\t') p++;
        size_t len = strlen(p);
        while (len && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ')) p[--len] = 0;
        if (!len) continue;
        if (strncasecmp(p, "http://", 7) && strncasecmp(p, "https://", 8)) {
            say("streams.txt line %d: skipped, not a URL: \"%.40s\"", lineNo, p);
            continue;
        }
        if (at + len + 2 >= sizeof(queue)) break;
        at += (size_t)snprintf(queue + at, sizeof(queue) - at, "%s\n", p);
        streams++;
    }
    if (f) fclose(f);
    writeFile(SC_BGM_QUEUE, queue, at);
    chmod(SC_BGM_QUEUE, 0666);
    say("queue.txt: %d track(s) from music/, %d stream(s) from streams.txt", n, streams);
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

#ifndef SC_TITLE_ID
#define SC_TITLE_ID "SCMP00001"
#endif

// Shell Companion's module (sc_companion.prx, the player and the music
// watch) into SceShellUI through the client library, unless it is there,
// then the queue played.
static void loadPlayer(void) {
    say("-- loading, app " APP_VERSION " --");
    char what[512];
    const int rc = scOpen(what, sizeof(what));
    say("scOpen: %d: %s", rc, what);
    if (scPlayerRunning()) scMusicPlay();
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

// Whether mcw-fake.txt says off: the watch answers the quick menu for our
// player otherwise.
static int fakeOff(void) {
    char t[8] = {0};
    FILE* f = fopen(SC_BGM_DIR "/mcw-fake.txt", "r");
    if (!f) return 0;
    const size_t n = fread(t, 1, 3, f);
    fclose(f);
    return n == 3 && !memcmp(t, "off", 3);
}

static void draw(void) {
    fill(0, 0, W, H, BG);
    int y = 60;
    const int x = 90;
    text(x, y, ACCENT, "Shell Companion - PS4 background music test " APP_VERSION);
    y += LINE * 2;
    if (g_status.running) {
        textf(x, y, GOOD, "Player: running in the shell (%s)", g_status.state);
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
    textf(x, y, DIM, "Tracks: %s, yoursong.mp3, streams.txt", SC_BGM_MUSIC);
    y += LINE * 2;

    text(x, y, FG, "X      load the player / play");
    y += LINE;
    text(x, y, FG, "SQUARE pause / resume      TRIANGLE stop");
    y += LINE;
    text(x, y, FG, "R1     next track          L1 previous track");
    y += LINE;
    text(x, y, FG, "OPTIONS  write a test tone into music/ again");
    y += LINE;
    text(x, y, FG, "L2     background downloads test: 3 songs to ShellCompanion/ps4/downloads");
    y += LINE;
    text(x, y, FG, "R2     take the downloads back (stops them), their status in app.log");
    y += LINE;
    text(x, y, FG, "L3     self-update from test-update.txt (a .pkg path or URL), then close");

    y += LINE;
    textf(x, y, FG, "R3     quick menu answers for our player (mcw-fake.txt): %s",
          fakeOff() ? "off" : "ON");
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

// What GoldHEN's jailbreak replaced (the SDK's jailbreak_backup), to put
// back: the music service refuses a jailbroken app, it seems.
static struct JailbreakBackup g_jb;

// Background downloads (0.82), through the client library: three test
// songs and three pages, into /data/ShellCompanion/ps4/downloads/. Its
// own downloads.txt (lines of URLs) is used instead if there is one.
static void testDownloads(void) {
    char what[512];
    const int rc = scOpen(what, sizeof(what));
    say("scOpen: %d: %s", rc, what);
    static ScDownload items[16];
    static char urls[16][512], files[16][256], titles[16][64], ids[16][16], icons[16][512];
    int n = 0;
    FILE* f = fopen(SC_BGM_DIR "/test-downloads.txt", "r");
    char line[512];
    while (f && n < 16 && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
        // A URL, or a package on the console (0.99: /data/....pkg).
        if (strncmp(line, "http", 4) && line[0] != '/') continue;
        // An icon after it, a space or tab between (1.29): a PNG on the
        // console or a URL, for its row in Notifications > Downloads.
        icons[n][0] = 0;
        char* gap = strpbrk(line, " \t");
        if (gap) {
            *gap++ = 0;
            while (*gap == ' ' || *gap == '\t') gap++;
            snprintf(icons[n], sizeof(icons[n]), "%s", gap);
        }
        snprintf(urls[n], sizeof(urls[n]), "%s", line);
        const char* name = strrchr(line, '/');
        snprintf(files[n], sizeof(files[n]), "/data/ShellCompanion/ps4/downloads/%s", name ? name + 1 : "file");
        char* q = strchr(files[n], '?');
        if (q) *q = 0;
        snprintf(titles[n], sizeof(titles[n]), "%s", name ? name + 1 : line);
        n++;
    }
    if (f) fclose(f);
    if (!n) {
        for (int i = 0; i < 3; i++) {
            icons[n][0] = 0;
            snprintf(urls[n], sizeof(urls[n]), "https://www.soundhelix.com/examples/mp3/SoundHelix-Song-%d.mp3", i + 1);
            snprintf(files[n], sizeof(files[n]), "/data/ShellCompanion/ps4/downloads/SoundHelix-Song-%d.mp3", i + 1);
            snprintf(titles[n], sizeof(titles[n]), "SoundHelix Song %d", i + 1);
            n++;
        }
    }
    for (int i = 0; i < n; i++) {
        snprintf(ids[i], sizeof(ids[i]), "test%d", i + 1);
        // A .pkg goes to the PS4's own download service (0.89).
        const char* q = strchr(urls[i], '?');
        const size_t ul = q ? (size_t)(q - urls[i]) : strlen(urls[i]);
        const int pkg = ul > 4 && !strncasecmp(urls[i] + ul - 4, ".pkg", 4);
        items[i] = (ScDownload){.id = ids[i], .file = pkg ? NULL : files[i], .title = titles[i],
                                .kind = pkg ? "pkg" : "url", .args = {urls[i], NULL, NULL},
                                .icon = icons[i][0] ? icons[i] : NULL};
    }
    mkdir(SC_BGM_DIR "/downloads", 0777);
    // Fetched again each test (0.97): a file there already is done at once.
    for (int i = 0; i < n; i++)
        if (items[i].file) unlink(items[i].file);
    const int w = scWriteDownloads(1, NULL, items, (uint32_t)n);
    const int s = scDownloadsStart();
    say("Downloads handed over: %d item(s), written %d, started %d: see bgm.log and downloads/", n, w, s);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    // Out of the sandbox, to reach /data. GoldHEN gives this to homebrew.
    const long jbrc = sysCall(500, 2 /* JAILBREAK */, (long)&g_jb, 0);
    scInit(SC_TITLE_ID, "Shell Companion BGM Test");

    mkdir("/data/ShellCompanion", 0777);
    mkdir(SC_BGM_DIR, 0777);
    mkdir(SC_BGM_MUSIC, 0777);
    g_log = fopen(SC_BGM_DIR "/app.log", "w");
    say("App %s; jailbreak 0x%lX; GoldHEN SDK 0x%lX", APP_VERSION, (unsigned long)jbrc,
        (unsigned long)sysCall(500, 0 /* VERSION */, 0, 0));
    {
        // What came of a self-update (0.89), if one ran.
        int result = 0;
        char why[200] = "";
        if (scReadUpdateResult(&result, why, sizeof(why)) == 0) say("Last self-update: %d, %s", result, why);
    }

    // The audio port type the old test app's LEFT/RIGHT chose: gone, only
    // type 0 works in the shell (0.95).
    unlink(SC_BGM_PORT);

    const int yourSong = useYourSong();
    if (!yourSong && !countMusic() && access(STREAMS, R_OK) != 0) {
        const int rc = writeTestTone();
        say("No tracks yet: test-tone.wav written (%d)", rc);
    }
    writeQueue();

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
        // Background downloads (0.82; the music service probes these
        // buttons had are gone): L2 hands over a test list, R2 takes it back.
        if (down & ORBIS_PAD_BUTTON_L2) testDownloads();
        if (down & ORBIS_PAD_BUTTON_R2) {
            const int rc = scDownloadsTakeBack();
            int running = 0;
            ScDownloadStatus st[8];
            uint32_t n = 0;
            scReadDownloadsStatus(&running, st, 8, &n);
            say("Downloads taken back: %d; running %d", rc, running);
            for (uint32_t i = 0; i < n; i++)
                say("  %s: %s, %llu of %llu bytes%s", st[i].id, st[i].state, (unsigned long long)st[i].bytes,
                    (unsigned long long)st[i].size, "");
        }
        if (down & ORBIS_PAD_BUTTON_L3) {
            // Self-update (0.89): test-update.txt's first line, a package's
            // path (/data/...) or URL, installed once this app has gone.
            char line[600] = "";
            FILE* uf = fopen(SC_BGM_DIR "/test-update.txt", "r");
            if (uf) {
                if (fgets(line, sizeof(line), uf)) line[strcspn(line, "\r\n")] = 0;
                fclose(uf);
            }
            if (!line[0]) {
                say("Self-update: put a package's path or URL in test-update.txt first");
            } else {
                char what[512];
                scOpen(what, sizeof(what));
                const int rc = scUpdateInstallPkg(line, 1);
                say("Self-update from %s: %d; closing so it can install", line, rc);
                if (rc == 0) {
                    if (g_log) fflush(g_log);
                    exit(0);
                }
            }
        }
        if (down & ORBIS_PAD_BUTTON_R3) {
            // The watch answers the quick menu for our player while this
            // file is there (ps4/mcw, fakeApply): as the test app.
            // On unless the file says off (from 0.36).
            if (!fakeOff()) {
                writeFile(SC_BGM_DIR "/mcw-fake.txt", "off\n", 4);
                say("Quick menu answers: off");
            } else {
                writeFile(SC_BGM_DIR "/mcw-fake.txt", "CUSA01780\n", 10);
                say("Quick menu answers: on, as Spotify (CUSA01780): the Music section too");
            }
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
