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
#define MAX_TRACKS 10000  // 0.84: 512 before; the tables hold pointers into g_pool
#define MAX_PATH 768  // a path, or a URL (0.72), Plex's with its token
#define MAX_FILE (96u << 20)

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
static const char* stateName(int s) {
    return s == ST_PLAYING ? "playing" : s == ST_PAUSED ? "paused" : "stopped";
}

// The library (every track loaded, by its number: a track's id, sc<n>, in
// the Music section) and the queue (the library's numbers, in the order they
// play: g_count of them, g_index the one playing). The Music section edits
// the queue (0.67): an album played becomes it, a track moved, taken out, or
// put next or last in it.
// Text kept for the library (paths, URLs, tags), in chunks of 1 MB mapped as
// needed and freed when the library is loaded again (0.84: fixed arrays
// before, 2 MB for 512 tracks, which 4000 would have made 15).
#define POOL_CHUNK (1u << 20)
static struct {
    char* chunk[512];
    int n;
    size_t used;
} g_pool;

static void poolReset(void) {
    for (int i = 0; i < g_pool.n; i++) sysUnmap(g_pool.chunk[i], POOL_CHUNK);
    g_pool.n = 0;
    g_pool.used = 0;
}

// len bytes of s kept, nul-terminated; "" for none (or no memory).
static const char* poolStr(const char* s, size_t len) {
    if (!s || !len) return "";
    if (len >= POOL_CHUNK) len = POOL_CHUNK - 1;
    if (!g_pool.n || g_pool.used + len + 1 > POOL_CHUNK) {
        if (g_pool.n >= 512) return "";
        char* c = (char*)sysMap(POOL_CHUNK);
        if (!c) return "";
        g_pool.chunk[g_pool.n++] = c;
        g_pool.used = 0;
    }
    char* p = g_pool.chunk[g_pool.n - 1] + g_pool.used;
    memcpy(p, s, len);
    p[len] = 0;
    g_pool.used += len + 1;
    return p;
}

static const char* poolCopy(const char* s) {
    return s ? poolStr(s, strlen(s)) : "";
}

static const char* g_tracks[MAX_TRACKS];
static int g_libCount;
static int g_q[MAX_TRACKS];
static int g_count;
static int g_index;
static int g_queueRev;  // counts the queue's changes, in status.txt (queue_rev)
// Where in the first track to begin (queue.txt's "#position <ms>", the
// client library's ScQueue.positionMs), and where the music was when an app
// took it back (the "takeback" command), for status.txt's pos_ms (0.81).
static uint64_t g_seekMs, g_heldPosMs;
// A streamed track's tags as queue.txt gives them (0.72): URL, then title,
// artist, album and a cover's URL, tab-separated; "" where not given.
#define GIVEN_TEXT 160
static struct {
    const char *title, *artist, *album, *cover;  // in g_pool, "" where not given
    const char* id;  // the app's own id for the track (0.86, its sixth field), for "+"
} g_given[MAX_TRACKS];

// A path's track by its hash (FNV-1a), for streams.txt's and liked.txt's
// (0.84: they were looked for one track at a time).
#define PATH_SLOTS 32768
static int g_pathSlot[PATH_SLOTS];  // a track + 1, 0 for none

static uint32_t hashBytes(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static int pathFind(const char* s, size_t n) {
    for (uint32_t k = hashBytes(s, n) & (PATH_SLOTS - 1);; k = (k + 1) & (PATH_SLOTS - 1)) {
        const int t = g_pathSlot[k] - 1;
        if (t < 0) return -1;
        if (strlen(g_tracks[t]) == n && !memcmp(g_tracks[t], s, n)) return t;
    }
}

static void pathAdd(int i) {
    const size_t n = strlen(g_tracks[i]);
    uint32_t k = hashBytes(g_tracks[i], n) & (PATH_SLOTS - 1);
    while (g_pathSlot[k]) k = (k + 1) & (PATH_SLOTS - 1);
    g_pathSlot[k] = i + 1;
}

static void pathIndex(void) {
    memset(g_pathSlot, 0, sizeof(g_pathSlot));
    for (int i = 0; i < g_libCount; i++) pathAdd(i);
}

// A whole file into memory mapped for it, nul-terminated: its length, or -1
// (0.84: fixed buffers before, 48 KB for queue.txt). *mapped to unmap.
static int64_t readMapped(const char* path, char** out, size_t* mapped) {
    *out = NULL;
    *mapped = 0;
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) return -1;
    const int64_t size = sysSeek(fd, 0, 2);
    sysSeek(fd, 0, 0);
    if (size < 0 || size > (int64_t)(64u << 20)) {
        sysClose(fd);
        return -1;
    }
    *mapped = ((size_t)size + 1 + 0x3FFF) & ~(size_t)0x3FFF;
    *out = (char*)sysMap(*mapped);
    if (!*out) {
        sysClose(fd);
        *mapped = 0;
        return -1;
    }
    int64_t got = 0;
    while (got < size) {
        const int k = sysRead(fd, *out + got, (size_t)(size - got));
        if (k <= 0) break;
        got += k;
    }
    sysClose(fd);
    (*out)[got] = 0;
    return got;
}
static int g_state = ST_STOPPED;
// The volume, 0 to 32768 (0 dB): the quick menu's, through the music watch
// (volume=<n> as cmd).
static int32_t g_volume = 32768;
// Repeat (0 none, 1 all, 2 one) and shuffle (0, 1), as the quick menu sets
// them (repeat=<n>, shuffle=<n> as cmd).
static int g_repeat, g_shuffle;
static int g_played;  // tracks played since the queue began, for shuffle
static uint32_t g_rand = 0x9E3779B9u;

static int otherTrack(void) {
    if (g_count < 2) return g_index;
    g_rand = g_rand * 1664525u + 1013904223u + (uint32_t)sysNowUs();
    const int r = (int)((g_rand >> 8) % (uint32_t)(g_count - 1));
    return r >= g_index ? r + 1 : r;
}

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
    // WAV: where its samples end (wavFull: as its header says, of a stream
    // still coming).
    size_t wavEnd, wavFull;
    uint64_t framesRead;  // source frames, for the position
    uint64_t durMs;       // the track's length: an MP3's from its first frame's bit rate
    int kbps;             // that bit rate
    int streamed;         // from a URL, still coming (g_dl): size grows (0.72)
    int buffering;        // waiting for more of it
} g_src;

// ---- streaming (0.72): a track's URL fetched with the shell's own HTTP
// library (libSceHttp, through libSceNet and libSceSsl), into memory, on a
// thread of its own; it plays from what has come while the rest comes.
// Nothing of it is written to a file. ----

typedef int (*FnNetPool)(const char*, int, int);
typedef int (*FnSslInit)(size_t);
typedef int (*FnHttpInit)(int, int, size_t);
typedef int (*FnHttpTmpl)(int, const char*, int, int);
typedef int (*FnHttpConn)(int, const char*, int);
typedef int (*FnHttpReq)(int, int32_t, const char*, uint64_t);
typedef int (*FnHttpSend)(int, const void*, size_t);
typedef int (*FnHttpStatus)(int, int*);
typedef int (*FnHttpLength)(int, int*, uint64_t*);
typedef int (*FnHttpRead)(int, void*, size_t);
typedef int (*FnHttpId)(int);
typedef int (*FnHttpTimeout)(int, uint32_t);
typedef int (*FnHttpsOption)(int, uint32_t);
typedef int (*FnHttpHeader)(int, const char*, const char*, uint32_t);
static struct {
    int tried, ok, ctx;
    FnHttpTmpl tmpl;
    FnHttpConn conn;
    FnHttpReq req;
    FnHttpSend send;
    FnHttpStatus status;
    FnHttpLength length;
    FnHttpRead read;
    FnHttpId delReq, delConn, delTmpl;
    FnHttpTimeout connectTimeout, recvTimeout;
    FnHttpsOption httpsOff;
    FnHttpHeader addHeader;  // sceHttpAddRequestHeader (0.82, downloads)
    volatile int lock;
} g_http;

// A function from the module given, else from any module the shell has
// loaded (0.75: on 11.50 its TLS library is libSceSsl2.sprx).
static void* anySym(int handle, const char* symbol) {
    void* p = handle ? libSym(handle, symbol) : NULL;
    if (p) return p;
    int handles[256], count = 0;
    if (sysCall(592, (long)handles, 256, (long)&count, 0, 0, 0) < 0) return NULL;
    for (int i = 0; i < count && i < 256; i++)
        if ((p = libSym(handles[i], symbol))) return p;
    return NULL;
}

// libSceHttp, once: 1 if it is there to use.
static int httpSetupOnce(void);
// Two threads ask (a stream's, the downloads'): one sets it up.
static int httpSetup(void) {
    while (__atomic_exchange_n(&g_http.lock, 1, __ATOMIC_ACQUIRE)) sysSleepMs(1);
    const int ok = httpSetupOnce();
    __atomic_store_n(&g_http.lock, 0, __ATOMIC_RELEASE);
    return ok;
}

