/**
 * Background music for the PS4: a module GoldHEN loads into SceShellUI
 * (load.c asks it to), which plays a queue of MP3 and WAV files on the
 * shell's own audio output, so that it goes on while games run, as the Vita
 * plugin's queue plays on through SceShell's music service.
 *
 * It is driven with files in /data/ShellCompanion/ps4/ (bgm.h), the PS4's
 * counterpart of the Vita plugin's queue and commands:
 *
 *   queue.txt   the tracks, one absolute path a line; without it, every
 *               .mp3 and .wav in music/, by name
 *   cmd         commands, one a line, read and removed: play, pause, toggle, next,
 *               prev, stop, reload, quit
 *   port.txt    the audio port type, if not MAIN (0): 1 BGM, 14, ...
 *   status.txt  written each second: state, track, position, and a
 *               heartbeat that tells the loader a player is running
 *
 * The decoder is minimp3 (CC0). The PS4's audio output takes 48 kHz only, so
 * other rates are resampled, linearly. Its log is ps4/bgm.log.
 */

#include "../probe/sys.h"
#include "bgm.h"
#include "minimp3.h"

typedef int32_t (*FnOpen)(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                          uint32_t param);
typedef int32_t (*FnOutput)(int32_t handle, const void* p);
typedef int32_t (*FnClose)(int32_t handle);
typedef int32_t (*FnSetVolume)(int32_t handle, int32_t flag, int32_t* vol);
typedef int32_t (*FnInit)(void);

// libSceAudioOut, linked by the system's loader (-lSceAudioOut), as
// GoldHEN's plugins link what they use.
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                        uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void* p);
int32_t sceAudioOutClose(int32_t handle);
int32_t sceAudioOutSetVolume(int32_t handle, int32_t flag, int32_t* vol);

static FnOpen g_aoOpen;
static FnOutput g_aoOutput;
static FnClose g_aoClose;
static FnSetVolume g_aoVolume;

#define RATE 48000
#define GRAIN 1024  // frames a grain, ~21 ms
#define MAX_TRACKS 512
#define MAX_PATH 256
#define MAX_FILE (96u << 20)

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
static const char* stateName(int s) {
    return s == ST_PLAYING ? "playing" : s == ST_PAUSED ? "paused" : "stopped";
}

// The queue.
static char g_tracks[MAX_TRACKS][MAX_PATH];
static int g_count;
static int g_index;
static int g_state = ST_STOPPED;

// ---- a source: a whole file in memory, decoded a frame at a time ----

enum { SRC_NONE, SRC_MP3, SRC_WAV };
static struct {
    int type;
    uint8_t* data;
    size_t mapped, size, off;
    int channels, hz;
    // MP3: the frame decoded, and how far into it.
    mp3dec_t dec;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcmFrames, pcmAt;
    // WAV: where its samples end.
    size_t wavEnd;
    uint64_t framesRead;  // source frames, for the position
    uint64_t durMs;       // the track's length: an MP3's from its first frame's bit rate
    int kbps;             // that bit rate
} g_src;

static void srcClose(void) {
    if (g_src.data) sysUnmap(g_src.data, g_src.mapped);
    memset(&g_src, 0, sizeof(g_src));
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static int endsWith(const char* s, const char* tail) {
    const size_t n = strlen(s), m = strlen(tail);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++) {
        char c = s[n - m + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != tail[i]) return 0;
    }
    return 1;
}

