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
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "../bgm/bgm.h"
#include "font8x8_basic.h"

#define APP_VERSION "0.1"

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

__attribute__((noinline)) static long sysCall(long num, long a, long b, long c) {
    long rax;
    __asm__ volatile("syscall; jnc 1f; negq %%rax; 1:"
                     : "=a"(rax), "+D"(a), "+S"(b), "+d"(c)
                     : "a"(num)
                     : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return rax;
}

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
    return rename(tmp, path);
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

static void loadPlayer(void) {
    readStatus();
    if (g_status.running) {
        sendCommand("reload\nplay\n");
        return;
    }
    const size_t size = (size_t)(g_prxEnd - g_prxStart);
    if (writeFile(SC_BGM_PRX, g_prxStart, size) != 0) {
        say("Could not write %s (is the app jailbroken?)", SC_BGM_PRX);
        return;
    }
    static struct ProcPrxLoad args;
    memset(&args, 0, sizeof(args));
    strcpy(args.processName, "SceShellUI");
    strcpy(args.prxPath, SC_BGM_PRX);
    const long rc = sysCall(500, 6 /* PROCESS_PRX_LOAD */, (long)&args, 0);
    say("Loaded into the shell: 0x%lX, result 0x%lX", (unsigned long)rc,
        (unsigned long)args.res);
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

    // Out of the sandbox, to reach /data. GoldHEN gives this to homebrew.
    static struct JailbreakBackup jb;
    const long jbrc = sysCall(500, 2 /* JAILBREAK */, (long)&jb, 0);

    mkdir("/data/ShellCompanion", 0777);
    mkdir(SC_BGM_DIR, 0777);
    mkdir(SC_BGM_MUSIC, 0777);
    g_log = fopen(SC_BGM_DIR "/app.log", "w");
    say("App %s; jailbreak 0x%lX; GoldHEN SDK 0x%lX", APP_VERSION, (unsigned long)jbrc,
        (unsigned long)sysCall(500, 0 /* VERSION */, 0, 0));

    char portText[16];
    if (readFile(SC_BGM_PORT, portText, sizeof(portText)) > 0) g_portType = atoi(portText);

    if (!countMusic() && access(SC_BGM_QUEUE, F_OK) != 0) {
        const int rc = writeTestTone();
        say("No tracks yet: test-tone.wav written (%d)", rc);
    }

    if (!videoInit()) {
        notify("BGM test: the screen would not start");
        say("video init failed");
        for (;;) sceKernelUsleep(1000000);
    }

    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
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
            // MAIN 0, BGM 1, VOICE 2, PERSONAL 3, PADSPK 4, AUX 127, 14 ...
            static const int types[] = {0, 1, 2, 3, 14, 127};
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