static int httpSetupOnce(void) {
    if (g_http.tried) return g_http.ok;
    g_http.tried = 1;
    const int net = libLoad("libSceNet.sprx"), http = libLoad("libSceHttp.sprx");
    int ssl = libLoad("libSceSsl.sprx");
    if (!ssl) ssl = libLoad("libSceSsl2.sprx");
    FnNetPool pool = (FnNetPool)anySym(net, "sceNetPoolCreate");
    FnSslInit sslInit = (FnSslInit)anySym(ssl, "sceSslInit");
    FnHttpInit httpInit = (FnHttpInit)anySym(http, "sceHttpInit");
    logLine("http: libSceNet %d, libSceSsl(2) %d, libSceHttp %d; sceNetPoolCreate %p, sceSslInit %p, "
            "sceHttpInit %p", net, ssl, http, (void*)pool, (void*)sslInit, (void*)httpInit);
#define HTTP_SYM(field, type, name)                    \
    do {                                               \
        g_http.field = (type)anySym(http, name);       \
        if (!g_http.field) logLine("http: no %s", name); \
    } while (0)
    HTTP_SYM(tmpl, FnHttpTmpl, "sceHttpCreateTemplate");
    HTTP_SYM(conn, FnHttpConn, "sceHttpCreateConnectionWithURL");
    HTTP_SYM(req, FnHttpReq, "sceHttpCreateRequestWithURL");
    HTTP_SYM(send, FnHttpSend, "sceHttpSendRequest");
    HTTP_SYM(status, FnHttpStatus, "sceHttpGetStatusCode");
    HTTP_SYM(length, FnHttpLength, "sceHttpGetResponseContentLength");
    HTTP_SYM(read, FnHttpRead, "sceHttpReadData");
    HTTP_SYM(delReq, FnHttpId, "sceHttpDeleteRequest");
    HTTP_SYM(delConn, FnHttpId, "sceHttpDeleteConnection");
    HTTP_SYM(delTmpl, FnHttpId, "sceHttpDeleteTemplate");
    HTTP_SYM(connectTimeout, FnHttpTimeout, "sceHttpSetConnectTimeOut");
    HTTP_SYM(recvTimeout, FnHttpTimeout, "sceHttpSetRecvTimeOut");
    HTTP_SYM(httpsOff, FnHttpsOption, "sceHttpsDisableOption");
    HTTP_SYM(addHeader, FnHttpHeader, "sceHttpAddRequestHeader");
#undef HTTP_SYM
    if (!pool || !sslInit || !httpInit || !g_http.tmpl || !g_http.conn || !g_http.req ||
        !g_http.send || !g_http.status || !g_http.length || !g_http.read || !g_http.delReq ||
        !g_http.delConn || !g_http.delTmpl) {
        logLine("http: functions missing (above)");
        return 0;
    }
    const int memId = pool("ScBgmNet", 64 * 1024, 0);
    const int sslId = sslInit(256 * 1024);
    g_http.ctx = httpInit(memId, sslId, 256 * 1024);
    logLine("http: net pool %d, ssl %d, http %d", memId, sslId, g_http.ctx);
    g_http.ok = memId >= 0 && sslId >= 0 && g_http.ctx >= 0;
    return g_http.ok;
}

static int isUrl(const char* s) {
    return !memcmp(s, "http://", 7) || !memcmp(s, "https://", 8);
}

// The URL as the log shows it: up to its query, which may hold a token.
static void urlShown(const char* url, char* out, size_t cap) {
    size_t k = 0;
    for (; url[k] && url[k] != '?' && k + 1 < cap; k++) out[k] = url[k];
    out[k] = 0;
    if (url[k] == '?' && k + 4 < cap) fmt(out + k, cap - k, "?...");
}

#define DL_FIRST (16u << 20)  // what is taken when the server gives no length
static struct {
    volatile int running, stop, ready, failed, done, orphan;
    volatile size_t have;  // bytes in buf
    uint64_t total;        // its length, 0 if not known
    uint8_t* buf;
    size_t cap;
    int code;
    char url[MAX_PATH];
} g_dl;

static void* dlThread(void* arg) {
    (void)arg;
    char shown[160];
    urlShown(g_dl.url, shown, sizeof(shown));
    const uint64_t t0 = sysNowUs();
    int tmpl = -1, conn = -1, req = -1;
    uint8_t* mine = NULL;
    size_t mineCap = 0;
    if (!httpSetup()) goto fail;
    tmpl = g_http.tmpl(g_http.ctx, "ShellCompanion/" SC_BGM_VERSION, 2 /* HTTP/1.1 */, 1);
    if (tmpl < 0) goto fail;
    if (g_http.httpsOff && sysExists(SC_BGM_DIR "/https-noverify"))
        g_http.httpsOff(tmpl, 0x01 | 0x04 | 0x08 | 0x10 | 0x20);  // a server's own certificate
    conn = g_http.conn(tmpl, g_dl.url, 1);
    if (conn < 0) goto fail;
    req = g_http.req(conn, 0 /* GET */, g_dl.url, 0);
    if (req < 0) goto fail;
    if (g_http.connectTimeout) g_http.connectTimeout(req, 10u * 1000000u);
    if (g_http.recvTimeout) g_http.recvTimeout(req, 15u * 1000000u);
    int rc = g_http.send(req, NULL, 0);
    if (rc < 0) {
        logLine("stream %s: not sent: 0x%08X", shown, (unsigned)rc);
        goto fail;
    }
    g_http.status(req, &g_dl.code);
    int known = -1;
    uint64_t len = 0;
    g_http.length(req, &known, &len);
    logLine("stream %s: HTTP %d, %s %lu, after %lu ms", shown, g_dl.code,
            known == 0 ? "length" : "no length, up to", known == 0 ? len : (uint64_t)DL_FIRST,
            (sysNowUs() - t0) / 1000u);
    if (g_dl.code < 200 || g_dl.code > 299) goto fail;
    g_dl.total = known == 0 ? len : 0;
    g_dl.cap = known == 0 && len ? (size_t)len : DL_FIRST;
    if (g_dl.cap > MAX_FILE) g_dl.cap = MAX_FILE;
    g_dl.cap = (g_dl.cap + 0x3FFF) & ~(size_t)0x3FFF;
    mine = (uint8_t*)sysMap(g_dl.cap);
    mineCap = g_dl.cap;
    g_dl.buf = mine;
    if (!g_dl.buf) {
        logLine("stream %s: no memory for %lu bytes", shown, (uint64_t)g_dl.cap);
        goto fail;
    }
    g_dl.ready = 1;
    while (!g_dl.stop && g_dl.have < mineCap) {
        size_t want = mineCap - g_dl.have;
        if (want > 64 * 1024) want = 64 * 1024;
        const int n = g_http.read(req, mine + g_dl.have, want);
        if (n < 0) {
            logLine("stream %s: read failed at %lu: 0x%08X", shown, (uint64_t)g_dl.have, (unsigned)n);
            break;
        }
        if (n == 0) break;
        __atomic_store_n(&g_dl.have, g_dl.have + (size_t)n, __ATOMIC_RELEASE);
    }
    if (g_dl.have >= g_dl.cap && !g_dl.total) logLine("stream %s: cut at %lu bytes", shown, (uint64_t)g_dl.cap);
    logLine("stream %s: %lu bytes in %lu ms%s", shown, (uint64_t)g_dl.have, (sysNowUs() - t0) / 1000u,
            g_dl.stop ? ", stopped" : "");
    g_dl.done = 1;
    goto out;
fail:
    logLine("stream %s: failed (template %d, connection %d, request %d, HTTP %d)", shown, tmpl, conn,
            req, g_dl.code);
    g_dl.failed = 1;
    g_dl.done = 1;
out:
    if (req >= 0) g_http.delReq(req);
    if (conn >= 0) g_http.delConn(conn);
    if (tmpl >= 0) g_http.delTmpl(tmpl);
    // Given up on by the player meanwhile (srcClose): its memory is freed here.
    if (g_dl.orphan && mine) sysUnmap(mine, mineCap);
    __atomic_store_n(&g_dl.running, 0, __ATOMIC_RELEASE);
    return NULL;
}

// The download stopped: 1, or 0 if it is still running after ms (it frees
// its memory itself then).
static int dlStop(uint32_t ms) {
    if (!g_dl.running) return 1;
    g_dl.stop = 1;
    for (uint32_t w = 0; w < ms && g_dl.running; w += 10) sysSleepMs(10);
    if (!g_dl.running) return 1;
    g_dl.orphan = 1;
    logLine("stream: still reading after %u ms, left to end on its own", ms);
    return 0;
}