static int wavOpen(void) {
    const uint8_t* d = g_src.data;
    if (g_src.size < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    size_t at = 12;
    int fmtOk = 0;
    while (at + 8 <= g_src.size) {
        const uint32_t len = le32(d + at + 4);
        const uint8_t* body = d + at + 8;
        if (!memcmp(d + at, "fmt ", 4) && len >= 16) {
            const int format = le16(body), bits = le16(body + 14);
            g_src.channels = le16(body + 2);
            g_src.hz = (int)le32(body + 4);
            fmtOk = format == 1 && bits == 16 && (g_src.channels == 1 || g_src.channels == 2);
        } else if (!memcmp(d + at, "data", 4)) {
            if (!fmtOk) return 0;
            g_src.off = at + 8;
            g_src.wavEnd = at + 8 + len > g_src.size ? g_src.size : at + 8 + len;
            return 1;
        }
        at += 8 + len + (len & 1);
    }
    return 0;
}

// Decodes MP3 frames until one has samples: 1, or 0 at the end.
static int mp3Decode(void) {
    mp3dec_frame_info_t info;
    while (g_src.off < g_src.size) {
        const int frames = mp3dec_decode_frame(&g_src.dec, g_src.data + g_src.off,
                                               (int)(g_src.size - g_src.off), g_src.pcm, &info);
        if (!info.frame_bytes) return 0;  // nothing more it can find
        g_src.off += (size_t)info.frame_bytes;
        if (frames > 0) {
            if (!g_src.kbps) g_src.kbps = info.bitrate_kbps;
            g_src.channels = info.channels;
            g_src.hz = info.hz;
            g_src.pcmFrames = frames;
            g_src.pcmAt = 0;
            return 1;
        }
    }
    return 0;
}

static int srcOpen(const char* path) {
    srcClose();
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) {
        logLine("%s: not opened (%d)", path, fd);
        return 0;
    }
    const int64_t size = sysSeek(fd, 0, 2);
    sysSeek(fd, 0, 0);
    if (size <= 0 || (uint64_t)size > MAX_FILE) {
        logLine("%s: its size, %ld, is not one we take", path, (long)size);
        sysClose(fd);
        return 0;
    }
    g_src.mapped = ((size_t)size + 0x3FFF) & ~(size_t)0x3FFF;
    g_src.data = (uint8_t*)sysMap(g_src.mapped);
    if (!g_src.data) {
        logLine("%s: no memory for %ld bytes", path, (long)size);
        g_src.mapped = 0;
        sysClose(fd);
        return 0;
    }
    size_t got = 0;
    while (got < (size_t)size) {
        const int n = sysRead(fd, g_src.data + got, (size_t)size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    sysClose(fd);
    g_src.size = got;
    if (endsWith(path, ".wav")) {
        g_src.type = SRC_WAV;
        if (!wavOpen()) {
            logLine("%s: not a 16-bit PCM WAV", path);
            srcClose();
            return 0;
        }
        g_src.durMs = g_src.hz && g_src.channels
                          ? (uint64_t)(g_src.wavEnd - g_src.off) / (uint64_t)(g_src.channels * 2) *
                                1000u / (uint64_t)g_src.hz
                          : 0;
    } else {
        g_src.type = SRC_MP3;
        // An ID3v2 tag first, cover art and all: skipped, its size syncsafe.
        if (g_src.size > 10 && !memcmp(g_src.data, "ID3", 3)) {
            const uint8_t* h = g_src.data;
            g_src.off = 10 + ((size_t)(h[6] & 0x7F) << 21 | (size_t)(h[7] & 0x7F) << 14 |
                              (size_t)(h[8] & 0x7F) << 7 | (size_t)(h[9] & 0x7F));
            if (h[5] & 0x10) g_src.off += 10;  // a footer
        }
        mp3dec_init(&g_src.dec);
        g_src.kbps = 0;
        const size_t audioAt = g_src.off;
        if (!mp3Decode()) {
            logLine("%s: no MP3 frames in it", path);
            srcClose();
            return 0;
        }
        // kbit/s is bits a millisecond.
        g_src.durMs = g_src.kbps ? (uint64_t)(g_src.size - audioAt) * 8u / (uint64_t)g_src.kbps : 0;
    }
    if (g_src.hz < 8000 || g_src.hz > 192000) {
        logLine("%s: a rate of %d Hz", path, g_src.hz);
        srcClose();
        return 0;
    }
    logLine("%s: %u bytes, %d Hz, %d channel(s)", path, (unsigned)got, g_src.hz, g_src.channels);
    return 1;
}

// One source frame, as stereo: 1, or 0 at its end.
static int srcFrame(int16_t out[2]) {
    if (g_src.type == SRC_WAV) {
        const size_t need = (size_t)g_src.channels * 2;
        if (g_src.off + need > g_src.wavEnd) return 0;
        const uint8_t* p = g_src.data + g_src.off;
        out[0] = (int16_t)le16(p);
        out[1] = g_src.channels == 2 ? (int16_t)le16(p + 2) : out[0];
        g_src.off += need;
    } else if (g_src.type == SRC_MP3) {
        if (g_src.pcmAt >= g_src.pcmFrames && !mp3Decode()) return 0;
        const int16_t* p = g_src.pcm + g_src.pcmAt * g_src.channels;
        out[0] = p[0];
        out[1] = g_src.channels == 2 ? p[1] : p[0];
        g_src.pcmAt++;
    } else {
        return 0;
    }
    g_src.framesRead++;
    return 1;
}

// ---- the resampler: linear, from the source's rate to 48 kHz ----

static struct {
    int16_t a[2], b[2];
    uint32_t frac, step;  // 16.16
    int ended;
} g_rs;

static void rsStart(void) {
    memset(&g_rs, 0, sizeof(g_rs));
    g_rs.step = (uint32_t)(((uint64_t)g_src.hz << 16) / RATE);
    if (!srcFrame(g_rs.a)) g_rs.ended = 1;
    if (!g_rs.ended && !srcFrame(g_rs.b)) g_rs.b[0] = g_rs.a[0], g_rs.b[1] = g_rs.a[1];
}

// A grain of 48 kHz stereo: the frames that came from the track; fewer
// than GRAIN at its end, the rest silence.
static int rsFill(int16_t* out) {
    int n = 0;
    for (; n < GRAIN && !g_rs.ended; n++) {
        while (g_rs.frac >= 0x10000) {
            g_rs.a[0] = g_rs.b[0];
            g_rs.a[1] = g_rs.b[1];
            if (!srcFrame(g_rs.b)) {
                g_rs.ended = 1;
                break;
            }
            g_rs.frac -= 0x10000;
        }
        if (g_rs.ended) break;
        const int32_t f = (int32_t)g_rs.frac;
        out[2 * n] = (int16_t)(g_rs.a[0] + (((g_rs.b[0] - g_rs.a[0]) * f) >> 16));
        out[2 * n + 1] = (int16_t)(g_rs.a[1] + (((g_rs.b[1] - g_rs.a[1]) * f) >> 16));
        g_rs.frac += g_rs.step;
    }
    for (int i = n; i < GRAIN; i++) out[2 * i] = out[2 * i + 1] = 0;
    return n;
}

// ---- files ----

static int readText(const char* path, char* out, size_t cap) {
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sysRead(fd, out, cap - 1);
    sysClose(fd);
    if (n < 0) n = 0;
    out[n] = 0;
    return n;
}

static void writeText(const char* path, const char* text) {
    // Written beside and renamed over, so that a reader never sees half.
    char tmp[128];
    fmt(tmp, sizeof(tmp), "%s.tmp", path);
    const int fd = sysOpen(tmp, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (fd < 0) return;
    sysWrite(fd, text, strlen(text));
    sysClose(fd);
    sysRename(tmp, path);
}

static int cmpName(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y || !x) return x - y;
    }
}

static void addTrack(const char* path, size_t len) {
    if (g_count >= MAX_TRACKS || !len || len >= MAX_PATH) return;
    memcpy(g_tracks[g_count], path, len);
    g_tracks[g_count][len] = 0;
    g_count++;
}

// Every .mp3 and .wav in music/, by name.
static void scanMusic(void) {
    const int fd = sysOpen(SC_BGM_MUSIC, SC_O_RDONLY | SC_O_DIRECTORY, 0);
    if (fd < 0) return;
    static uint8_t buf[0x2000];
    int n;
    while ((n = sysGetdents(fd, buf, sizeof(buf))) > 0) {
        for (int at = 0; at < n;) {
            const uint16_t reclen = le16(buf + at + 4);
            const uint8_t type = buf[at + 6], namlen = buf[at + 7];
            const char* name = (const char*)buf + at + 8;
            if (!reclen) break;
            if (type != 4 /* DT_DIR */ && namlen && name[0] != '.') {
                char path[MAX_PATH];
                fmt(path, sizeof(path), "%s/%s", SC_BGM_MUSIC, name);
                if (endsWith(path, ".mp3") || endsWith(path, ".wav")) addTrack(path, strlen(path));
            }
            at += reclen;
        }
    }
    sysClose(fd);
    // By name: insertion sort, as there are few.
    static char tmp[MAX_PATH];
    for (int i = 1; i < g_count; i++) {
        memcpy(tmp, g_tracks[i], MAX_PATH);
        int j = i - 1;
        for (; j >= 0 && cmpName(g_tracks[j], tmp) > 0; j--)
            memcpy(g_tracks[j + 1], g_tracks[j], MAX_PATH);
        memcpy(g_tracks[j + 1], tmp, MAX_PATH);
    }
}

// The queue, from queue.txt or music/. "#start N" in queue.txt picks the
// first track (from 0).
static void loadQueue(void) {
    static char text[MAX_TRACKS * 96];
    g_count = 0;
    g_index = 0;
    const int n = readText(SC_BGM_QUEUE, text, sizeof(text));
    int start = 0;
    if (n > 0) {
        for (int at = 0; at < n;) {
            int end = at;
            while (end < n && text[end] != '\n' && text[end] != '\r') end++;
            const char* line = text + at;
            const int len = end - at;
            if (len > 7 && !memcmp(line, "#start ", 7)) {
                for (int i = 7; i < len && line[i] >= '0' && line[i] <= '9'; i++)
                    start = start * 10 + (line[i] - '0');
            } else if (len > 0 && line[0] == '/') {
                addTrack(line, (size_t)len);
            }
            at = end + 1;
        }
        logLine("queue.txt: %d track(s)", g_count);
    } else {
        scanMusic();
        logLine("no queue.txt; music/: %d track(s)", g_count);
    }
    if (start < g_count) g_index = start;
}

static const char* baseName(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

static uint64_t g_lastStatus;

static void writeStatus(void) {
    static char text[1024];
    const uint64_t posMs = g_src.hz ? g_src.framesRead * 1000u / (uint64_t)g_src.hz : 0;
    fmt(text, sizeof(text),
        "protocol=%d\nstate=%s\nindex=%d\ncount=%d\npos_ms=%lu\ndur_ms=%lu\nfile=%s\nbeat_ms=%lu\n",
        SC_BGM_PROTOCOL, stateName(g_state), g_index, g_count, (uint64_t)posMs, g_src.durMs,
        g_count ? g_tracks[g_index] : "", (uint64_t)(sysNowUs() / 1000u));
    writeText(SC_BGM_STATUS, text);
    g_lastStatus = sysNowUs();
}

// ---- the player ----

static int g_port = -1;
static int g_portType;
static int16_t g_out[GRAIN * 2];

static void portClose(void) {
    if (g_port >= 0) logLine("port closed: 0x%08X", (unsigned)g_aoClose(g_port));
    g_port = -1;
}

static int portOpen(void) {
    if (g_port >= 0) return 1;
    char text[16];
    g_portType = 0;
    if (readText(SC_BGM_PORT, text, sizeof(text)) > 0)
        for (int i = 0; text[i] >= '0' && text[i] <= '9'; i++)
            g_portType = g_portType * 10 + (text[i] - '0');
    g_port = g_aoOpen(255 /* system */, g_portType, 0, GRAIN, RATE, 1 /* S16 stereo */);
    logLine("port type %d opened: 0x%08X", g_portType, (unsigned)g_port);
    if (g_port < 0) {
        notify("Background music: no audio port (type %d, 0x%08X)", g_portType, (unsigned)g_port);
        g_port = -1;
        return 0;
    }
    if (g_aoVolume) {
        int32_t vol[8];
        for (int i = 0; i < 8; i++) vol[i] = 32768;  // 0 dB
        g_aoVolume(g_port, 0xFF, vol);
    }
    return 1;
}

// Opens the track at g_index, or the next that opens: 1, or 0 when none
// is left.
static int openTrack(void) {
    for (; g_index < g_count; g_index++) {
        if (srcOpen(g_tracks[g_index])) {
            rsStart();
            notify("Now playing: %s", baseName(g_tracks[g_index]));
            return 1;
        }
    }
    return 0;
}

static void stop(const char* why) {
    srcClose();
    portClose();
    g_state = ST_STOPPED;
    logLine("stopped: %s", why);
}

static int g_quit;

// One command, a word.
static void runCommand(const char* text, int n) {
    logLine("command: %s", text);  // nul-terminated by command()
#define IS(word) (n == (int)sizeof(word) - 1 && !memcmp(text, word, n))
    if (IS("play")) {
        if (g_state == ST_STOPPED) {
            if (!g_count) loadQueue();
            if (g_index >= g_count) g_index = 0;
            if (openTrack()) g_state = ST_PLAYING;
            else stop("nothing in the queue would open");
        } else {
            g_state = ST_PLAYING;
        }
    } else if (IS("pause")) {
        if (g_state == ST_PLAYING) g_state = ST_PAUSED;
    } else if (IS("toggle")) {
        if (g_state == ST_PLAYING) g_state = ST_PAUSED;
        else if (g_state == ST_PAUSED) g_state = ST_PLAYING;
    } else if (IS("next") || IS("prev")) {
        if (!g_count) return;
        const uint64_t posMs = g_src.hz ? g_src.framesRead * 1000u / (uint64_t)g_src.hz : 0;
        if (IS("next")) g_index = g_index + 1 < g_count ? g_index + 1 : 0;
        else if (posMs < 3000) g_index = g_index > 0 ? g_index - 1 : g_count - 1;
        // (prev after 3 s starts the track again, as players do)
        if (g_state == ST_STOPPED) g_state = ST_PAUSED;
        if (!openTrack()) stop("nothing after it would open");
    } else if (IS("stop")) {
        stop("asked");
    } else if (IS("reload")) {
        // The queue again, from its start; playing if it was. The port
        // too, as port.txt may have changed.
        const int was = g_state;
        stop("reloading");
        loadQueue();
        if (was != ST_STOPPED && openTrack()) g_state = ST_PLAYING;
    } else if (IS("quit")) {
        stop("asked to quit");
        g_quit = 1;
    }
#undef IS
}

// The commands in cmd, one a line, if it is there. It is renamed first, so
// that a command written meanwhile is not lost, nor one read half written.
static void command(void) {
    char text[256];
    if (sysRename(SC_BGM_CMD, SC_BGM_CMD ".work") < 0) return;
    const int len = readText(SC_BGM_CMD ".work", text, sizeof(text));
    sysUnlink(SC_BGM_CMD ".work");
    for (int at = 0; at < len;) {
        int end = at;
        while (end < len && text[end] != '\n' && text[end] != '\r' && text[end] != ' ') end++;
        const int next = end + 1;
        text[end] = 0;  // text has room: readText leaves one byte
        if (end > at) runCommand(text + at, end - at);
        at = next;
    }
    writeStatus();
}

static void* player(void* arg) {
    (void)arg;
    const int lib = 0;
    FnInit init = sceAudioOutInit;
    g_aoOpen = sceAudioOutOpen;
    g_aoOutput = sceAudioOutOutput;
    g_aoClose = sceAudioOutClose;
    g_aoVolume = sceAudioOutSetVolume;
    if (!init || !g_aoOpen || !g_aoOutput || !g_aoClose) {
        logLine("libSceAudioOut 0x%X: its functions are missing", lib);
        notify("Background music: no audio library");
        return NULL;
    }
    logLine("sceAudioOutInit: 0x%08X", (unsigned)init());  // already done in the shell: fine

    loadQueue();
    if (g_count && openTrack()) g_state = ST_PLAYING;
    else notify("Background music: ready. Put tracks in " SC_BGM_MUSIC);
    writeStatus();

    uint32_t grains = 0;
    while (!g_quit) {
        if (g_state != ST_PLAYING) {
            if (g_state == ST_PAUSED) portClose();  // the port, back for others while paused
            command();
            if (sysNowUs() - g_lastStatus > 1000000u) writeStatus();
            sysSleepMs(100);
            continue;
        }
        if ((grains++ & 3) == 0) command();  // ~every 85 ms
        if (g_state != ST_PLAYING) continue;
        if (!portOpen()) {
            stop("no port");
            continue;
        }
        const int frames = rsFill(g_out);
        if (frames > 0) g_aoOutput(g_port, g_out);
        if (frames < GRAIN) {
            // The track ended: the next, or the end of the queue.
            g_index++;
            if (!openTrack()) {
                g_index = 0;
                stop("the end of the queue");
                notify("Background music: the queue has ended");
            }
        }
        if (sysNowUs() - g_lastStatus > 1000000u) writeStatus();
    }
    writeStatus();
    logLine("player: done");
    return NULL;
}

// One import through the module's import table, for its .got.plt, which a
// .prx must have; the system's loader resolves it, inside the shell.
int sceKernelUsleep(unsigned int us);

// The player's start, on its own thread: its folders, its log, then the
// player. module_start does nothing but start it, so that the shell, which
// waits for module_start, is never kept waiting.
static void* begin(void* arg) {
    sysMkdir("/user/data/ShellCompanion", 0777);
    sysMkdir(SC_BGM_DIR, 0777);
    sysMkdir(SC_BGM_MUSIC, 0777);
    logOpen("ps4/bgm.log");
    char fw[64];
    firmware(fw, sizeof(fw));
    logLine("background music %s: started, firmware %s", SC_BGM_VERSION, fw);
    return player(arg);
}

__attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    const int rc = threadStart(begin, NULL, "ScBgm");
    if (rc < 0) notify("Background music: its thread would not start (0x%08X)", (unsigned)rc);
    return 0;
}

__attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}