static void srcClose(void) {
    if (g_src.streamed) {
        // Its memory is the download's.
        if (dlStop(2000) && g_dl.buf) sysUnmap(g_dl.buf, g_dl.cap);
        g_dl.buf = NULL;
    } else if (g_src.data) {
        sysUnmap(g_src.data, g_src.mapped);
    }
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
            g_src.wavFull = at + 8 + (size_t)len;
            g_src.wavEnd = g_src.wavFull > g_src.size ? g_src.size : g_src.wavFull;
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

// ---- an MP3's tags, for the quick menu: title, artist, album, cover art ----

#define META_LEN 300
static struct {
    char title[META_LEN], artist[META_LEN], album[META_LEN];
    uint64_t artOff, artLen;  // the cover's image in the file (APIC), or 0
    char cover[128];          // that image as a file of its own, or ""
} g_meta;
static unsigned g_coverN;

// The cover as a file of its own, for the Music section's panel, which loads
// a picture by its address alone (Spotify's, CoverArtLoadAsync(uri)): a new
// name each track, so that no cached picture is shown for another.
static void coverWrite(const uint8_t* data) {
    if (g_meta.cover[0]) sysUnlink(g_meta.cover);
    g_meta.cover[0] = 0;
    if (!g_meta.artLen || g_meta.artLen > (8u << 20)) return;
    const uint8_t* img = data + g_meta.artOff;
    const int png = img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G';
    char path[128];
    fmt(path, sizeof(path), SC_BGM_DIR "/cover-%u.%s", g_coverN++ % 1000u, png ? "png" : "jpg");
    const int fd = sysOpen(path, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (fd < 0) return;
    const int n = sysWrite(fd, img, (size_t)g_meta.artLen);
    sysClose(fd);
    if (n == (int)g_meta.artLen) fmt(g_meta.cover, sizeof(g_meta.cover), "%s", path);
}

static uint32_t syncsafe(const uint8_t* p) {
    return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 |
           (uint32_t)(p[2] & 0x7F) << 7 | (uint32_t)(p[3] & 0x7F);
}
static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static size_t putUtf8(char* out, size_t at, size_t cap, uint32_t c) {
    if (c < 0x80) {
        if (at + 1 < cap) out[at++] = (char)c;
    } else if (c < 0x800) {
        if (at + 2 < cap) {
            out[at++] = (char)(0xC0 | (c >> 6));
            out[at++] = (char)(0x80 | (c & 0x3F));
        }
    } else if (c < 0x10000) {
        if (at + 3 < cap) {
            out[at++] = (char)(0xE0 | (c >> 12));
            out[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[at++] = (char)(0x80 | (c & 0x3F));
        }
    } else if (at + 4 < cap) {
        out[at++] = (char)(0xF0 | (c >> 18));
        out[at++] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[at++] = (char)(0x80 | (c & 0x3F));
    }
    return at;
}

// A text frame's body (its encoding byte first) as UTF-8, one line.
static void id3Text(const uint8_t* b, size_t n, char* out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    if (!n) return;
    const uint8_t enc = b[0];
    b++;
    n--;
    if (enc == 0 || enc == 3) {
        for (size_t i = 0; i < n && b[i]; i++)
            o = enc == 3 ? (o + 1 < cap ? (out[o] = (char)b[i], o + 1) : o)
                         : putUtf8(out, o, cap, b[i]);
    } else {
        int le = enc == 1;  // UTF-16 with a BOM; 2 is UTF-16BE
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            le = !(b[0] == 0xFE && b[1] == 0xFF);
            if ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF)) i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t c = le ? (uint32_t)(b[i] | b[i + 1] << 8) : (uint32_t)(b[i] << 8 | b[i + 1]);
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                const uint32_t lo = le ? (uint32_t)(b[i + 2] | b[i + 3] << 8)
                                       : (uint32_t)(b[i + 2] << 8 | b[i + 3]);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            o = putUtf8(out, o, cap, c);
        }
    }
    out[o < cap ? o : cap - 1] = 0;
    for (size_t i = 0; out[i]; i++)
        if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
}

// ID3v2.3 and 2.4 at the file's start: TIT2, TPE1, TALB and the first APIC.
static void id3Read(const uint8_t* d, size_t n) {
    memset(&g_meta, 0, sizeof(g_meta));
    if (n < 10 || memcmp(d, "ID3", 3) != 0 || (d[3] != 3 && d[3] != 4)) return;
    const int v4 = d[3] == 4;
    size_t end = 10 + (size_t)syncsafe(d + 6);
    if (end > n) end = n;
    size_t p = 10;
    if ((d[5] & 0x40) && p + 4 <= end) p += v4 ? syncsafe(d + p) : be32(d + p) + 4;
    while (p + 10 <= end) {
        const uint8_t* id = d + p;
        if (!id[0]) break;
        const size_t size = v4 ? syncsafe(id + 4) : be32(id + 4);
        const size_t body = p + 10;
        if (body + size > end) break;
        const uint8_t* b = d + body;
        if (!memcmp(id, "TIT2", 4)) id3Text(b, size, g_meta.title, META_LEN);
        else if (!memcmp(id, "TPE1", 4)) id3Text(b, size, g_meta.artist, META_LEN);
        else if (!memcmp(id, "TALB", 4)) id3Text(b, size, g_meta.album, META_LEN);
        else if (!memcmp(id, "APIC", 4) && !g_meta.artLen && size > 4) {
            // encoding, MIME type\0, picture type, description\0 (\0\0 in UTF-16), the image
            const uint8_t enc = b[0];
            size_t q = 1;
            while (q < size && b[q]) q++;
            q += 2;  // its \0 and the picture type
            if (enc == 1 || enc == 2) {
                while (q + 1 < size && (b[q] || b[q + 1])) q += 2;
                q += 2;
            } else {
                while (q < size && b[q]) q++;
                q++;
            }
            if (q < size) {
                g_meta.artOff = body + q;
                g_meta.artLen = size - q;
            }
        }
        p = body + size;
    }
}

// A stream's start: its download begun, and its first bytes waited for
// (an ID3 tag's whole, cover and all, and 192 KB of audio past it). 1, or 0.
static int srcFetch(const char* url) {
    char shown[160];
    urlShown(url, shown, sizeof(shown));
    for (uint32_t w = 0; g_dl.running && w < 15000; w += 50) sysSleepMs(50);  // one before it, ending
    if (g_dl.running) {
        logLine("stream %s: the one before still ends", shown);
        return 0;
    }
    memset((void*)&g_dl, 0, sizeof(g_dl));
    fmt(g_dl.url, sizeof(g_dl.url), "%s", url);
    g_dl.running = 1;
    if (threadStart(dlThread, NULL, "ScBgmFetch") < 0) {
        g_dl.running = 0;
        logLine("stream %s: no thread", shown);
        return 0;
    }
    const uint64_t t0 = sysNowUs();
    size_t want = 192 * 1024;
    while (!g_dl.done) {
        const size_t have = __atomic_load_n(&g_dl.have, __ATOMIC_ACQUIRE);
        if (g_dl.ready && have >= 10 && !memcmp(g_dl.buf, "ID3", 3)) {
            const uint8_t* h = g_dl.buf;
            const size_t tag = 10 + ((size_t)(h[6] & 0x7F) << 21 | (size_t)(h[7] & 0x7F) << 14 |
                                     (size_t)(h[8] & 0x7F) << 7 | (size_t)(h[9] & 0x7F));
            if (tag + 192 * 1024 > want) want = tag + 192 * 1024;
        }
        if (g_dl.ready && have >= want) break;
        if (sysNowUs() - t0 > 30000000u) {
            logLine("stream %s: 30 s and only %lu bytes", shown, (uint64_t)have);
            if (dlStop(2000) && g_dl.buf) sysUnmap(g_dl.buf, g_dl.cap);
            g_dl.buf = NULL;
            return 0;
        }
        sysSleepMs(20);
    }
    if (!g_dl.ready || (g_dl.failed && !g_dl.have)) {
        if (dlStop(2000) && g_dl.buf) sysUnmap(g_dl.buf, g_dl.cap);
        g_dl.buf = NULL;
        return 0;
    }
    g_src.streamed = 1;
    g_src.data = g_dl.buf;
    g_src.mapped = g_dl.cap;
    g_src.size = __atomic_load_n(&g_dl.have, __ATOMIC_ACQUIRE);
    logLine("stream %s: playing from %lu bytes, after %lu ms", shown, (uint64_t)g_src.size,
            (sysNowUs() - t0) / 1000u);
    return 1;
}

// More of a stream: its size as far as it has come.
static void srcGrow(void) {
    if (!g_src.streamed) return;
    g_src.size = __atomic_load_n(&g_dl.have, __ATOMIC_ACQUIRE);
    if (g_src.type == SRC_WAV) g_src.wavEnd = g_src.wavFull > g_src.size ? g_src.size : g_src.wavFull;
}

static int srcOpen(const char* path) {
    srcClose();
    char shownPath[160];
    urlShown(path, shownPath, sizeof(shownPath));
    size_t got = 0;
    if (isUrl(path)) {
        if (!srcFetch(path)) return 0;
        got = g_src.size;
    } else {
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) {
        logLine("%s: not opened (%d)", shownPath, fd);
        return 0;
    }
    const int64_t size = sysSeek(fd, 0, 2);
    sysSeek(fd, 0, 0);
    if (size <= 0 || (uint64_t)size > MAX_FILE) {
        logLine("%s: its size, %ld, is not one we take", shownPath, (long)size);
        sysClose(fd);
        return 0;
    }
    g_src.mapped = ((size_t)size + 0x3FFF) & ~(size_t)0x3FFF;
    g_src.data = (uint8_t*)sysMap(g_src.mapped);
    if (!g_src.data) {
        logLine("%s: no memory for %ld bytes", shownPath, (long)size);
        g_src.mapped = 0;
        sysClose(fd);
        return 0;
    }
    while (got < (size_t)size) {
        const int n = sysRead(fd, g_src.data + got, (size_t)size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    sysClose(fd);
    g_src.size = got;
    }
    // WAV by its RIFF header (a URL may not end in .wav), else MP3.
    if (endsWith(path, ".wav") || (g_src.size >= 12 && !memcmp(g_src.data, "RIFF", 4))) {
        if (g_meta.cover[0]) sysUnlink(g_meta.cover);
        memset(&g_meta, 0, sizeof(g_meta));
        g_src.type = SRC_WAV;
        if (!wavOpen()) {
            logLine("%s: not a 16-bit PCM WAV", shownPath);
            srcClose();
            return 0;
        }
        g_src.durMs = g_src.hz && g_src.channels
                          ? (uint64_t)(g_src.wavFull - g_src.off) / (uint64_t)(g_src.channels * 2) *
                                1000u / (uint64_t)g_src.hz
                          : 0;
    } else {
        g_src.type = SRC_MP3;
        char was[128];
        fmt(was, sizeof(was), "%s", g_meta.cover);
        id3Read(g_src.data, g_src.size);
        fmt(g_meta.cover, sizeof(g_meta.cover), "%s", was);  // coverWrite removes it
        coverWrite(g_src.data);
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
            logLine("%s: no MP3 frames in it", shownPath);
            srcClose();
            return 0;
        }
        // kbit/s is bits a millisecond.
        const uint64_t whole = g_src.streamed ? g_dl.total : g_src.size;
        g_src.durMs = g_src.kbps && whole > audioAt ? (whole - audioAt) * 8u / (uint64_t)g_src.kbps : 0;
    }
    if (g_src.hz < 8000 || g_src.hz > 192000) {
        logLine("%s: a rate of %d Hz", shownPath, g_src.hz);
        srcClose();
        return 0;
    }
    logLine("%s: %u bytes, %d Hz, %d channel(s)", shownPath, (unsigned)got, g_src.hz, g_src.channels);
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

// Background downloads (0.82): their own file, with this one's HTTP and files.
#include "downloads.c"

static int cmpName(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y || !x) return x - y;
    }
}

static void addTrack(const char* path, size_t len) {
    if (g_libCount >= MAX_TRACKS || !len) return;
    // URL<tab>title<tab>artist<tab>album<tab>cover URL (0.72); | between
    // them too (0.80), easier to type than a tab, for a URL that starts the
    // line (a path keeps any | it has).
    const int url = isUrl(path);
#define SEP(c) ((c) == '\t' || (url && (c) == '|'))
    size_t end = 0;
    while (end < len && !SEP(path[end])) end++;
    if (end >= MAX_PATH) return;
    // The URL without spaces after it.
    size_t u = end;
    while (u && path[u - 1] == ' ') u--;
    g_tracks[g_libCount] = poolStr(path, u);
    const char** fields[5] = {&g_given[g_libCount].title, &g_given[g_libCount].artist,
                              &g_given[g_libCount].album, &g_given[g_libCount].cover, &g_given[g_libCount].id};
    size_t at = end;
    for (int f = 0; f < 5; f++) {
        *fields[f] = "";
        if (at >= len) continue;
        at++;  // the tab
        size_t from = at;
        while (at < len && !SEP(path[at])) at++;
        // Spaces around a field, as one may write " | ".
        size_t to = at;
        while (from < to && path[from] == ' ') from++;
        while (to > from && path[to - 1] == ' ') to--;
        *fields[f] = poolStr(path + from, to - from);
    }
#undef SEP
    g_libCount++;
}

// The track playing's path, or "".
static const char* curPath(void) {
    return g_index < g_count ? g_tracks[g_q[g_index]] : "";
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
    for (int i = 1; i < g_libCount; i++) {
        const char* tmp = g_tracks[i];
        int j = i - 1;
        for (; j >= 0 && cmpName(g_tracks[j], tmp) > 0; j--) g_tracks[j + 1] = g_tracks[j];
        g_tracks[j + 1] = tmp;
    }
}

// The queue, from queue.txt or music/. "#start N" in queue.txt picks the
// first track (from 0).
static void writeLibrary(void);
static void plusReset(void);
static int plusLine(const char* line, int len);

static void loadQueue(void) {
    char* text;
    size_t textMapped;
    g_seekMs = 0;
    g_libCount = 0;
    g_count = 0;
    g_index = 0;
    poolReset();
    plusReset();
    const int n = (int)readMapped(SC_BGM_QUEUE, &text, &textMapped);
    int start = 0;
    if (n > 0) {
        for (int at = 0; at < n;) {
            int end = at;
            while (end < n && text[end] != '\n' && text[end] != '\r') end++;
            const char* line = text + at;
            const int len = end - at;
            if (plusLine(line, len)) {
            } else if (len > 10 && !memcmp(line, "#position ", 10)) {
                g_seekMs = 0;
                for (int i = 10; i < len && line[i] >= '0' && line[i] <= '9'; i++)
                    g_seekMs = g_seekMs * 10 + (uint64_t)(line[i] - '0');
            } else if (len > 7 && !memcmp(line, "#start ", 7)) {
                for (int i = 7; i < len && line[i] >= '0' && line[i] <= '9'; i++)
                    start = start * 10 + (line[i] - '0');
            } else if (len > 0 && (line[0] == '/' || isUrl(line))) {
                addTrack(line, (size_t)len);
            }
            at = end + 1;
        }
        logLine("queue.txt: %d track(s)%s", g_libCount, g_libCount >= MAX_TRACKS ? " (the most taken)" : "");
    } else {
        scanMusic();
        logLine("no queue.txt; music/: %d track(s)", g_libCount);
    }
    if (text) sysUnmap(text, textMapped);
    pathIndex();
    // streams.txt's URLs too, read here as well as by the app (0.74): those
    // not already in, after the rest.
    char* streams;
    size_t streamsMapped;
    const int sn = (int)readMapped(SC_BGM_DIR "/streams.txt", &streams, &streamsMapped);
    int added = 0;
    for (int at = 0; at < sn;) {
        int end = at;
        while (end < sn && streams[end] != '\n' && streams[end] != '\r') end++;
        const char* line = streams + at;
        int len = end - at;
        if (len >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) {
            line += 3;
            len -= 3;
        }
        while (len > 0 && (*line == ' ' || *line == '\t')) line++, len--;
        if (len > 8 && isUrl(line)) {
            int urlLen = 0;
            while (urlLen < len && line[urlLen] != '\t' && line[urlLen] != '|' && line[urlLen] != ' ') urlLen++;
            if (pathFind(line, (size_t)urlLen) < 0 && g_libCount < MAX_TRACKS) {
                addTrack(line, (size_t)len);
                pathAdd(g_libCount - 1);
                added++;
            }
        }
        at = end + 1;
    }
    if (streams) sysUnmap(streams, streamsMapped);
    logLine("streams.txt: %d byte(s), %d stream(s) added", sn < 0 ? 0 : sn, added);
    // The queue: the whole library, in order.
    for (int i = 0; i < g_libCount; i++) g_q[i] = i;
    g_count = g_libCount;
    g_queueRev++;
    if (start < g_count) g_index = start;
    writeLibrary();
}

static const char* baseName(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

// ---- the Music section's library (the music watch's ebgm/ answers) ----
//
// The quick menu's Music section, with the music watch (ps4/mcw) answering
// its requests from ebgm/, shows sections of playlists, each a picture and a
// count, and a playlist's tracks. Written here from the queue, each time it
// is read: a "Shell Companion" section with the whole queue, and "Albums",
// one playlist an album, its cover as the picture.
//
// The shapes are the shell's own (ps4/README.md): recommended_views.json,
// {"views": [{"name", "lists": [{"name", "uri", "image_url", "total"}]}]};
// tracks_<id>.json, {"total", "offset", "track_list": [{"name", "artists":
// [{"name"}], "explicit", "is_playable"}]}, the watch choosing the file by
// the playlist's uri. A tile's picture is image_url + "&w=192&h=192" as a
// file's own name, and a JPEG.

#define LIB_DIR SC_BGM_DIR "/ebgm"
#define LIB_URL "file:///user/data/ShellCompanion/ps4/ebgm/"
#define LIB_URI "spotify:user:shellcompanion:playlist:"
#define MAX_ALBUMS 1000  // 0.84: 64 before
#define LIB_TEXT 160

static struct {
    const char *title, *artist, *album;  // in g_pool
    const char* art;  // its cover's file in ebgm/art/ (0.84), or ""
    int albumId;
} g_lib[MAX_TRACKS];
static struct {
    int tracks;
    const char* art;  // its cover's file in ebgm/art/, a JPEG if one of its tracks has one, or ""
    int url;          // else a track whose cover URL is its (0.72), or -1
} g_albums[MAX_ALBUMS];
static int g_nAlbums;

// Albums by name's hash (0.84: looked for one at a time before).
#define ALBUM_SLOTS 4096
static int g_albumSlot[ALBUM_SLOTS];  // an album + 1

static int albumOf(int i) {
    const char* name = g_lib[i].album;
    const size_t n = strlen(name);
    uint32_t k = hashBytes(name, n) & (ALBUM_SLOTS - 1);
    for (;; k = (k + 1) & (ALBUM_SLOTS - 1)) {
        const int a = g_albumSlot[k] - 1;
        if (a < 0) break;
        const char* other = g_lib[g_albums[a].tracks].album;
        if (strlen(other) == n && !memcmp(other, name, n)) return a;
    }
    if (g_nAlbums >= MAX_ALBUMS) return -1;
    const int a = g_nAlbums++;
    g_albums[a].tracks = i;  // its first track, for its name
    g_albums[a].art = "";
    g_albums[a].url = -1;
    g_albumSlot[k] = a + 1;
    return a;
}

// ── The tag cache (0.84) ──────────────────────────────────────────────────────
// A local file's tags read once: tags.txt, a line a file,
// path<tab>size<tab>title<tab>artist<tab>album<tab>cover file (ebgm/art/),
// rewritten with the library's files each time it is written. A file whose
// size changed is read again. 4000 files read each time took minutes.
#define TAG_CACHE SC_BGM_DIR "/tags.txt"
#define CACHE_SLOTS 32768
static struct {
    char* text;
    size_t mapped;
    int n;
    struct {
        const char *path, *title, *artist, *album, *art;
        uint64_t size;
    } e[MAX_TRACKS];
    int slot[CACHE_SLOTS];  // an entry + 1
} g_cache;

static void cacheLoad(void) {
    if (g_cache.text) sysUnmap(g_cache.text, g_cache.mapped);
    g_cache.n = 0;
    memset(g_cache.slot, 0, sizeof(g_cache.slot));
    const int64_t len = readMapped(TAG_CACHE, &g_cache.text, &g_cache.mapped);
    for (int64_t at = 0; at < len && g_cache.n < MAX_TRACKS;) {
        char* line = g_cache.text + at;
        int64_t end = at;
        while (end < len && g_cache.text[end] != '\n') end++;
        g_cache.text[end] = 0;
        char* f[6] = {line, 0, 0, 0, 0, 0};
        int k = 1;
        for (char* p = line; *p && k < 6; p++)
            if (*p == '\t') {
                *p = 0;
                f[k++] = p + 1;
            }
        if (k == 6 && f[0][0] == '/') {
            const int i = g_cache.n++;
            g_cache.e[i].path = f[0];
            g_cache.e[i].size = 0;
            for (const char* q = f[1]; *q >= '0' && *q <= '9'; q++) g_cache.e[i].size = g_cache.e[i].size * 10 + (uint64_t)(*q - '0');
            g_cache.e[i].title = f[2];
            g_cache.e[i].artist = f[3];
            g_cache.e[i].album = f[4];
            g_cache.e[i].art = f[5];
            uint32_t s = hashBytes(f[0], strlen(f[0])) & (CACHE_SLOTS - 1);
            while (g_cache.slot[s]) s = (s + 1) & (CACHE_SLOTS - 1);
            g_cache.slot[s] = i + 1;
        }
        at = end + 1;
    }
}

static int cacheFind(const char* path) {
    const size_t n = strlen(path);
    for (uint32_t s = hashBytes(path, n) & (CACHE_SLOTS - 1);; s = (s + 1) & (CACHE_SLOTS - 1)) {
        const int i = g_cache.slot[s] - 1;
        if (i < 0) return -1;
        if (!memcmp(g_cache.e[i].path, path, n + 1)) return i;
    }
}

// The library's local files' tags, as tags.txt (a tab or a line break in a
// tag made a space).
static struct {
    const char *title, *artist, *album;  // the file's own tags, "" where none
    uint64_t size;
} g_file[MAX_TRACKS];

static void cacheSave(void) {
    const size_t cap = 4096 + (size_t)g_libCount * 1200;
    char* out = (char*)sysMap(cap);
    if (!out) return;
    size_t at = 0;
    for (int i = 0; i < g_libCount && at + 1200 < cap; i++) {
        if (isUrl(g_tracks[i])) continue;
        const char* f[5] = {g_file[i].title, g_file[i].artist, g_file[i].album, g_lib[i].art, 0};
        at += (size_t)fmt(out + at, cap - at, "%s\t%lu", g_tracks[i], (unsigned long)g_file[i].size);
        for (int k = 0; k < 4; k++) {
            out[at++] = '\t';
            for (const char* q = f[k]; *q && at + 2 < cap; q++) out[at++] = (*q == '\t' || *q == '\n') ? ' ' : *q;
        }
        out[at++] = '\n';
    }
    out[at] = 0;
    writeText(TAG_CACHE, out);
    sysUnmap(out, cap);
}

static int libSame(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

// s into out at *at, as a JSON string's inside.
static void jsonPut(char* out, size_t cap, size_t* at, const char* s) {
    for (; *s && *at + 8 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[(*at)++] = '\\';
            out[(*at)++] = (char)c;
        } else if (c < 0x20) {
            *at += (size_t)fmt(out + *at, cap - *at, "\\u%04x", c);
        } else {
            out[(*at)++] = (char)c;
        }
    }
    out[*at] = 0;
}

static void libAdd(char* out, size_t cap, size_t* at, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    const int n = fmtv(out + *at, cap - *at, f, ap);
    va_end(ap);
    *at += (size_t)n;
    if (*at >= cap) *at = cap - 1;
}

// The covers' names change with each library written (0.71): the shell
// keeps a picture by its address, and showed trackart0.jpg's old picture
// for the new library's first track. album<a>-<rev>.jpg, trackart<n>-<rev>.jpg;
// the rev in status.txt (art_rev), for the music watch.
static int g_artRev;

static int hasText(const char* s, const char* part) {
    const size_t n = strlen(part);
    for (; *s; s++)
        if (!memcmp(s, part, n)) return 1;
    return 0;
}

// A cover's kind from its first bytes: 1 JPEG, 2 PNG, 0 other (0.71: a
// PNG is written too, for the rows and tiles, which showed JPEGs only).
static int artKind(const uint8_t* p, uint64_t n) {
    if (!p || n < 8) return 0;
    if (p[0] == 0xFF && p[1] == 0xD8) return 1;
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return 2;
    return 0;
}
static const char* artExt(int kind) {
    return kind == 2 ? "png" : "jpg";
}

// The covers of other revs, deleted.
static void artClean(void) {
    const int fd = sysOpen(LIB_DIR, SC_O_RDONLY | SC_O_DIRECTORY, 0);
    if (fd < 0) return;
    static uint8_t buf[0x2000];
    char keep[16];
    fmt(keep, sizeof(keep), "-%d.txt", g_artRev);
    int n;
    while ((n = sysGetdents(fd, buf, sizeof(buf))) > 0) {
        for (int at = 0; at < n;) {
            const uint16_t reclen = le16(buf + at + 4);
            const uint8_t namlen = buf[at + 7];
            const char* name = (const char*)buf + at + 8;
            if (!reclen) break;
            char nm[128];
            if (namlen < sizeof(nm)) {
                memcpy(nm, name, namlen);
                nm[namlen] = 0;
                // 0.71-0.83's covers a track and an album, all gone (art/
                // since 0.84); rowart-<rev>.txt of other libraries.
                const int old = !memcmp(nm, "trackart", 8) ||
                                (!memcmp(nm, "album", 5) && (hasText(nm, ".jpg") || hasText(nm, ".png")));
                const int row = !memcmp(nm, "rowart-", 7) && !hasText(nm, keep);
                if (old || row) {
                    char path[200];
                    fmt(path, sizeof(path), LIB_DIR "/%s", nm);
                    sysUnlink(path);
                }
            }
            at += reclen;
        }
    }
    sysClose(fd);
}


// A title from a file's name without a YouTube video's id, as download
// tools add it (0.76): "Celebration-QZWRu3kqkG8", "Celebration [QZWRu3kqkG8]".
static int idChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}
static void titleTidy(char* t) {
    size_t n = strlen(t);
    if (n > 14 && t[n - 1] == ']' && t[n - 13] == '[') {
        int ok = 1;
        for (size_t k = n - 12; k < n - 1; k++) ok &= idChar(t[k]);
        if (ok) {
            n -= 13;
            while (n && t[n - 1] == ' ') n--;
            t[n] = 0;
            return;
        }
    }
    if (n > 12 && t[n - 12] == '-') {
        int ok = 1;
        for (size_t k = n - 11; k < n; k++) ok &= idChar(t[k]);
        if (ok) {
            n -= 12;
            while (n && t[n - 1] == ' ') n--;
            t[n] = 0;
        }
    }
}

// A cover's file in ebgm/art/, named by its bytes' hash (0.84): written
// once however many tracks carry it (0.83 and before wrote one a track, 14
// copies of one 482 KB PNG), and never the same name for another picture, so
// the shell's picture cache cannot show an old one. Its copy named
// "<name>&w=192&h=192" is for a tile (the shell adds that to image_url).
#define ART_DIR LIB_DIR "/art"
static const char* artSave(const uint8_t* img, uint64_t n, int kind) {
    uint32_t h1 = 2166136261u, h2 = 0x9E3779B9u;
    for (uint64_t k = 0; k < n; k++) {
        h1 = (h1 ^ img[k]) * 16777619u;
        h2 = (h2 ^ img[k]) * 0x01000193u + 0x7F4A7C15u;
    }
    char name[64], path[160];
    fmt(name, sizeof(name), "%08x%08x-%lu.%s", h1, h2, (unsigned long)n, artExt(kind));
    for (int tile = 0; tile < 2; tile++) {
        fmt(path, sizeof(path), ART_DIR "/%s%s", name, tile ? "&w=192&h=192" : "");
        if (sysExists(path)) continue;
        char tmp[176];
        fmt(tmp, sizeof(tmp), "%s.new", path);
        const int fd = sysOpen(tmp, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
        if (fd < 0) continue;
        const int w = sysWrite(fd, img, (size_t)n);
        sysClose(fd);
        if (w == (int)n) sysRename(tmp, path);
        else sysUnlink(tmp);
    }
    return poolCopy(name);
}

// The file's title from its name, without its type or a video id.
static const char* titleFromName(const char* path) {
    char t[LIB_TEXT];
    fmt(t, sizeof(t), "%s", baseName(path));
    for (char* q = t + strlen(t); q > t; q--)
        if (*q == '.') {
            *q = 0;
            break;
        }
    titleTidy(t);
    return poolCopy(t);
}

// A local file's own tags and cover: from the cache if its size is the one
// cached and its cover file is there, else read (its ID3 tag only).
static int g_cacheHits, g_cacheReads;
static void fileTags(int i) {
    static uint8_t head[10];
    const char* path = g_tracks[i];
    g_file[i].title = g_file[i].artist = g_file[i].album = "";
    g_lib[i].art = "";
    const int fd = sysOpen(path, SC_O_RDONLY, 0);
    if (fd < 0) return;
    const int64_t fsize = sysSeek(fd, 0, 2);
    g_file[i].size = fsize > 0 ? (uint64_t)fsize : 0;
    const int c = cacheFind(path);
    if (c >= 0 && g_cache.e[c].size == g_file[i].size) {
        char artPath[160];
        fmt(artPath, sizeof(artPath), ART_DIR "/%s", g_cache.e[c].art);
        if (!g_cache.e[c].art[0] || sysExists(artPath)) {
            g_file[i].title = poolCopy(g_cache.e[c].title);
            g_file[i].artist = poolCopy(g_cache.e[c].artist);
            g_file[i].album = poolCopy(g_cache.e[c].album);
            g_lib[i].art = poolCopy(g_cache.e[c].art);
            sysClose(fd);
            g_cacheHits++;
            return;
        }
    }
    g_cacheReads++;
    sysSeek(fd, 0, 0);
    size_t size = 0;
    if (sysRead(fd, head, 10) == 10 && !memcmp(head, "ID3", 3)) size = 10 + syncsafe(head + 6);
    if (size > (16u << 20)) size = 16u << 20;
    uint8_t* tag = size ? (uint8_t*)sysMap((size + 0x3FFF) & ~(size_t)0x3FFF) : NULL;
    size_t got = 0;
    if (tag) {
        memcpy(tag, head, 10);
        got = 10;
        while (got < size) {
            const int n = sysRead(fd, tag + got, size - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
    }
    sysClose(fd);
    if (!tag) return;
    // id3Read fills g_meta, the playing track's: kept and put back.
    static char keepCover[128];
    static char keep[3][META_LEN];
    const uint64_t keepOff = g_meta.artOff, keepLen = g_meta.artLen;
    memcpy(keep[0], g_meta.title, META_LEN);
    memcpy(keep[1], g_meta.artist, META_LEN);
    memcpy(keep[2], g_meta.album, META_LEN);
    memcpy(keepCover, g_meta.cover, sizeof(keepCover));
    id3Read(tag, got);
    g_file[i].title = poolCopy(g_meta.title);
    g_file[i].artist = poolCopy(g_meta.artist);
    g_file[i].album = poolCopy(g_meta.album);
    const uint8_t* img = tag + g_meta.artOff;
    const int kind = g_meta.artLen > 4 && g_meta.artOff + g_meta.artLen <= got ? artKind(img, g_meta.artLen) : 0;
    if (kind) g_lib[i].art = artSave(img, g_meta.artLen, kind);
    sysUnmap(tag, (size + 0x3FFF) & ~(size_t)0x3FFF);
    memcpy(g_meta.title, keep[0], META_LEN);
    memcpy(g_meta.artist, keep[1], META_LEN);
    memcpy(g_meta.album, keep[2], META_LEN);
    memcpy(g_meta.cover, keepCover, sizeof(keepCover));
    g_meta.artOff = keepOff;
    g_meta.artLen = keepLen;
}

static int g_logged;  // library lines logged, the first 40 only (0.84)

// A track's title, artist, album and cover: an app's (queue.txt) over the
// file's own; its album found or made.
static void libRead(int i) {
    const char* path = g_tracks[i];
    const int url = isUrl(path);
    g_file[i].title = g_file[i].artist = g_file[i].album = "";
    g_file[i].size = 0;
    g_lib[i].art = "";
    if (!url) fileTags(i);
    const char* title = g_given[i].title[0] ? g_given[i].title : g_file[i].title;
    if (!title[0]) {
        if (url) {
            const char* name = path;
            for (const char* q = path; *q && *q != '?'; q++)
                if (*q == '/') name = q + 1;
            size_t k = 0;
            while (name[k] && name[k] != '?') k++;
            title = poolStr(name, k);
        } else {
            title = titleFromName(path);
        }
    }
    g_lib[i].title = title;
    g_lib[i].artist = g_given[i].artist[0] ? g_given[i].artist
                      : g_file[i].artist[0] ? g_file[i].artist : "Unknown artist";
    g_lib[i].album = g_given[i].album[0] ? g_given[i].album
                     : g_file[i].album[0] ? g_file[i].album : url ? "Streamed" : "Unknown album";
    const int a = albumOf(i);
    g_lib[i].albumId = a;
    if (a >= 0) {
        // The album's cover: a JPEG if one of its tracks has one (the tiles
        // showed JPEGs only in 0.58), else a PNG, else a cover URL.
        const char* art = g_lib[i].art;
        if (art[0] && (!g_albums[a].art[0] || (hasText(g_albums[a].art, ".png") && hasText(art, ".jpg"))))
            g_albums[a].art = art;
        if (!art[0] && g_given[i].cover[0] && g_albums[a].url < 0) g_albums[a].url = i;
    }
    if (g_logged < 40) {
        g_logged++;
        logLine("library %d: \"%s\" by \"%s\", album \"%s\"; cover: %s", i, g_lib[i].title, g_lib[i].artist,
                g_lib[i].album, g_lib[i].art[0] ? g_lib[i].art : g_given[i].cover[0] ? "a URL" : "none");
    }
}

// One playlist's tracks, the library's numbers list[0..n), as
// tracks_<id>.json.
static void libTracks(char* out, size_t cap, const char* id, const int* list, int n) {
    size_t at = 0;
    libAdd(out, cap, &at, "{\"total\":%d,\"offset\":0,\"limit\":%d,\"track_list\":[", n, n);
    int first = 1;
    for (int k = 0; k < n; k++) {
        const int i = list[k];
        libAdd(out, cap, &at, "%s{\"name\":\"", first ? "" : ",");
        jsonPut(out, cap, &at, g_lib[i].title);
        libAdd(out, cap, &at, "\",\"artists\":[{\"name\":\"");
        jsonPut(out, cap, &at, g_lib[i].artist);
        libAdd(out, cap, &at, "\"}],\"album\":{\"name\":\"");
        jsonPut(out, cap, &at, g_lib[i].album);
        // "id": the shell's TrackListMenuData.IsSameData compares it (0.79):
        // with none, a row reused for another track as the list scrolled was
        // taken for the same and kept its old title.
        libAdd(out, cap, &at, "\"},\"id\":\"sc%d\",\"uri\":\"spotify:track:sc%d\",\"explicit\":false,"
                              "\"is_playable\":true}", i, i);
        first = 0;
    }
    libAdd(out, cap, &at, "]}\n");
    char path[96];
    fmt(path, sizeof(path), LIB_DIR "/tracks_%s.json", id);
    writeText(path, out);
}

// The files' text, made here: mapped for the library's size (0.84: 512 KB
// before, which All songs of 4000 tracks would have passed).
static char* g_libOut;
static size_t g_libOutCap;
#define LIB_CAP g_libOutCap

// The liked tracks (liked.txt, a path a line, as the music watch keeps it for
// R3 "+"): their library numbers, into list; how many.
static int likedList(int* list) {
    char* text;
    size_t mapped;
    const int64_t len = readMapped(SC_BGM_DIR "/liked.txt", &text, &mapped);
    int n = 0;
    static uint8_t seen[MAX_TRACKS];
    memset(seen, 0, sizeof(seen));
    for (int64_t at = 0; at < len;) {
        int64_t end = at;
        while (end < len && text[end] != '\n' && text[end] != '\r') end++;
        const int t = end > at ? pathFind(text + at, (size_t)(end - at)) : -1;
        if (t >= 0 && !seen[t]) {
            seen[t] = 1;
            list[n++] = t;
        }
        at = end + 1;
    }
    if (text) sysUnmap(text, mapped);
    return n;
}

// An album's tracks, in the library's order; how many.
static int albumList(int a, int* list) {
    int n = 0;
    for (int i = 0; i < g_libCount; i++)
        if (g_lib[i].albumId == a) list[n++] = i;
    return n;
}

static int g_list[MAX_TRACKS];  // a list's numbers, made here (the player's thread only)

// A list's picture: the album cover of its first track that has one.
static int listArt(const int* list, int n) {
    for (int k = 0; k < n; k++) {
        const int a = g_lib[list[k]].albumId;
        if (a >= 0 && a < g_nAlbums && (g_albums[a].art[0] || g_albums[a].url >= 0)) return a;
    }
    return -1;
}

static void viewList(size_t* at, const char* name, const char* id, int art, int total, int first) {
    char* out = g_libOut;
    libAdd(out, LIB_CAP, at, "%s{\"name\":\"", first ? "" : ",");
    jsonPut(out, LIB_CAP, at, name);
    libAdd(out, LIB_CAP, at, "\",\"uri\":\"" LIB_URI "%s\",\"type\":\"playlist\",\"image_url\":\"", id);
    if (art >= 0 && g_albums[art].art[0]) libAdd(out, LIB_CAP, at, LIB_URL "art/%s", g_albums[art].art);
    else if (art >= 0 && g_albums[art].url >= 0) {  // a cover by its URL, the shell fetching it (0.72)
        const char* url = g_given[g_albums[art].url].cover;
        jsonPut(out, LIB_CAP, at, url);
        // The shell adds "&w=192&h=192": a URL with no query is given one.
        if (!hasText(url, "?")) libAdd(out, LIB_CAP, at, "?");
    }
    libAdd(out, LIB_CAP, at, "\",\"total\":%d}", total);
}

// The Music section's front page, recommended_views.json: Shell Companion
// (Queue, All songs, Liked), then Albums.
static void writeViews(void) {
    if (!g_libOut) return;
    size_t at = 0;
    libAdd(g_libOut, LIB_CAP, &at, "{\"upgrade_scopes_required\":false,\"accounts_linked\":true,"
                                   "\"views\":[{\"name\":\"Shell Companion\",\"lists\":[");
    viewList(&at, "Queue", "queue", listArt(g_q, g_count), g_count, 1);
    for (int i = 0; i < g_libCount; i++) g_list[i] = i;
    viewList(&at, "All songs", "all", listArt(g_list, g_libCount), g_libCount, 0);
    const int liked = likedList(g_list);
    viewList(&at, "Liked", "liked", listArt(g_list, liked), liked, 0);
    libAdd(g_libOut, LIB_CAP, &at, "]}");
    if (g_nAlbums) {
        libAdd(g_libOut, LIB_CAP, &at, ",{\"name\":\"Albums\",\"lists\":[");
        for (int a = 0; a < g_nAlbums; a++) {
            char id[16];
            fmt(id, sizeof(id), "album%d", a);
            viewList(&at, g_lib[g_albums[a].tracks].album, id,
                     g_albums[a].art[0] || g_albums[a].url >= 0 ? a : -1,
                     albumList(a, g_list), a == 0);
        }
        libAdd(g_libOut, LIB_CAP, &at, "]}");
    }
    libAdd(g_libOut, LIB_CAP, &at, "]}\n");
    writeText(LIB_DIR "/recommended_views.json", g_libOut);
}

// The queue changed: its list and the front page (its total) again, and
// queue_rev counted on, for the music watch, which holds the Music
// section's asking for the queue until it sees it (0.67).
static void writeQueue(void) {
    if (!g_libOut) return;
    libTracks(g_libOut, LIB_CAP, "queue", g_q, g_count);
    writeViews();
    g_queueRev++;
}

static void writeLiked(void) {
    if (!g_libOut) return;
    libTracks(g_libOut, LIB_CAP, "liked", g_list, likedList(g_list));
    writeViews();
}

// Each track's cover for its row, a line a track (its number the line's):
// a file:// URL of its cover in ebgm/art/, its cover's URL, or empty. The
// music watch reads it whole once a library (0.84: a file a track before).
static void writeRowArt(void) {
    const size_t cap = 4096 + (size_t)g_libCount * 160 + 1;
    size_t extra = 0;
    for (int i = 0; i < g_libCount; i++) extra += strlen(g_given[i].cover);
    char* out = (char*)sysMap(cap + extra);
    if (!out) return;
    size_t at = 0;
    for (int i = 0; i < g_libCount; i++) {
        if (g_lib[i].art[0]) at += (size_t)fmt(out + at, cap + extra - at, LIB_URL "art/%s", g_lib[i].art);
        else at += (size_t)fmt(out + at, cap + extra - at, "%s", g_given[i].cover);
        out[at++] = '\n';
    }
    out[at] = 0;
    char path[96];
    fmt(path, sizeof(path), LIB_DIR "/rowart-%d.txt", g_artRev);
    writeText(path, out);
    sysUnmap(out, cap + extra);
}

static void writeLibrary(void) {
    sysMkdir(LIB_DIR, 0777);
    sysMkdir(ART_DIR, 0777);
    g_nAlbums = 0;
    memset(g_albumSlot, 0, sizeof(g_albumSlot));
    g_artRev = (int)((sysNowUs() / 1000u) % 1000000u);
    const uint64_t t0 = sysNowUs();
    cacheLoad();
    g_cacheHits = g_cacheReads = g_logged = 0;
    for (int i = 0; i < g_libCount; i++) libRead(i);
    cacheSave();
    writeRowArt();
    artClean();
    logLine("library: tags of %d file(s) from the cache, %d read, in %lu ms", g_cacheHits, g_cacheReads,
            (unsigned long)((sysNowUs() - t0) / 1000u));
    const size_t want = ((size_t)(1u << 20) + (size_t)g_libCount * 900 + 0x3FFF) & ~(size_t)0x3FFF;
    if (g_libOut && g_libOutCap < want) {
        sysUnmap(g_libOut, g_libOutCap);
        g_libOut = NULL;
    }
    if (!g_libOut) {
        g_libOut = (char*)sysMap(want);
        g_libOutCap = g_libOut ? want : 0;
    }
    if (!g_libOut) return;
    for (int i = 0; i < g_libCount; i++) g_list[i] = i;
    libTracks(g_libOut, LIB_CAP, "all", g_list, g_libCount);
    for (int a = 0; a < g_nAlbums; a++) {
        char id[16];
        fmt(id, sizeof(id), "album%d", a);
        libTracks(g_libOut, LIB_CAP, id, g_list, albumList(a, g_list));
    }
    writeLiked();
    writeQueue();
    logLine("library: %d track(s), %d album(s), written to ebgm/", g_libCount, g_nAlbums);
}

static const char* libTitle(int i) {
    return i >= 0 && i < g_libCount && g_lib[i].title[0] ? g_lib[i].title : "a track";
}

// A track chosen in the Music section: a list's id (queue, all, liked,
// album<n>) and the track's place in it, to its place in the queue; -1 if
// none. A list other than the queue becomes the queue (0.67), as a player
// plays an album from the track chosen.
static void queueInsert(int at, int lib);

// lib, the row's own track if known (0.77): the list the shell shows may be
// older than ours (the queue, after a list played became it), so where the
// row's place holds another track, the track is looked for.
static int listFind(const int* list, int count, int n, int lib) {
    if (lib < 0 || lib >= g_libCount) return n;
    if (n >= 0 && n < count && list[n] == lib) return n;
    int best = -1;
    for (int k = 0; k < count; k++)
        if (list[k] == lib && (best < 0 || (k > n ? k - n : n - k) < (best > n ? best - n : n - best))) best = k;
    if (best >= 0) logLine("pick: place %d holds track %d, not %d: track %d is at %d", n,
                           n >= 0 && n < count ? list[n] : -1, lib, lib, best);
    return best;  // -1: not in it
}

static int libPick(const char* id, int n, int lib) {
    if (libSame(id, "queue")) {
        n = listFind(g_q, g_count, n, lib);
        if (n >= 0 && n < g_count) return n;
        if (lib < 0 || lib >= g_libCount || g_count >= MAX_TRACKS) return -1;
        // Not in the queue now: put after the one playing, and played.
        queueInsert(g_count ? g_index + 1 : 0, lib);
        return g_count > 1 ? g_index + 1 : 0;
    }
    int count;
    if (libSame(id, "all")) {
        for (int i = 0; i < g_libCount; i++) g_list[i] = i;
        count = g_libCount;
    } else if (libSame(id, "liked")) {
        count = likedList(g_list);
    } else if (!memcmp(id, "album", 5)) {
        int a = 0;
        for (const char* p = id + 5; *p >= '0' && *p <= '9'; p++) a = a * 10 + (*p - '0');
        count = albumList(a, g_list);
    } else {
        return -1;
    }
    const int found = listFind(g_list, count, n, lib);
    if (found >= 0) n = found;
    if (n < 0 || n >= count) return -1;
    memcpy(g_q, g_list, sizeof(int) * (size_t)count);
    g_count = count;
    g_played = 0;
    writeQueue();
    logLine("queue: now %s, %d track(s)", id, count);
    return n;
}

// The queue edited from the Music section (0.67, through the music watch).
static void queueMove(int from, int to) {
    if (from < 0 || from >= g_count || from == to) return;
    if (to < 0) to = 0;
    if (to >= g_count) to = g_count - 1;
    const int t = g_q[from];
    if (from < to)
        for (int k = from; k < to; k++) g_q[k] = g_q[k + 1];
    else
        for (int k = from; k > to; k--) g_q[k] = g_q[k - 1];
    g_q[to] = t;
    if (g_index == from) g_index = to;
    else if (from < g_index && to >= g_index) g_index--;
    else if (from > g_index && to <= g_index) g_index++;
    logLine("queue: %s moved from %d to %d", libTitle(t), from, to);
    writeQueue();
}

static void queueInsert(int at, int lib) {
    if (lib < 0 || lib >= g_libCount || g_count >= MAX_TRACKS) return;
    if (at > g_count) at = g_count;
    for (int k = g_count; k > at; k--) g_q[k] = g_q[k - 1];
    g_q[at] = lib;
    g_count++;
    if (at <= g_index && g_count > 1) g_index++;
    writeQueue();
}

// The Music section's "+" as each app wants it (0.86).
#include "plus.c"

// Packages through the PS4's own download service, and self-update (0.89).
#include "pkg.c"

static uint64_t g_lastStatus;

static void writeStatus(void) {
    static char text[4096];
    const uint64_t posMs = g_src.hz ? g_src.framesRead * 1000u / (uint64_t)g_src.hz : g_heldPosMs;
    fmt(text, sizeof(text),
        "protocol=%d\nstate=%s\nindex=%d\ncount=%d\npos_ms=%lu\ndur_ms=%lu\nfile=%s\nbeat_ms=%lu\n"
        "title=%s\nartist=%s\nalbum=%s\nart_off=%lu\nart_len=%lu\nvolume=%d\nrepeat=%d\nshuffle=%d\ncover=%s\n"
        "queue_rev=%d\ntrack=%d\nart_rev=%d\n",
        SC_BGM_PROTOCOL, stateName(g_state), g_index, g_count, (uint64_t)posMs, g_src.durMs,
        curPath(), (uint64_t)(sysNowUs() / 1000u), g_meta.title,
        g_meta.artist, g_meta.album, g_meta.artOff, g_meta.artLen, (int)g_volume, g_repeat, g_shuffle,
        g_index < g_count && g_given[g_q[g_index]].cover[0] ? g_given[g_q[g_index]].cover : g_meta.cover,
        g_queueRev, g_index < g_count ? g_q[g_index] : -1, g_artRev);
    writeText(SC_BGM_STATUS, text);
    g_lastStatus = sysNowUs();
}

// ---- the player ----

static int g_port = -1;
static int g_portType;

static void applyVolume(void) {
    if (!g_aoVolume || g_port < 0) return;
    int32_t vol[8];
    for (int i = 0; i < 8; i++) vol[i] = g_volume;
    g_aoVolume(g_port, 0xFF, vol);
}
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
    applyVolume();
    return 1;
}

// Opens the track at g_index, or the next that opens: 1, or 0 when none
// is left.
static int openTrack(void) {
    for (; g_index < g_count; g_index++) {
        if (srcOpen(curPath())) {
            g_heldPosMs = 0;
            if (g_seekMs && g_src.hz) {
                // Into it as far as queue.txt's #position says: decoded and
                // passed over (0.81).
                const uint64_t want = g_seekMs * (uint64_t)g_src.hz / 1000u;
                int16_t skip[2];
                while (g_src.framesRead < want && srcFrame(skip)) {
                }
                logLine("began %lu ms into it", (unsigned long)(g_src.framesRead * 1000u / (uint64_t)g_src.hz));
            }
            g_seekMs = 0;
            rsStart();
            const int lib = g_q[g_index];
            if (isUrl(curPath())) {
                // Its tags from the queue where the stream has none (0.72).
                if (!g_meta.title[0]) fmt(g_meta.title, META_LEN, "%s", g_lib[lib].title);
                if (!g_meta.artist[0]) fmt(g_meta.artist, META_LEN, "%s", g_lib[lib].artist);
                if (!g_meta.album[0]) fmt(g_meta.album, META_LEN, "%s", g_lib[lib].album);
            }
            notify("Now playing: %s", g_lib[lib].title[0] ? g_lib[lib].title : baseName(curPath()));
            plusTrackBegun(lib);
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
        if (IS("next")) g_index = g_shuffle ? otherTrack() : g_index + 1 < g_count ? g_index + 1 : 0;
        else if (posMs < 3000) g_index = g_index > 0 ? g_index - 1 : g_count - 1;
        // (prev after 3 s starts the track again, as players do)
        if (g_state == ST_STOPPED) g_state = ST_PAUSED;
        if (!openTrack()) stop("nothing after it would open");
    } else if (IS("stop")) {
        stop("asked");
    } else if (n > 10 && (!memcmp(text, "plus_save=", 10) || !memcmp(text, "plus_delete=", 12))) {
        // "+" pressed in the Music section (the music watch, 0.86): plus.c.
        const int save = text[5] == 's';
        int lib = 0;
        for (int i = save ? 10 : 12; i < n && text[i] >= '0' && text[i] <= '9'; i++) lib = lib * 10 + (text[i] - '0');
        plusPressed(save, lib);
    } else if (IS("bgft_probe")) {
        // An experiment (0.90): media through the PS4's download service.
        probeStart();
    } else if (IS("update_start")) {
        // An app's own update, once it has gone (the client library's
        // scUpdateInstallPkg, 0.89): pkg.c.
        updateStart();
    } else if (IS("dl_start")) {
        // An app's downloads handed over (the client library's
        // scDownloadsStart, 0.82): downloads.c's thread takes them.
        g_dls.takeBack = 0;
        g_dls.start = 1;
    } else if (IS("dl_takeback")) {
        // Back to the app: stopped after the piece being written,
        // running=0 in downloads-status.txt once it has.
        g_dls.takeBack = 1;
        if (!g_dls.running) dlStatus();
    } else if (IS("takeback")) {
        // An app takes the music back (the client library's
        // scMusicTakeBack, 0.81): where it was kept for status.txt, so the
        // app goes on from there.
        g_heldPosMs = g_src.hz ? g_src.framesRead * 1000u / (uint64_t)g_src.hz : g_heldPosMs;
        stop("taken back by the app");
    } else if (IS("reload")) {
        // The queue again, from its start; playing if it was. The port
        // too, as port.txt may have changed.
        const int was = g_state;
        stop("reloading");
        loadQueue();
        if (was != ST_STOPPED && openTrack()) g_state = ST_PLAYING;
    } else if (n > 6 && !memcmp(text, "index=", 6)) {
        // A track of the queue by its number, from 0, playing: the Music
        // section's choice, through the music watch (PlaySpUri's index).
        int v = 0;
        for (int i = 6; i < n && text[i] >= '0' && text[i] <= '9'; i++) v = v * 10 + (text[i] - '0');
        if (!g_count) loadQueue();
        if (g_count) {
            g_index = v < g_count ? v : 0;
            if (openTrack()) g_state = ST_PLAYING;
            else stop("that track would not open");
        }
    } else if (n > 5 && !memcmp(text, "pick=", 5)) {
        // pick=<playlist id>:<n>: a track chosen in the Music section,
        // through the music watch (PlaySpUri's playlist and index).
        char id[32];
        int k = 0, i = 5;
        while (i < n && text[i] != ':' && k < 31) id[k++] = text[i++];
        id[k] = 0;
        int v = 0, lib = -1;
        for (i++; i < n && text[i] >= '0' && text[i] <= '9'; i++) v = v * 10 + (text[i] - '0');
        // :<track>: the row's own track (0.77), the library's number.
        if (i < n && text[i] == ':')
            for (lib = 0, i++; i < n && text[i] >= '0' && text[i] <= '9'; i++) lib = lib * 10 + (text[i] - '0');
        if (!g_count) loadQueue();
        const int at = libPick(id, v, lib);
        logLine("pick %s:%d (track %d): place %d in the queue, track %d", id, v, lib, at,
                at >= 0 && at < g_count ? g_q[at] : -1);
        if (at >= 0) {
            g_index = at;
            if (openTrack()) g_state = ST_PLAYING;
            else stop("that track would not open");
        }
    } else if (n >= 5 && (!memcmp(text, "move=", 5) || !memcmp(text, "remove=", 7) ||
                         !memcmp(text, "next=", 5) || !memcmp(text, "add=", 4))) {
        // The queue edited in the Music section (0.67): move=<from>:<to>
        // (places in the queue), remove=<place>, next=<track> and
        // add=<track> (the library's numbers: put after the one playing, or
        // last).
        int v[2] = {0, 0}, k = 0;
        for (int i = 0; i < n && k < 2; i++) {
            if (text[i] == '=' || text[i] == ':') {
                for (i++; i < n && text[i] >= '0' && text[i] <= '9'; i++) v[k] = v[k] * 10 + (text[i] - '0');
                k++;
                i--;
            }
        }
        if (!g_count && g_libCount == 0) loadQueue();
        if (text[0] == 'm' && text[1] == 'o') {
            queueMove(v[0], v[1]);
        } else if (text[0] == 'r') {
            if (v[0] >= 0 && v[0] < g_count) {
                const int t = g_q[v[0]];
                for (int i = v[0]; i + 1 < g_count; i++) g_q[i] = g_q[i + 1];
                g_count--;
                notify("Removed from the queue: %s", libTitle(t));
                if (v[0] < g_index) {
                    g_index--;
                } else if (v[0] == g_index) {
                    // The one playing: the next plays, or the queue has ended.
                    const int was = g_state;
                    srcClose();
                    if (g_index >= g_count) g_index = 0;
                    if (!g_count || (was != ST_STOPPED && !openTrack())) stop("its track was removed");
                    else if (was == ST_PAUSED && g_state != ST_STOPPED) g_state = ST_PAUSED;
                    if (was == ST_STOPPED) g_state = ST_STOPPED;
                }
                writeQueue();
            }
        } else if (text[0] == 'n') {
            queueInsert(g_count ? g_index + 1 : 0, v[0]);
            notify("Plays next: %s", libTitle(v[0]));
        } else {
            queueInsert(g_count, v[0]);
            notify("Added to the queue: %s", libTitle(v[0]));
        }
    } else if (IS("liked")) {
        writeLiked();  // liked.txt changed (R3 "+"): the Liked list again
    } else if (n > 7 && !memcmp(text, "repeat=", 7)) {
        g_repeat = text[7] == '1' ? 1 : text[7] == '2' ? 2 : 0;
    } else if (n > 8 && !memcmp(text, "shuffle=", 8)) {
        g_shuffle = text[8] == '1';
        g_played = 0;
    } else if (n > 7 && !memcmp(text, "volume=", 7)) {
        int32_t v = 0;
        for (int i = 7; i < n && text[i] >= '0' && text[i] <= '9'; i++) v = v * 10 + (text[i] - '0');
        g_volume = v > 32768 ? 32768 : v;
        applyVolume();
    } else if (IS("quit")) {
        stop("asked to quit");
        g_quit = 1;
    }
#undef IS
}

// The commands in cmd, one a line, if it is there. It is renamed first, so
// that a command written meanwhile is not lost, nor one read half written.
static void command(void) {
    static char text[4096];  // 0.86: 256, and commands are added to cmd since 0.67
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
        // A stream: more of it, or silence while it comes (0.72).
        if (g_src.streamed) {
            srcGrow();
            const int starving = !g_dl.done && g_src.size < g_src.off + 64 * 1024;
            if (starving != g_src.buffering) {
                g_src.buffering = starving;
                logLine("stream: %s at %lu of %lu bytes", starving ? "buffering" : "playing again",
                        (uint64_t)g_src.off, (uint64_t)g_src.size);
                writeStatus();
            }
            if (starving) {
                memset(g_out, 0, sizeof(g_out));
                g_aoOutput(g_port, g_out);
                continue;
            }
        }
        const int frames = rsFill(g_out);
        if (frames > 0) g_aoOutput(g_port, g_out);
        if (frames < GRAIN) {
            // The track ended: again (repeat one), another (shuffle), the
            // next; past the end, the first (repeat all) or the end.
            g_played++;
            int ended = 0;
            if (g_repeat == 2) {
            } else if (g_shuffle) {
                if (g_played >= g_count && !g_repeat) ended = 1;
                else g_index = otherTrack();
            } else if (++g_index >= g_count) {
                if (g_repeat) g_index = 0;
                else ended = 1;
            }
            if (ended || !openTrack()) {
                g_index = 0;
                g_played = 0;
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
    if (threadStart(dlMain, NULL, "ScBgmDl") < 0) logLine("downloads: no thread");
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
