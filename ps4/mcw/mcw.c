/**
 * The music watch: a module for SceShellUI, loaded by the test app (UP), that
 * watches the shell's calls to the system's music server and changes none.
 *
 * The quick menu's Music section is the shell's, and talks to the music
 * server (SceMusicCoreServer, NPXS21007) through libSceMusicCoreServerClient
 * and libSceMusicCoreServerClientJsEx: sceMusicCoreServerPlayControl,
 * GetPlayStatus, GetTrackInfo and the rest (their names from shadPS4's
 * aerolib). Nothing says what they take or give back. So each is hooked
 * (hook.c): every call is passed on unchanged, and logged with its
 * arguments, the memory each pointer argument points to before and after
 * the call, what it returned and how long it took. Play something with the
 * USB Music Player, press pause and next in the quick menu, and the log
 * shows what the quick menu sends and what it is given.
 *
 * A getter (Get..., Check...) the quick menu may ask many times a second is
 * logged only when its answer changes, with how many calls gave the same
 * one since. A function given a callback (RegistEventCallback) has the
 * callback hooked too, so the events the server sends the shell show.
 *
 * Files, in /user/data/ShellCompanion/ps4 (/data/ShellCompanion/ps4 by FTP):
 *   mcw.log        the log (mcw.1.log the one before, past 8 MB)
 *   mcw.off        if there, nothing is hooked (for a PS4 that misbehaves)
 *   mcw-names.txt  if there, the functions to hook instead, one a line
 *   mcw.quiet      while there, nothing is logged (the hooks stay)
 *   mcw-fake.txt   while our player (sc_bgm) has a track, the quick menu is
 *                  answered for it (fakeApply) unless this says off; its
 *                  first word is the title: CUSA01780 (Spotify's, so that the
 *                  Music section shows us too) if none; SCMP00001 the test app
 *   mcw.carve      when it appears (the test app's DOWN), the .NET assemblies
 *                  in the shell's memory are copied to shell/, then it is
 *                  deleted
 */

#include "../bgm/bgm.h"
#include "../probe/sys.h"
#include "hde64.h"
#include "hook.h"

int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags,
                             const void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addr);
const char* sceKernelGetFsSandboxRandomWord(void);
int sceKernelGetModuleList(int* handles, size_t size, size_t* available);
// The same, system libraries too: on a PS4 (11.50) the shell's
// sceKernelGetModuleList gave eboot.bin alone.
int sceKernelGetModuleListInternal(int* handles, size_t size, size_t* available);
int scePthreadGetthreadid(void);

typedef struct {
    void* address;
    uint32_t size;
    int32_t prot;
} SegmentInfo;
typedef struct {
    size_t size;
    char name[256];
    SegmentInfo segment[4];
    uint32_t segmentCount;
    uint8_t fingerprint[20];
} ModuleInfo;  // OpenOrbis's OrbisKernelModuleInfo
int sceKernelGetModuleInfo(int handle, ModuleInfo* info);

#define LOG_PATH SC_BGM_DIR "/mcw.log"
#define LOG_OLD SC_BGM_DIR "/mcw.1.log"
#define LOG_MAX (8u << 20)
#define POST_BYTES HOOK_PRE_BYTES
#define IN_SHOWN 128  // of what a pointer argument points to before the call

// The functions, all of the music server's client libraries (shadPS4's
// aerolib), less sceMusicCoreServer.
static const char* const g_defaultNames[] = {
    "PlayControl", "GetPlayStatus", "GetPlayStatus_vsh", "GetTrackInfo", "GetSpTrackInfo",
    "GetEvent", "RegistEventCallback", "UnregistEventCallback", "TriggerEvent",
    "TriggerEventSync", "CheckCoreStatus", "GetCoreInfo", "GetStatusOfTracks", "SetShuffle",
    "SetRepeatMode", "SetAudioVolume", "SetAudioMute", "SetTrack", "SetTrackList", "SetPlaylist",
    "SetAutoPlay", "SelectCore", "TryLoadCore", "LaunchSpCore", "LaunchUsbMusicCore",
    "PlaySpUri", "KillCurrentCore", "ForceKillCurrentCore", "GetPlayStartCondition",
    "SetLaunchUser", "GetLastLaunchUser", "SetTitleId", "GetTitleId", "GetUserData",
    "GetSpWakeupAuthorityList", "SetSpWakeupAuthorityList", "UpdateSpWakeupAuthorityList",
    "Initialize", "Terminate",
    // A music core's side (libSceCustomMusicCore): found only in a core's
    // process, such as the USB Music Player's, not in the shell's.
    "sceCustomMusicCoreStartOperation", "sceCustomMusicCoreStartOperation2",
    "sceCustomMusicCoreStopOperation", "sceCustomMusicCoreBgmOpen", "sceCustomMusicCoreBgmOpen2",
    "sceCustomMusicCoreBgmStart", "sceCustomMusicCoreBgmStop", "sceCustomMusicCoreBgmClose",
    "sceCustomMusicCoreBgmOutput", "sceCustomMusicCoreBgmSetAudioVolume",
    "sceCustomMusicCoreGetBgmAuthorityStatus", "sceCustomMusicCoreGetSystemAudioVolume",
    "sceCustomMusicCoreSetSystemAudioVolume", "sceCustomMusicCoreGetHdcpStatus",
    "sceCustomMusicCorePostEvent", "sceCustomMusicCoreSendEvent",
    "sceCustomMusicCoreSendMulticastEvent", "sceCustomMusicCoreSendSpMessage",
    // PSN (0.38): what the Music section asks before it shows "Sign in to
    // PlayStation Network", and the web API it gets Spotify's lists from
    // (/v1/ebgm/...). Watched only.
    "sceNpGetState", "sceNpGetAccountIdA", "sceNpGetOnlineId", "sceNpGetNpReachabilityState",
    "sceNpCheckNpAvailability", "sceNpCheckNpAvailabilityA", "sceNpHasSignedUp",
    "sceNpIsPlusMember", "sceNpWebApiCreateContext", "sceNpWebApiCreateContextA",
    "sceNpWebApiCreateRequest", "sceNpWebApiAddHttpRequestHeader", "sceNpWebApiSendRequest",
    "sceNpWebApiSendRequest2", "sceNpWebApiGetHttpStatusCode", "sceNpWebApiReadData",
    "sceNpWebApiDeleteRequest", "sceNpWebApiGetHttpResponseHeaderValueLength",
    "sceNpWebApiGetHttpResponseHeaderValue",
};
#define N_DEFAULT (int)(sizeof(g_defaultNames) / sizeof(g_defaultNames[0]))

#define MAX_HOOKS 96
#define MAX_CALLBACKS 8
static Hook g_hooks[MAX_HOOKS];
static char g_names[MAX_HOOKS][80];
static int g_nHooks;
static volatile int g_quiet;
static volatile int g_installLock;
static uint64_t g_began;

// ── The log: lines from any thread into a buffer, written by this module's
// thread ──────────────────────────────────────────────────────────────────────

#define BUF (256u * 1024u)
static char* g_buf[2];
static uint32_t g_used;
static int g_active;
static volatile int g_bufLock;
static volatile uint32_t g_dropped;

static void lock(volatile int* l) {
    while (__atomic_test_and_set(l, __ATOMIC_ACQUIRE))
        ;
}
static void unlock(volatile int* l) {
    __atomic_clear(l, __ATOMIC_RELEASE);
}

static void put(const char* line, int n) {
    if (!g_buf[0] || n <= 0) return;
    lock(&g_bufLock);
    if (g_used + (uint32_t)n > BUF) {
        g_dropped++;
    } else {
        memcpy(g_buf[g_active] + g_used, line, (size_t)n);
        g_used += (uint32_t)n;
    }
    unlock(&g_bufLock);
}

static void say(const char* f, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* f, ...) {
    char line[640];
    const uint64_t ms = (sysNowUs() - g_began) / 1000u;
    int n = fmt(line, sizeof(line), "%6u.%03u ", (unsigned)(ms / 1000u), (unsigned)(ms % 1000u));
    va_list ap;
    va_start(ap, f);
    n += fmtv(line + n, sizeof(line) - n, f, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    put(line, n);
}

static int g_fd = -1;
static uint32_t g_written;

static void drain(void) {
    if (g_fd < 0 || !g_buf[0]) return;
    lock(&g_bufLock);
    char* full = g_buf[g_active];
    const uint32_t n = g_used;
    const uint32_t dropped = g_dropped;
    g_active ^= 1;
    g_used = 0;
    g_dropped = 0;
    unlock(&g_bufLock);
    if (n) sysWrite(g_fd, full, n);
    g_written += n;
    if (dropped) {
        char line[96];
        const int k = fmt(line, sizeof(line), "(%u lines lost: the log was full)\n", dropped);
        sysWrite(g_fd, line, (size_t)k);
    }
    if (n || dropped) sysFsync(g_fd);
    if (g_written > LOG_MAX) {
        sysClose(g_fd);
        sysUnlink(LOG_OLD);
        sysRename(LOG_PATH, LOG_OLD);
        g_fd = sysOpen(LOG_PATH, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
        g_written = 0;
    }
}

// ── Each call ────────────────────────────────────────────────────────────────

static uint64_t hashBytes(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

// Up to the last byte that is not 0, in whole lines of 16.
static size_t trimmed(const uint8_t* p, size_t n) {
    while (n && !p[n - 1]) n--;
    return (n + 15) & ~(size_t)15;
}

// Rows of 16 bytes; with was, only the rows that differ from it (all of
// those past wasLen).
static void dump(const char* label, const uint8_t* p, size_t n, const uint8_t* was,
                 size_t wasLen) {
    n = trimmed(p, n);
    if (!n) {
        say("      %s: all 0", label);
        return;
    }
    for (size_t at = 0; at < n; at += 16) {
        if (was && at + 16 <= wasLen && memcmp(p + at, was + at, 16) == 0) continue;
        char hex[16 * 3 + 1], asc[17];
        const size_t k = n - at < 16 ? n - at : 16;
        for (size_t i = 0; i < 16; i++) {
            static const char d[] = "0123456789abcdef";
            if (i < k) {
                hex[i * 3] = d[p[at + i] >> 4];
                hex[i * 3 + 1] = d[p[at + i] & 15];
                asc[i] = p[at + i] >= 0x20 && p[at + i] < 0x7F ? (char)p[at + i] : '.';
            } else {
                hex[i * 3] = hex[i * 3 + 1] = ' ';
                asc[i] = ' ';
            }
            hex[i * 3 + 2] = ' ';
        }
        hex[48] = 0;
        asc[16] = 0;
        say("      %s +%03x  %s |%s|", label, (unsigned)at, hex, asc);
    }
}

// A pointer argument worth reading: not to the stack below the caller's,
// which the call itself reuses (an argument register left over from before).
static int deadStack(const Frame* f, uint64_t p) {
    const uint64_t sp = (uint64_t)(uintptr_t)f->callerStack;
    return p < sp && sp - p < (1u << 20);
}

static int isRegist(const Hook* h) {
    for (const char* n = h->name; *n; n++)
        if (n[0] == 'R' && n[1] == 'e' && n[2] == 'g' && n[3] == 'i' && n[4] == 's' && n[5] == 't')
            return 1;
    return 0;
}

static int g_nCallbacks;
static Hook g_cbHooks[MAX_CALLBACKS];
static char g_cbNames[MAX_CALLBACKS][96];

// A callback handed to a Regist... function, hooked as well.
static void hookCallback(const Hook* via, int arg, uint64_t fn) {
    lock(&g_installLock);
    for (int i = 0; i < g_nCallbacks; i++)
        if ((uint64_t)(uintptr_t)g_cbHooks[i].target == fn) {
            unlock(&g_installLock);
            return;
        }
    for (int i = 0; i < g_nHooks; i++)
        if ((uint64_t)(uintptr_t)g_hooks[i].target == fn) {
            unlock(&g_installLock);
            return;
        }
    if (g_nCallbacks >= MAX_CALLBACKS) {
        unlock(&g_installLock);
        return;
    }
    Hook* h = &g_cbHooks[g_nCallbacks];
    fmt(g_cbNames[g_nCallbacks], sizeof(g_cbNames[0]), "callback %lx (a%d of %s)", fn, arg,
        via->name);
    h->name = g_cbNames[g_nCallbacks];
    const char* why = "";
    const int rc = hookInstall(h, (void*)(uintptr_t)fn, &why);
    if (rc == 0) g_nCallbacks++;
    unlock(&g_installLock);
    say("   hooking %s: %s%s", h->name, rc == 0 ? "hooked" : "not hooked: ", why);
}

// ── Answering the quick menu for our player (mcw-fake.txt) ───────────────────
//
// The shell's own declarations (its app.exe and platform.dll, copied from
// its memory and decompiled: Sce.Vsh.ShellUI.MusicCoreServer.
// MusicCoreServerWrapper) give the music server's structures. The quick
// menu's entry for the current music core (MainMenuType.MusicApp, its
// MusicPanel: cover, title, album, artist, time and buttons) shows the core
// named by GetCoreInfo once the event "core ready" (1) has come; "play
// status changed" (2) and "track info changed" (3) have it ask again. (The
// Music section, MainMenuType.PSMusic, is Spotify's PlayStationMusicPanel.)
//
// While mcw-fake.txt is there and our player has a track, after each real
// call: GetEvent gives core ready, then track info, then play status, as
// they are owed; GetCoreInfo, GetPlayStatus, GetTrackInfo and GetTitleId
// describe our player as a core whose title is the file's first word
// (SCMP00001, the test app, by default); PlayControl's play, pause, next and
// previous go to our player. When it stops: "core killed" (5).

#define FAKE SC_BGM_DIR "/mcw-fake.txt"
#define EV_CORE_READY 1
#define EV_PLAY_STATUS 2
#define EV_TRACK_INFO 3
#define EV_SP_MESSAGE 4
#define EV_CORE_KILLED 5
#define EV_SP_TRACK 6  // SP_TRACKINFOCHANGED: GetSpTrackInfo, the track's Spotify id, again
#define TRACK_CURRENT 0xFFFFFFFEu  // SCE_MUSICCORE_CURRENT_TRACK_INDEX
#define TRACK_PREV 0xFFFFFFFDu
#define TRACK_NEXT 0xFFFFFFFCu

// SceMusicCoreCoreInfo, 0x48 bytes.
#define CI_SCHEME 0x00  // char[32]
#define CI_TITLE 0x20   // char[10]
#define CI_CONTENT 0x2C // then 6 capabilities, u32 each, 1 supported
#define CI_SIZE 0x48
// SceMusicCorePlayStatusInfo, 0x70 bytes (as seen, the USB Music Player's).
#define PS_SIZE 0x70
// SceMusicCoreTrackInfo: srcUri[1048], trackName[341], albumName[341],
// artistName[341], a byte, coverartUri[1048], coverartOffset, coverartSize.
#define TI_SRC 0
#define TI_TRACK 1048
#define TI_ALBUM (TI_TRACK + 341)
#define TI_ARTIST (TI_ALBUM + 341)
#define TI_COVER (TI_ARTIST + 341 + 1)
#define TI_COVER_OFF (TI_COVER + 1048)
#define TI_SIZE (TI_COVER_OFF + 16)
_Static_assert(TI_COVER_OFF == 3120, "SceMusicCoreTrackInfo");

static struct {
    volatile int active;
    volatile uint32_t state;  // 2 playing, 3 paused
    volatile uint64_t durS, posS;
    volatile uint32_t index, count;
    volatile uint32_t volume;  // 0 to 32768, as the quick menu sets it
    volatile uint32_t repeat, shuffle;  // SceMusicCoreRepeatMode, SceMusicCoreShuffle
    volatile uint32_t owed;  // events owed: 1 << type
    char title[16];
    // The track, under a lock: the watch's thread writes, the shell's read.
    volatile int lock;
    char file[800], name[300], artist[300], album[300], cover[1048];  // file, cover: URLs too (0.72)
    uint64_t artOff, artLen;
} g_fake;

static int eq(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

// key=value's value from status.txt's text, or "".
static void statusField(const char* text, const char* key, char* out, size_t cap) {
    const size_t n = strlen(key);
    out[0] = 0;
    for (const char* p = text; *p;) {
        if (memcmp(p, key, n) == 0 && p[n] == '=') {
            p += n + 1;
            size_t k = 0;
            while (*p && *p != '\n' && k + 1 < cap) out[k++] = *p++;
            out[k] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

static uint64_t toU64(const char* s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

static void owe(int type) {
    __atomic_or_fetch(&g_fake.owed, 1u << type, __ATOMIC_RELAXED);
}

static void fakeRefresh(void) {
    static char text[4096];
    char v[800];
    int on = 0;
    // On unless mcw-fake.txt says off; its first word, else SCMP00001, the
    // title (0.35 and before: off unless the file was there).
    int fd = sysOpen(FAKE, SC_O_RDONLY, 0);
    v[0] = 0;
    if (fd >= 0) {
        const int n = sysRead(fd, v, 64);
        sysClose(fd);
        v[n > 0 ? n : 0] = 0;
    }
    fd = -1;
    if (!(v[0] == 'o' && v[1] == 'f' && v[2] == 'f')) {
        char title[16];
        size_t k = 0;
        for (const char* p = v; *p && *p != '\n' && *p != '\r' && *p != ' ' && k < 9; p++)
            title[k++] = *p;
        if (!k) memcpy(title, "CUSA01780", (k = 9));  // Spotify's: the Music section shows us too
        title[k] = 0;
        if (!eq(title, g_fake.title)) {
            // Another title while we are the core: announced again, as a
            // core newly ready (0.34 kept the first).
            memcpy(g_fake.title, title, k + 1);
            if (g_fake.active) {
                owe(EV_CORE_READY);
                owe(EV_TRACK_INFO);
                owe(EV_PLAY_STATUS);
                say("fake: the title is now %s: core ready again", g_fake.title);
            }
        }
        fd = sysOpen(SC_BGM_STATUS, SC_O_RDONLY, 0);
    }
    if (fd >= 0) {
        const int m = sysRead(fd, text, sizeof(text) - 1);
        sysClose(fd);
        text[m > 0 ? m : 0] = 0;
        statusField(text, "beat_ms", v, sizeof(v));
        const uint64_t beat = toU64(v), now = sysNowUs() / 1000u;
        statusField(text, "state", v, sizeof(v));
        const uint32_t state = eq(v, "playing") ? 2 : eq(v, "paused") ? 3 : 0;
        on = state && beat && now >= beat && now - beat < 4000;
        if (on) {
            statusField(text, "pos_ms", v, sizeof(v));
            const uint64_t pos = toU64(v) / 1000u;
            statusField(text, "dur_ms", v, sizeof(v));
            g_fake.durS = toU64(v) / 1000u;
            statusField(text, "index", v, sizeof(v));
            g_fake.index = (uint32_t)toU64(v);
            statusField(text, "count", v, sizeof(v));
            g_fake.count = (uint32_t)toU64(v);
            statusField(text, "volume", v, sizeof(v));
            if (v[0]) g_fake.volume = (uint32_t)toU64(v);
            statusField(text, "repeat", v, sizeof(v));
            const uint32_t repeat = (uint32_t)toU64(v);
            statusField(text, "shuffle", v, sizeof(v));
            const uint32_t shuffle = (uint32_t)toU64(v);
            if (repeat != g_fake.repeat || shuffle != g_fake.shuffle) owe(EV_PLAY_STATUS);
            g_fake.repeat = repeat;
            g_fake.shuffle = shuffle;
            static char cover[1048];
            statusField(text, "cover", cover, sizeof(cover));
            statusField(text, "file", v, sizeof(v));
            // A new track, or the same one opened again (a new cover file).
            if (!g_fake.active || !eq(v, g_fake.file) || !eq(cover, g_fake.cover)) {
                lock(&g_fake.lock);
                fmt(g_fake.file, sizeof(g_fake.file), "%s", v);
                statusField(text, "title", g_fake.name, sizeof(g_fake.name));
                if (!g_fake.name[0]) {  // the file's name, without its folder and type
                    const char* base = v;
                    for (const char* q = v; *q; q++)
                        if (*q == '/') base = q + 1;
                    fmt(g_fake.name, sizeof(g_fake.name), "%s", base);
                    for (char* q = g_fake.name + strlen(g_fake.name); q > g_fake.name; q--)
                        if (*q == '.') {
                            *q = 0;
                            break;
                        }
                }
                statusField(text, "artist", g_fake.artist, sizeof(g_fake.artist));
                statusField(text, "album", g_fake.album, sizeof(g_fake.album));
                statusField(text, "art_off", v, sizeof(v));
                g_fake.artOff = toU64(v);
                statusField(text, "art_len", v, sizeof(v));
                g_fake.artLen = toU64(v);
                fmt(g_fake.cover, sizeof(g_fake.cover), "%s", cover);
                unlock(&g_fake.lock);
                owe(EV_TRACK_INFO);
                owe(EV_SP_TRACK);
                say("fake: track \"%s\" by \"%s\" (%s), cover %lu bytes at %lu, %s", g_fake.name,
                    g_fake.artist, g_fake.file, g_fake.artLen, g_fake.artOff,
                    g_fake.cover[0] ? g_fake.cover : "no cover file");
            }
            if (!g_fake.active) {
                owe(EV_CORE_READY);
                say("fake: our player is the music core, as %s", g_fake.title);
            }
            if (state != g_fake.state || pos != g_fake.posS || !g_fake.active)
                owe(EV_PLAY_STATUS);
            if (state != g_fake.state || !g_fake.active)
                say("fake: %s, %lu of %lu s, track %u of %u", state == 2 ? "playing" : "paused",
                    pos, g_fake.durS, g_fake.index + 1, g_fake.count);
            g_fake.state = state;
            g_fake.posS = pos;
        }
    }
    if (!on && g_fake.active) {
        say("fake: off (%s)", fd >= 0 ? "our player has no track" : "mcw-fake.txt says off");
        g_fake.owed = 1u << EV_CORE_KILLED;
    }
    g_fake.active = on;
}

static void put32At(uint8_t* p, size_t at, uint32_t v) {
    memcpy(p + at, &v, 4);
}
static void put64At(uint8_t* p, size_t at, uint64_t v) {
    memcpy(p + at, &v, 8);
}
static void putStr(uint8_t* p, size_t at, size_t cap, const char* s) {
    const size_t n = strlen(s);
    memcpy(p + at, s, n < cap - 1 ? n : cap - 1);
}

#define SC_O_APPEND_ 0x0008
static void sendPlayer(const char* cmd) {
    // Added to what is there (0.67): two commands in one player's turn
    // (~85 ms) are both kept. The player renames cmd before it reads it.
    const int fd = sysOpen(SC_BGM_CMD, SC_O_WRONLY | SC_O_CREAT | SC_O_APPEND_, 0666);
    if (fd >= 0) {
        sysWrite(fd, cmd, strlen(cmd));
        sysClose(fd);
    }
}

// The player's queue_rev (status.txt), counted on once for each edit of
// the queue: -1 if not known.
static int statusInt(const char* key) {
    char text[4096];
    const int fd = sysOpen(SC_BGM_STATUS, SC_O_RDONLY, 0);
    if (fd < 0) return -1;
    const int m = sysRead(fd, text, sizeof(text) - 1);
    sysClose(fd);
    text[m > 0 ? m : 0] = 0;
    char v[24];
    statusField(text, key, v, sizeof(v));
    return v[0] ? (int)toU64(v) : -1;
}

static int queueRevNow(void) {
    return statusInt("queue_rev");
}

// Edits of the queue sent and not yet seen made (0.67): the Music section's
// asking for the queue's tracks is held until they are.
static struct {
    int lock;
    int base, pending;
} g_edits;

static void sendEdit(const char* cmd) {
    lock(&g_edits.lock);
    if (!g_edits.pending) g_edits.base = queueRevNow();
    g_edits.pending++;
    unlock(&g_edits.lock);
    sendPlayer(cmd);
}

// The row last chosen in a track list: its track (the library's number, from
// its uri, sc<n>), or -1 for Play All; when (0.77).
static struct {
    int lib;
    uint64_t us;
} g_chosen = {-1, 0};

// Before the queue's tracks are read for the Music section: the edits sent
// made, waited for (up to 1.5 s, never on the UI thread).
static int g_uiThread;
static void editsWait(void) {
    lock(&g_edits.lock);
    const int pending = g_edits.pending, want = g_edits.base + g_edits.pending;
    unlock(&g_edits.lock);
    if (!pending || g_edits.base < 0) {
        g_edits.pending = 0;
        return;
    }
    int rev = queueRevNow(), waited = 0;
    if (scePthreadGetthreadid() != g_uiThread)
        for (; rev < want && waited < 1500; waited += 50) {
            sysSleepMs(50);
            rev = queueRevNow();
        }
    say("queue: %d edit(s) sent, queue_rev %d (wanted %d), %d ms waited", pending, rev, want, waited);
    lock(&g_edits.lock);
    g_edits.pending = 0;
    unlock(&g_edits.lock);
}

static uint8_t* outBuf(const Frame* f, int arg, size_t size) {
    uint8_t* p = (uint8_t*)(uintptr_t)f->gpr[arg];
    if (hookReadable(p, size) < size || !(hookProt(p) & 2)) return NULL;
    return p;
}

// ── The Music section's web requests (ebgm/, an experiment) ──────────────────
//
// Each time the Music section opens, the shell asks PSN's web API, group
// "music", for /v1/ebgm/me/partner/profile?partner=spotify; signed out, the
// send fails at once (0x80550003) and the section shows "Sign in to
// PlayStation Network" (0.38's log). Here, a request of group "music" whose
// answer is a file in ebgm/ is answered with it: the send succeeds, the status
// is 200, ReadData gives the file. Any other request, of any other group or
// with no file, is left as it was, its path logged. Nothing is sent anywhere.
//
// The file: the path after /v1/ebgm/, each character not a letter or digit
// made '_', ".json" after it; with its query first, then without, then
// without its last part (me_partner_profile_partner_spotify.json, then
// me_partner_profile.json; recommended_views_SCMP00001_00.json, then
// recommended_views.json).

static int contains(const char* s, const char* part);

#define EBGM_DIR SC_BGM_DIR "/ebgm"
#define EBGM_SLOTS 16
#define EBGM_MAX (256u * 1024u)  // 0.84: 64 KB, which the front page of 300 albums nearly filled
#define TRACKS_MAX (4u * 1024u * 1024u)  // a track list file, whole (0.78; 1 MB before 0.84)

typedef struct {
    int64_t id;     // the request's, 0 for a free slot
    uint32_t len, off;
    char* body;     // EBGM_MAX bytes of g_ebgmPool
} EbgmReq;
static EbgmReq g_ebgm[EBGM_SLOTS];
static volatile uint64_t g_jsonUntil;  // JSON keys are logged until then: 10 s after an answer
static char* g_ebgmPool;
static volatile int g_ebgmLock;

// how: 0 with the query, 1 without, 2 without the last part of the path too
// (recommended/views/SCMP00001_00 -> recommended_views: the views asked for
// the app in front, one file for any).
static void ebgmName(const char* path, int how, char* out, size_t cap) {
    size_t o = (size_t)fmt(out, cap, EBGM_DIR "/");
    const char* end = path;
    while (*end && *end != '?') end++;
    if (how == 2) {
        while (end > path && end[-1] != '/') end--;
        if (end > path) end--;
    }
    for (const char* p = path; *p && o + 6 < cap; p++) {
        if (how && p >= end) break;
        const char c = *p;
        out[o++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    fmt(out + o, cap - o, ".json");
}

static EbgmReq* ebgmFind(int64_t id) {
    for (int i = 0; i < EBGM_SLOTS; i++)
        if (g_ebgm[i].id == id && id) return &g_ebgm[i];
    return NULL;
}

// After CreateRequest(ctx, apiGroup, path, method, content, int64* requestId).
// A slot for request id, its answer text: the slot, or NULL.
static EbgmReq* ebgmClaim(int64_t id) {
    if (!g_ebgmPool) g_ebgmPool = sysMap(EBGM_SLOTS * EBGM_MAX);
    if (!g_ebgmPool) return NULL;
    lock(&g_ebgmLock);
    EbgmReq* r = NULL;
    for (int i = 0; i < EBGM_SLOTS && !r; i++)
        if (!g_ebgm[i].id) r = &g_ebgm[i];
    if (r) {
        r->body = g_ebgmPool + (size_t)(r - g_ebgm) * EBGM_MAX;
        r->len = r->off = 0;
        r->id = id;
    }
    unlock(&g_ebgmLock);
    return r;
}

// ── "Add to your music" (R3 on the player panel): liked.txt ──────────────────
//
// /v1/ebgm/users/me/library/contains|save|delete/<id>, for the playing track
// (its id sc<n>, GetSpTrackInfo's): answered here, the track's file kept in
// liked.txt (one path a line) for an app to read, or taken out of it.

#define LIKED SC_BGM_DIR "/liked.txt"

static int likedHas(const char* file, char* text, size_t cap) {
    const int fd = sysOpen(LIKED, SC_O_RDONLY, 0);
    int n = fd >= 0 ? sysRead(fd, text, cap - 1) : 0;
    if (fd >= 0) sysClose(fd);
    text[n > 0 ? n : 0] = 0;
    const size_t len = strlen(file);
    for (const char* p = text; *p;) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        if ((size_t)(e - p) == len && !memcmp(p, file, len)) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

static void likedSet(const char* file, int on) {
    static char text[512 * 1024];  // liked.txt (0.84: 64 KB)
    const int had = likedHas(file, text, sizeof(text));
    if (on == had) return;
    const int fd = sysOpen(LIKED ".tmp", SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (fd < 0) return;
    const size_t len = strlen(file);
    for (const char* p = text; *p;) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        if (!((size_t)(e - p) == len && !memcmp(p, file, len)) && e > p) {
            sysWrite(fd, p, (size_t)(e - p));
            sysWrite(fd, "\n", 1);
        }
        p = *e ? e + 1 : e;
    }
    if (on) {
        sysWrite(fd, file, len);
        sysWrite(fd, "\n", 1);
    }
    sysClose(fd);
    sysRename(LIKED ".tmp", LIKED);
}

// sub: the path after /v1/ebgm/. 1 if it was a library request, answered.
static int ebgmLibrary(const char* sub, int64_t id) {
    static const char lib[] = "users/me/library/";
    if (memcmp(sub, lib, sizeof(lib) - 1) != 0) return 0;
    const char* act = sub + sizeof(lib) - 1;
    const int contains_ = !memcmp(act, "contains/", 9);
    const int save = !memcmp(act, "save/", 5);
    const int del = !memcmp(act, "delete/", 7);
    if (!contains_ && !save && !del) return 0;
    char file[800];
    lock(&g_fake.lock);
    fmt(file, sizeof(file), "%s", g_fake.file);
    unlock(&g_fake.lock);
    if (!file[0]) return 0;
    if (save || del) {
        likedSet(file, save);
        // The Liked list again (0.67), and the press, for the app's own
        // action (0.86: the player's plus.c), with the track's number.
        char cmd[48];
        fmt(cmd, sizeof(cmd), "liked\nplus_%s=%d\n", save ? "save" : "delete", statusInt("track"));
        sendPlayer(cmd);
    }
    static char text[512 * 1024];  // liked.txt (0.84: 64 KB)
    const int has = likedHas(file, text, sizeof(text));
    EbgmReq* r = ebgmClaim(id);
    if (!r) return 0;
    r->len = (uint32_t)fmt(r->body, EBGM_MAX, "{\"contains_in_library\":%s}", has ? "true" : "false");
    say("ebgm: library %s for %s: %s", contains_ ? "contains" : save ? "save" : "delete", file,
        has ? "in liked.txt" : "not in liked.txt");
    return 1;
}

// A query's number (name=<n>) in path, or -1.
static int queryInt(const char* path, const char* name) {
    const size_t n = strlen(name);
    for (const char* p = path; *p; p++)
        if ((p == path || p[-1] == '?' || p[-1] == '&') && !memcmp(p, name, n) && p[n] == '=') {
            int v = 0;
            for (p += n + 1; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
            return v;
        }
    return -1;
}

// The page asked for of a track list (0.78): its tracks from offset, limit
// of them, the total all of them. 0.77 and before gave every track whatever
// was asked: the shell, asking for 15, kept 15, and its rows past them
// showed the list's first tracks again, a row labelled Celebration playing
// the test tone. Into out; its length, or 0 if in is not such a list.
static size_t tracksPage(const char* in, size_t n, int offset, int limit, char* out, size_t cap) {
    const char* key = "\"track_list\":[";
    const size_t kn = strlen(key);
    size_t at = 0;
    while (at + kn <= n && memcmp(in + at, key, kn)) at++;
    if (at + kn > n) return 0;
    at += kn;
    // The header is written last, its total counted first. Items: each a {...} at the array's top, strings minded.
    int count = 0, kept = 0;
    char* body = out + 128;  // room before it for the header
    size_t bo = 0;
    while (at < n) {
        while (at < n && (in[at] == ' ' || in[at] == ',' || in[at] == '\n' || in[at] == '\r')) at++;
        if (at >= n || in[at] != '{') break;
        const size_t start = at;
        int depth = 0, str = 0;
        for (; at < n; at++) {
            const char c = in[at];
            if (str) {
                if (c == '\\') at++;
                else if (c == '"') str = 0;
            } else if (c == '"') {
                str = 1;
            } else if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    at++;
                    break;
                }
            }
        }
        if (count >= offset && kept < limit) {
            const size_t len = at - start;
            if (128 + bo + len + 8 >= cap) break;
            if (kept) body[bo++] = ',';
            memcpy(body + bo, in + start, len);
            bo += len;
            kept++;
        }
        count++;
    }
    char head[128];
    const int hn = fmt(head, sizeof(head), "{\"total\":%d,\"offset\":%d,\"limit\":%d,\"track_list\":[",
                       count, offset, limit);
    memmove(out + hn, body, bo);
    memcpy(out, head, (size_t)hn);
    const size_t o = (size_t)hn + bo;
    memcpy(out + o, "]}\n", 3);
    return o + 3;
}

static void ebgmCreated(Frame* f) {
    if ((int32_t)f->retRax < 0) return;
    const char* group = (const char*)(uintptr_t)f->gpr[1];
    const char* path = (const char*)(uintptr_t)f->gpr[2];
    int64_t* idp = (int64_t*)(uintptr_t)f->gpr[5];
    if (hookReadable(group, 8) < 6 || hookReadable(path, 16) < 16 || hookReadable(idp, 8) < 8)
        return;
    // "music" (/v1/ebgm/...), and "socialnetwork"'s Spotify session link,
    // which a playlist's track list asks for once Spotify's app is there
    // (0.52: /socialnetwork/api/v1/sp/sessionlink -> sp_sessionlink.json).
    const int social = eq(group, "socialnetwork");
    if (!eq(group, "music") && !social) return;
    const char* sub = path;
    static const char prefix[] = "/v1/ebgm/";
    static const char socialPrefix[] = "/socialnetwork/api/v1/";
    if (!social && memcmp(path, prefix, sizeof(prefix) - 1) == 0) sub = path + sizeof(prefix) - 1;
    if (social) {
        if (memcmp(path, socialPrefix, sizeof(socialPrefix) - 1) != 0) return;
        sub = path + sizeof(socialPrefix) - 1;
    }
    if (!social && ebgmLibrary(sub, *idp)) {
        g_jsonUntil = sysNowUs() + 10000000u;
        return;
    }
    char name[4][200];
    for (int i = 0; i < 3; i++) ebgmName(sub, i, name[i + 1], sizeof(name[0]));
    // A playlist's tracks by its id, the end of its uri (...%3Aplaylist%3Aalbum3:
    // tracks_album3.json), as our player writes them (ps4/bgm, writeLibrary).
    name[0][0] = 0;
    const char* uri = sub;
    while (*uri && memcmp(uri, "uri=", 4) != 0) uri++;
    if (*uri && contains(sub, "tracks")) {
        uri += 4;
        const char* end = uri;
        while (*end && *end != '&') end++;
        const char* id = uri;
        for (const char* q = uri; q + 2 < end; q++)
            if (q[0] == '%' && q[1] == '3' && (q[2] == 'A' || q[2] == 'a')) id = q + 3;
        for (const char* q = uri; q < end; q++)
            if (*q == ':') id = q + 1;
        char idText[48];
        size_t k = 0;
        for (const char* q = id; q < end && k + 1 < sizeof(idText); q++)
            idText[k++] = ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9')) ? *q : '_';
        idText[k] = 0;
        fmt(name[0], sizeof(name[0]), EBGM_DIR "/tracks_%s.json", idText);
        if (eq(idText, "queue")) editsWait();
    }
    int fd = -1, which = 0;
    for (; which < 4 && fd < 0; which++)
        if (name[which][0]) fd = sysOpen(name[which], SC_O_RDONLY, 0);
    if (fd < 0) {
        say("ebgm: %s (method %lu): no file (%s, %s, %s or %s), left as it was", path, f->gpr[3],
            name[0], name[1], name[2], name[3]);
        return;
    }
    if (!g_ebgmPool) g_ebgmPool = sysMap(EBGM_SLOTS * EBGM_MAX);
    lock(&g_ebgmLock);
    EbgmReq* r = NULL;
    for (int i = 0; i < EBGM_SLOTS && !r; i++)
        if (!g_ebgm[i].id) r = &g_ebgm[i];
    if (r && g_ebgmPool) {
        r->body = g_ebgmPool + (size_t)(r - g_ebgm) * EBGM_MAX;
        // A track list: read whole, and only the page asked for answered.
        static char* whole;
        if (!whole) whole = (char*)sysMap(TRACKS_MAX);
        const int pageOffset = queryInt(path, "offset"), pageLimit = queryInt(path, "limit");
        size_t paged = 0;
        if (which == 1 && name[0][0] && whole && pageLimit > 0) {
            size_t got = 0;
            int k;
            while (got < TRACKS_MAX && (k = sysRead(fd, whole + got, TRACKS_MAX - got)) > 0) got += (size_t)k;
            paged = tracksPage(whole, got, pageOffset < 0 ? 0 : pageOffset, pageLimit, r->body, EBGM_MAX);
            if (paged) say("ebgm: tracks %d to %d of the list asked for", pageOffset, pageOffset + pageLimit - 1);
            else sysSeek(fd, 0, 0);
        }
        const int n = paged ? (int)paged : sysRead(fd, r->body, EBGM_MAX);
        r->len = n > 0 ? (uint32_t)n : 0;
        r->off = 0;
        r->id = *idp;
    }
    unlock(&g_ebgmLock);
    sysClose(fd);
    g_jsonUntil = sysNowUs() + 10000000u;
    say("ebgm: %s: answered with %s (%u bytes), request %lx", path, name[which - 1],
        r ? r->len : 0, r ? (uint64_t)r->id : 0);
}

static void ebgmApply(Frame* f) {
    const Hook* h = f->hook;
    if (eq(h->name, "sceNpWebApiCreateRequest")) {
        ebgmCreated(f);
        return;
    }
    if (h->name[0] != 's' || !contains(h->name, "sceNpWebApi")) return;
    EbgmReq* r = ebgmFind((int64_t)f->gpr[0]);
    if (!r) return;
    if (eq(h->name, "sceNpWebApiSendRequest")) {
        f->retRax = 0;
    } else if (eq(h->name, "sceNpWebApiSendRequest2")) {
        // (requestId, data, size, SceNpWebApiResponseInformationOption*):
        // httpStatus, then the error object's pointer and size, then the
        // response's size.
        uint8_t* info = (uint8_t*)(uintptr_t)f->gpr[3];
        if (info && hookReadable(info, 32) == 32 && (hookProt(info) & 2)) {
            put32At(info, 0, 200);
            put64At(info, 24, r->len);
        }
        f->retRax = 0;
    } else if (eq(h->name, "sceNpWebApiGetHttpStatusCode")) {
        uint8_t* code = (uint8_t*)(uintptr_t)f->gpr[1];
        if (code && hookReadable(code, 4) == 4 && (hookProt(code) & 2)) put32At(code, 0, 200);
        f->retRax = 0;
    } else if (eq(h->name, "sceNpWebApiReadData")) {
        // (requestId, data, size): bytes read, 0 at the end.
        uint8_t* out = (uint8_t*)(uintptr_t)f->gpr[1];
        size_t want = (size_t)f->gpr[2];
        const uint32_t left = r->len - r->off;
        if (want > left) want = left;
        if (want && (hookReadable(out, want) < want || !(hookProt(out) & 2))) return;
        memcpy(out, r->body + r->off, want);
        r->off += (uint32_t)want;
        f->retRax = (uint64_t)want;
    } else if (eq(h->name, "sceNpWebApiGetHttpResponseHeaderValueLength")) {
        // (requestId, name, size_t* length)
        const char* name = (const char*)(uintptr_t)f->gpr[1];
        uint8_t* len = (uint8_t*)(uintptr_t)f->gpr[2];
        if (!len || hookReadable(len, 8) < 8 || !(hookProt(len) & 2) || hookReadable(name, 4) < 4)
            return;
        const int cl = contains(name, "ength");
        put64At(len, 0, cl ? 12 : 16);  // "Content-Length"'s digits, or "application/json"
        f->retRax = 0;
    } else if (eq(h->name, "sceNpWebApiGetHttpResponseHeaderValue")) {
        // (requestId, name, char* value, size, size_t* length)
        const char* name = (const char*)(uintptr_t)f->gpr[1];
        char* value = (char*)(uintptr_t)f->gpr[2];
        const size_t cap = (size_t)f->gpr[3];
        if (!value || cap < 2 || hookReadable(value, cap) < cap || !(hookProt(value) & 2) ||
            hookReadable(name, 4) < 4)
            return;
        if (contains(name, "ength")) fmt(value, cap, "%u", r->len);
        else fmt(value, cap, "application/json");
        uint8_t* len = (uint8_t*)(uintptr_t)f->gpr[4];
        if (len && hookReadable(len, 8) == 8 && (hookProt(len) & 2)) put64At(len, 0, strlen(value));
        f->retRax = 0;
    } else if (eq(h->name, "sceNpWebApiDeleteRequest")) {
        lock(&g_ebgmLock);
        say("ebgm: request %lx done, %u of %u bytes read", (uint64_t)r->id, r->off, r->len);
        r->id = 0;
        unlock(&g_ebgmLock);
    }
}


// After the real call, before it is logged: the answer, ours.
static void fakeApply(Frame* f) {
    const Hook* h = f->hook;
    if (eq(h->name, "GetEvent")) {
        // An event owed, when the server had none: killed only once we are off.
        if ((uint32_t)f->retRax != 0x80FD0014u || !g_fake.owed) return;
        uint8_t* p = outBuf(f, 0, 8);
        if (!p) return;
        const uint32_t owed = g_fake.owed;
        const int type = (owed & (1u << EV_CORE_KILLED))   ? EV_CORE_KILLED
                         : (owed & (1u << EV_CORE_READY))  ? EV_CORE_READY
                         : (owed & (1u << EV_TRACK_INFO))  ? EV_TRACK_INFO
                         : (owed & (1u << EV_SP_MESSAGE))  ? EV_SP_MESSAGE
                         : (owed & (1u << EV_SP_TRACK))    ? EV_SP_TRACK
                                                           : EV_PLAY_STATUS;
        if (type != EV_CORE_KILLED && !g_fake.active) return;
        __atomic_and_fetch(&g_fake.owed, ~(1u << type), __ATOMIC_RELAXED);
        put32At(p, 0, (uint32_t)type);
        // A Spotify message's param: 200, MESSAGE_LOGIN_SUCCESS, what a
        // Spotify core says once it plays a URI it was given (PlaySpUri).
        put32At(p, 4, type == EV_SP_MESSAGE ? 200u : 0u);
        f->retRax = 0;
        if (type != EV_PLAY_STATUS) say("fake: event %d given", type);
        return;
    }
    if (!g_fake.active && !eq(h->name, "PlaySpUri") && !eq(h->name, "LaunchSpCore")) return;
    if (eq(h->name, "GetCoreInfo")) {
        uint8_t* p = outBuf(f, 0, CI_SIZE);
        if (!p) return;
        memset(p, 0, CI_SIZE);
        putStr(p, CI_SCHEME, 32, "customcore");
        putStr(p, CI_TITLE, 10, g_fake.title);
        put32At(p, CI_CONTENT, 0);  // music
        // next, previous, shuffle, repeat, playback time, duration: all ours
        for (int i = 0; i < 6; i++) put32At(p, CI_CONTENT + 4 + 4 * (size_t)i, 1);
        f->retRax = 0;
    } else if (eq(h->name, "GetPlayStatus")) {
        uint8_t* p = outBuf(f, 0, PS_SIZE);
        if (!p) return;
        memset(p, 0, PS_SIZE);
        put32At(p, 0x04, g_fake.state);  // SceMusicCorePlayStatus: 2 play, 3 pause
        put32At(p, 0x08, 100);           // play speed
        put64At(p, 0x10, g_fake.durS);
        put64At(p, 0x18, g_fake.posS);
        put32At(p, 0x24, g_fake.index);  // current track
        put32At(p, 0x28, g_fake.index);  // current play index
        put32At(p, 0x2C, g_fake.count);
        put32At(p, 0x30, g_fake.volume ? g_fake.volume : 0x8000);  // volume, 0x8000 0 dB
        put32At(p, 0x38, g_fake.repeat);   // 0 none, 1 all, 2 one
        put32At(p, 0x44, g_fake.shuffle);  // mute, auto play, BGM status, the GUID: 0
        // 0: can skip next (our player wraps round, or picks another)
        put32At(p, 0x58, (g_fake.index + 1 < g_fake.count || g_fake.repeat || g_fake.shuffle) ? 0 : 1);
        put32At(p, 0x5C, 0);                                          // can skip previous
        putStr(p, 0x60, 10, g_fake.title);
        f->retRax = 0;
    } else if (eq(h->name, "GetTrackInfo")) {
        uint8_t* p = outBuf(f, 1, TI_SIZE);
        if (!p) return;
        memset(p, 0, TI_SIZE);
        char uri[1048];
        lock(&g_fake.lock);
        fmt(uri, sizeof(uri), g_fake.file[0] == '/' ? "file://%s" : "%s", g_fake.file);
        putStr(p, TI_SRC, 1048, uri);
        putStr(p, TI_TRACK, 341, g_fake.name);
        putStr(p, TI_ALBUM, 341, g_fake.album);
        putStr(p, TI_ARTIST, 341, g_fake.artist);
        if (g_fake.cover[0]) {
            // The cover as a file of its own (our player writes it): the
            // Music section's panel loads a picture by its address alone.
            char cov[1048];
            const int web = !memcmp(g_fake.cover, "http://", 7) || !memcmp(g_fake.cover, "https://", 8);
            // A streamed track's cover by its URL, the shell fetching it (0.72).
            fmt(cov, sizeof(cov), web ? "%s" : "file://%s", g_fake.cover);
            putStr(p, TI_COVER, 1048, cov);
            put64At(p, TI_COVER_OFF, 0);
            put64At(p, TI_COVER_OFF + 8, web ? 0 : g_fake.artLen);
        } else if (g_fake.artLen) {
            putStr(p, TI_COVER, 1048, uri);
            put64At(p, TI_COVER_OFF, g_fake.artOff);
            put64At(p, TI_COVER_OFF + 8, g_fake.artLen);
        }
        unlock(&g_fake.lock);
        f->retRax = 0;
    } else if (eq(h->name, "GetSpTrackInfo")) {
        // SceMusicCoreSpTrackInfo: trackId[128], trackUri[128], contextUri[128],
        // canShare, canEditPlaylist: the playing track's id, sc<its place in
        // the queue>, which the player panel's R3 "+" (add to your music)
        // needs before it is enabled.
        uint8_t* p = outBuf(f, 0, 386);
        if (!p) return;
        memset(p, 0, 386);
        char v[64];
        fmt(v, sizeof(v), "sc%u", g_fake.index);
        putStr(p, 0, 128, v);
        fmt(v, sizeof(v), "spotify:track:sc%u", g_fake.index);
        putStr(p, 128, 128, v);
        putStr(p, 256, 128, "spotify:user:shellcompanion:playlist:queue");
        p[384] = 0;  // canShare
        p[385] = 1;  // canEditPlaylist
        f->retRax = 0;
    } else if (eq(h->name, "GetTitleId")) {
        char* p = (char*)(uintptr_t)f->gpr[0];
        const size_t cap = (size_t)f->gpr[1];
        if (cap < 10 || cap > 64 || hookReadable(p, cap) < cap || !(hookProt(p) & 2)) return;
        memset(p, 0, cap);
        putStr((uint8_t*)p, 0, cap, g_fake.title);
        f->retRax = 0;
    } else if (eq(h->name, "PlayControl")) {
        // (trackIndex, SceMusicCorePlayCmd, startTime, playSpeed). Commands: 0
        // start by time, 1 start at the current position, 2 jump, 3 stop.
        const uint32_t track = (uint32_t)f->gpr[0], cmd = (uint32_t)f->gpr[1];
        const char* send = track == TRACK_NEXT ? "next\n"
                           : track == TRACK_PREV ? "prev\n"
                           : cmd == 3            ? "pause\n"
                           : cmd <= 1            ? "play\n"
                                                 : NULL;
        say("fake: PlayControl track 0x%X, command %u, time %lu: %s", track, cmd, f->gpr[2],
            send ? send : "nothing sent\n");
        if (send) sendPlayer(send);
        owe(EV_PLAY_STATUS);
        if (track == TRACK_NEXT || track == TRACK_PREV) owe(EV_TRACK_INFO);
        f->retRax = 0;
    } else if (eq(h->name, "LaunchSpCore")) {
        // Spotify's core asked for: we are it already.
        say("fake: LaunchSpCore: we are the core");
        owe(EV_CORE_READY);
        f->retRax = 0;
    } else if (eq(h->name, "PlaySpUri")) {
        // (SceMusicCoreSpLoginInfo*, char* playUri, int trackIndex, startView,
        // shuffle): a track or Play All chosen in the Music section.
        char uri[160] = "";
        const char* u = (const char*)(uintptr_t)f->gpr[1];
        const size_t n = hookReadable(u, sizeof(uri) - 1);
        for (size_t i = 0; i < n && u[i]; i++) uri[i] = u[i], uri[i + 1] = 0;
        const int32_t index = (int32_t)f->gpr[2];
        // The playlist's id, after the uri's last ':' (queue, album<n>): our
        // player maps its index to the queue's (pick=).
        const char* id = uri;
        for (const char* q = uri; *q; q++)
            if (*q == ':') id = q + 1;
        char cmd[80];
        // The row's own track, if it was just chosen (0.77, qkChosen).
        char lib[16] = "";
        if (g_chosen.lib >= 0 && sysNowUs() - g_chosen.us < 3000000u) fmt(lib, sizeof(lib), ":%d", g_chosen.lib);
        g_chosen.lib = -1;
        fmt(cmd, sizeof(cmd), "pick=%s:%d%s\n%s", id, index < 0 ? 0 : index, lib,
            (uint32_t)f->gpr[4] ? "shuffle=1\n" : "");
        sendPlayer(cmd);
        say("fake: PlaySpUri(\"%s\", track %d, view %lu, shuffle %lu): %s", uri, index, f->gpr[3],
            f->gpr[4], cmd);
        owe(EV_TRACK_INFO);
        owe(EV_PLAY_STATUS);
        owe(EV_SP_MESSAGE);
        f->retRax = 0;
    } else if (eq(h->name, "SetAudioVolume")) {
        // (level): 0 to 0x8000, 0 dB; the quick menu's slider sends ten a second.
        uint32_t level = (uint32_t)f->gpr[0];
        if (level > 0x8000) level = 0x8000;
        char cmd[32];
        fmt(cmd, sizeof(cmd), "volume=%u\n", level);
        sendPlayer(cmd);
        g_fake.volume = level ? level : 1;  // 0 here means "not known": 1 is silent enough
        owe(EV_PLAY_STATUS);
        f->retRax = 0;
    } else if (eq(h->name, "SetShuffle") || eq(h->name, "SetRepeatMode")) {
        // SetShuffle(0 off, 1 on); SetRepeatMode(0 none, 1 all, 2 one)
        const uint32_t v = (uint32_t)f->gpr[0];
        char cmd[32];
        if (eq(h->name, "SetShuffle")) {
            fmt(cmd, sizeof(cmd), "shuffle=%u\n", v ? 1u : 0u);
            g_fake.shuffle = v ? 1 : 0;
        } else {
            fmt(cmd, sizeof(cmd), "repeat=%u\n", v > 2 ? 0u : v);
            g_fake.repeat = v > 2 ? 0 : v;
        }
        say("fake: %s(%u)", h->name, v);
        sendPlayer(cmd);
        owe(EV_PLAY_STATUS);
        f->retRax = 0;
    }
}

// ── The keys the shell's JSON is asked for (an experiment) ───────────────────
//
// The Music section reads the answers above with Sce.PlayStation.Json's
// JsonObject, managed code, precompiled (full AOT) into its .dll.sprx. Mono's
// own API, in the shell (libmonosgen-2.0), gives that code's address
// (mono_compile_method); those methods are hooked like the rest, and while an
// answer is being read (g_jsonUntil) each key asked for is logged: the
// answers' format, from what the shell looks for.

static void hex16(char* out, size_t cap, const uint8_t* p);

typedef void* (*FnMonoVoid)(void);
typedef void* (*FnMonoPtr)(void*);
typedef void* (*FnMonoName)(const char*);
typedef void* (*FnMonoClass)(void*, const char*, const char*);
typedef void* (*FnMonoMethod)(void*, const char*, int);
typedef void (*FnMonoDetach)(void*);

// A MonoString (64-bit: vtable, sync, int32 length, UTF-16), as ASCII.
static void monoText(uint64_t p, char* out, size_t cap) {
    out[0] = 0;
    const uint8_t* s = (const uint8_t*)(uintptr_t)p;
    if (hookReadable(s, 20) < 20) {
        fmt(out, cap, "(not a string: %lx)", p);
        return;
    }
    int32_t len;
    memcpy(&len, s + 16, 4);
    if (len < 0 || len > 8192) {  // what fits in out is kept (0.65: was 200)
        fmt(out, cap, "(length %d)", len);
        return;
    }
    const size_t have = hookReadable(s + 20, (size_t)len * 2) / 2;
    size_t o = 0;
    for (size_t i = 0; i < have && o + 1 < cap; i++) {
        uint16_t c;
        memcpy(&c, s + 20 + i * 2, 2);
        out[o++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
    out[o] = 0;
}

static void jsonLog(Frame* f) {
    if (sysNowUs() > g_jsonUntil) return;
    char key[100];
    monoText(f->gpr[1], key, sizeof(key));
    const Hook* h = f->hook;
    if (eq(h->name, "json.ContainsKey") || eq(h->name, "json.TryGetValue"))
        say("json: %s(\"%s\") = %s  [object %lx, thread %d]", h->name + 5, key,
            (f->retRax & 0xFF) ? "yes" : "no", f->gpr[0], scePthreadGetthreadid());
    else
        say("json: %s(\"%s\") = %lx  [object %lx, thread %d]", h->name + 5, key, f->retRax,
            f->gpr[0], scePthreadGetthreadid());
}

// Managed methods hooked through Mono: JsonObject's key lookups (1, their
// keys logged while an answer is read); the Music section's check that
// Spotify's app is there and recent enough (2, answered true while ebgm/ is
// there: Spotify is not installed, and the section offered to download it);
// the version it found (3, logged); the panels told of it (4, never of a
// version below APPVER_SAID while ebgm/ is there).
#define MONO_HOOKS 30
static Hook g_monoHooks[MONO_HOOKS];
static int g_forceApp;  // ebgm/ there: PlayStationMusicContext.IsValidAppVer answered true
#define APPVER_AT 0xB8      // PlayStationMusicContext's m_appVer (a float)
#define APPVER_SAID 9.99f   // what it is made when lower: Spotify's app there, and recent

// ── The Music section's "Spotify installed?" (0.48, an experiment) ───────────
//
// The section's panel shows "Download Spotify" while the user's Spotify app
// entry (UserContext.SpotifyAppBrowseItemData, made for CUSA01780) holds no
// AppBrowseItem: none is installed. In IsValidAppVer's hook, on the shell's
// own thread, the entry of the app in front (BigAppBrowseItemData's, the test
// app's) is put in Spotify's through Mono (mono_field_set_value, its write
// barrier and all), and Spotify's entry told (NotifyAppBrowseItemChanged,
// mono_runtime_invoke), as when an app is installed.

typedef void* (*FnMonoGetClass)(void*);
typedef void* (*FnMonoFieldByName)(void*, const char*);
typedef void (*FnMonoFieldValue)(void*, void*, void*);
typedef void* (*FnMonoFields)(void*, void**);
typedef const char* (*FnMonoFieldName)(void*);
typedef void* (*FnMonoInvoke)(void*, void*, void**, void**);
static struct {
    FnMonoGetClass objectClass;
    FnMonoFieldByName fieldByName;
    FnMonoFieldValue get, set;
    FnMonoFields fields;
    FnMonoFieldName fieldName;
    FnMonoMethod methodByName;
    FnMonoInvoke invoke;
} g_mono;
static int g_fixSaid;

// A field of klass named name, else the first whose name has part in it
// (an auto-property's backing field, <Name>k__BackingField).
static void* monoField(void* klass, const char* name, const char* part) {
    void* f = g_mono.fieldByName(klass, name);
    if (f || !part || !g_mono.fields || !g_mono.fieldName) return f;
    void* iter = NULL;
    void* each;
    while ((each = g_mono.fields(klass, &iter))) {
        const char* n = g_mono.fieldName(each);
        if (n && contains(n, part)) return each;
    }
    return NULL;
}

static void* monoGet(void* obj, void* field) {
    void* v = NULL;
    if (obj && field) g_mono.get(obj, field, &v);
    return v;
}

static void spotifyEntryFix(void* ctx) {
    if (!g_mono.objectClass || !g_mono.fieldByName || !g_mono.get || !g_mono.set || !ctx) return;
    const int say_ = g_fixSaid < 12;
    void* user = monoGet(ctx, monoField(g_mono.objectClass(ctx), "m_userContext", "userContext"));
    if (!user) {
        if (say_) say("mono: spotify entry: no m_userContext");
        g_fixSaid++;
        return;
    }
    void* uc = g_mono.objectClass(user);
    void* sp = monoGet(user, monoField(uc, "<SpotifyAppBrowseItemData>k__BackingField", "SpotifyAppBrowse"));
    void* big = monoGet(user, monoField(uc, "<BigAppBrowseItemData>k__BackingField", "BigAppBrowse"));
    if (!sp || !big) {
        if (say_) say("mono: spotify entry: user %p, Spotify's %p, the app in front's %p", user, sp, big);
        g_fixSaid++;
        return;
    }
    void* dc = g_mono.objectClass(sp);
    void* itemField = monoField(dc, "<AppBrowseItem>k__BackingField", "AppBrowseItem");
    void* spItem = monoGet(sp, itemField);
    void* bigItem = monoGet(big, itemField);
    if (spItem || !bigItem) {
        if (say_)
            say("mono: spotify entry: Spotify's holds %p, the app in front's %p: %s", spItem, bigItem,
                spItem ? "left" : "nothing to give it");
        g_fixSaid++;
        return;
    }
    g_mono.set(sp, itemField, &bigItem);
    void* notify = g_mono.methodByName ? g_mono.methodByName(dc, "NotifyAppBrowseItemChanged", 0) : NULL;
    void* exc = NULL;
    if (notify && g_mono.invoke) g_mono.invoke(notify, sp, NULL, &exc);
    if (say_)
        say("mono: spotify entry: Spotify's (%p) given the app in front's item %p; told: %s%s", sp,
            bigItem, notify ? "yes" : "no method", exc ? ", it threw" : "");
    g_fixSaid++;
}

// ── Pictures on a track list's rows (0.62, an experiment) ────────────────────
//
// A track's row (TrackListPanelItem) is two labels, no picture. After its
// UpdateData(menuData), on the shell's UI thread, the row is given an
// ImageBox of its own (Sce.PlayStation.PUI.UI2, made and kept through Mono),
// 64 square at its left, loaded with the track's cover (ebgm/trackart<n>.jpg,
// n from the track's uri, spotify:track:sc<n>, as our player writes them),
// and its two labels moved right to make room; hidden again, the labels put
// back, for a row with no cover or that is not a track. Rows are reused as
// the list scrolls: each is known by a weak GC handle, never a pointer.
// mcw-norowart turns it off.

#define ROWS 256
#define ROW_SHIFT 74.0f
typedef void* (*FnMonoNew)(void*, void*);
typedef void* (*FnMonoStrNew)(void*, const char*);
typedef void* (*FnMonoToString)(void*, void**);
typedef void* (*FnMonoUnbox)(void*);
typedef uint32_t (*FnMonoWeak)(void*, int);
typedef void* (*FnMonoTarget)(uint32_t);
typedef void* (*FnMonoVirtual)(void*, void*);
typedef int (*FnMonoOffset)(void*);
static struct {
    int ready;
    void* domain;
    FnMonoNew objectNew;
    FnMonoStrNew stringNew;
    FnMonoToString toString;
    FnMonoUnbox unbox;
    FnMonoWeak weak;
    FnMonoTarget target;
    FnMonoVirtual virt;
    void *imageBox, *ctor, *loadAsync;          // ImageBox
    void *scaleType, *getImage;                  // ImageBox
    void *options, *optCtor, *optConvert;        // ImageOptions
    void *fFormat, *fResize, *fUpdated;          // its fields (0.63)
    void *setCache, *globalCache;                // its Cache, ImageObject.GlobalCache (0.65)
    void *getManager, *setManager;               // its AssetManager (0.66)
    FnMonoWeak strong;                           // mono_gchandle_new
    void (*freeHandle)(uint32_t);                // mono_gchandle_free
    uint32_t manager;                            // the shell's own loads' AssetManager
    void* managerName;                           // AssetManager.name
    int ourLoad;                                 // in our own LoadAsync call
    void *setX, *setY, *setW, *setH, *getX, *setVisible;  // UINode
    void* appendChild;                           // Widget
    void* removeChild;                           // Widget.RemoveFromParent (0.75)
    void* jsonItem;                              // JsonObject.get_Item
    void *fType, *fData;                         // TrackListMenuData
    void *fText, *fSub;                          // TrackListPanelItem's labels
} g_row;
static struct {
    uint32_t row, img;  // weak handles
    int idx, shifted, rev;
    int loaded;         // a load was ever begun into its box
    uint32_t used;      // when it was last updated (g_rowClock), for reusing it
    float x[2];         // the labels' own X, before they were moved
} g_rows[ROWS];
static int g_rowSaid;

static void* rowCall(void* method, void* obj, void** args) {
    void* exc = NULL;
    // An override's, if obj's class has one (UINode.Y is virtual).
    if (obj && g_row.virt) {
        void* v = g_row.virt(obj, method);
        if (v) method = v;
    }
    void* r = g_mono.invoke(method, obj, args, &exc);
    if (exc && g_rowSaid < 20) {
        say("rowart: a call threw");
        g_rowSaid++;
    }
    return exc ? NULL : r;
}

static void rowFloat(void* method, void* obj, float v) {
    void* args[1] = {&v};
    rowCall(method, obj, args);
}

// Load options as the shell's own cover box makes them (MusicPlayerPanel.
// CoverArtLoadAsync): decoded to a Dxt5 texture by the GPU, resized to the
// box. 0.62 loaded with none, and showed nothing.
static void* rowOptions(void) {
    if (!g_row.options) return NULL;
    void* o = g_row.objectNew(g_row.domain, g_row.options);
    if (!o) return NULL;
    rowCall(g_row.optCtor, o, NULL);
    if (g_row.optConvert) {
        int conv = 4;  // ImageConvertOption.DXTCompressByGPU
        void* args[1] = {&conv};
        rowCall(g_row.optConvert, o, args);
    }
    // Nullable<T> as Mono lays it out: the value, then has_value.
    if (g_row.fFormat) {
        uint32_t fmt_[2] = {4, 1};  // ImageFormat.Dxt5
        g_mono.set(o, g_row.fFormat, fmt_);
    }
    if (g_row.fResize) {
        int32_t size[3] = {64, 64, 1};
        g_mono.set(o, g_row.fResize, size);
    }
    if (g_row.fUpdated) {
        uint8_t yes = 1;
        g_mono.set(o, g_row.fUpdated, &yes);
    }
    // The AssetManager the shell's own loads use: 0.65's, the default one,
    // failed "AssetManager(SceUIDefaultAssetManager) is out of memory".
    if (g_row.manager && g_row.setManager) {
        void* mgr = g_row.target(g_row.manager);
        if (mgr) {
            void* args[1] = {mgr};
            rowCall(g_row.setManager, o, args);
        }
    }
    // The shell gives every load a cache; 0.63's had none, and failed.
    if (g_row.setCache && g_row.globalCache) {
        void* cache = rowCall(g_row.globalCache, NULL, NULL);
        if (cache) {
            void* args[1] = {cache};
            rowCall(g_row.setCache, o, args);
        }
        static int said;
        if (!said++) say("rowart: the loads' cache: %p", cache);
    }
    return o;
}

static void rowShow(void* img, int on) {
    int v = on;
    void* args[1] = {&v};
    rowCall(g_row.setVisible, img, args);
}

// The labels' X: read (keep), then set to their own plus dx, absolutely, so
// a row the shell laid out again is not moved twice.
static void rowShift(void* row, int e, int keep, float dx) {
    void* labels[2] = {monoGet(row, g_row.fText), monoGet(row, g_row.fSub)};
    for (int k = 0; k < 2; k++) {
        if (!labels[k]) continue;
        if (keep) {
            void* boxed = rowCall(g_row.getX, labels[k], NULL);
            if (!boxed) continue;
            memcpy(&g_rows[e].x[k], g_row.unbox(boxed), 4);
        }
        rowFloat(g_row.setX, labels[k], g_rows[e].x[k] + dx);
    }
}

// The queue's keys' state (0.67, below: qkKey).
#define KEY_SQUARE 7
#define KEY_TRIANGLE 8
static struct {
    int ready;
    uint32_t panel;             // the TrackListPanel last seen (weak)
    uint32_t moving;            // the ListPanel in move mode, ours (weak)
    int moveOffset;             // its focus index less the track's place
    int startRow, lastRow;      // where the track was picked up, and is now (0.69)
    int cancelled;              // ○: put back
    int moved;                  // moves sent in this move mode
    void *fKey, *fKeyEvent, *fHandled;           // KeyEventArgs
    void *fListPanel, *fPlayList, *fTrackIndex;  // TrackListPanelItem, TrackListPanel, TrackListMenuData
    void *listFocused, *menuData, *startQuery;   // TrackListPanel
    void* getList;                               // LoadingListPanel.get_ListPanel
    void *focusIndex, *isMoving, *beginMove, *withTarget, *endOnBack, *decide;  // ListPanelBase
    void *moveTarget, *origTarget;               // its MoveTargetIndex, OrigMoveTargetIndex (0.68)
    void* endMove;                               // its EndItemMoveMode (0.69)
    void* selected;                              // its isMoveTargetSelected (0.71)
} g_qk;

// The track's place in the queue, from its uri (spotify:track:sc<n>): -1 if none.
static int rowIndex(void* data) {
    if (!data || !g_row.jsonItem) return -1;
    void* key = g_row.stringNew(g_row.domain, "uri");
    void* args[1] = {key};
    void* v = rowCall(g_row.jsonItem, data, args);
    if (!v) return -1;
    void* exc = NULL;
    void* str = g_row.toString(v, &exc);
    if (!str || exc) return -1;
    char text[80];
    monoText((uint64_t)(uintptr_t)str, text, sizeof(text));
    const char* p = text;
    for (const char* q = text; *q; q++)
        if (q[0] == 's' && q[1] == 'c' && q[2] >= '0' && q[2] <= '9') p = q + 2;
    if (p == text) return -1;
    int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    return n;
}

// The player's covers for rows, read whole when its library changes.
static struct {
    int rev, lines;
    char* text;
    size_t mapped;
    uint32_t* at;  // each line's start
} g_rowMap = {-1, 0, NULL, 0, NULL};

// Track idx's cover into out: 1 if it has one.
static int rowCover(int idx, int rev, char* out, size_t cap) {
    out[0] = 0;
    if (rev != g_rowMap.rev) {
        if (g_rowMap.text) sysUnmap(g_rowMap.text, g_rowMap.mapped);
        g_rowMap.text = NULL;
        g_rowMap.lines = 0;
        g_rowMap.rev = rev;
        char path[128];
        fmt(path, sizeof(path), EBGM_DIR "/rowart-%d.txt", rev);
        const int fd = sysOpen(path, SC_O_RDONLY, 0);
        if (fd >= 0) {
            const int64_t size = sysSeek(fd, 0, 2);
            sysSeek(fd, 0, 0);
            if (size > 0 && size < (int64_t)(32u << 20)) {
                g_rowMap.mapped = ((size_t)size + 1 + 0x3FFF) & ~(size_t)0x3FFF;
                g_rowMap.text = (char*)sysMap(g_rowMap.mapped);
                int64_t got = 0;
                while (g_rowMap.text && got < size) {
                    const int k = sysRead(fd, g_rowMap.text + got, (size_t)(size - got));
                    if (k <= 0) break;
                    got += k;
                }
                if (g_rowMap.text) g_rowMap.text[got] = 0;
            }
            sysClose(fd);
        }
        if (!g_rowMap.at) g_rowMap.at = (uint32_t*)sysMap(10000 * sizeof(uint32_t) + 0x4000);
        if (g_rowMap.text && g_rowMap.at) {
            uint32_t p = 0;
            while (g_rowMap.text[p] && g_rowMap.lines < 10000) {
                g_rowMap.at[g_rowMap.lines++] = p;
                while (g_rowMap.text[p] && g_rowMap.text[p] != '\n') p++;
                if (g_rowMap.text[p]) p++;
            }
        }
        say("rowart: covers of library %d: %d track(s)", rev, g_rowMap.lines);
    }
    if (idx < 0 || idx >= g_rowMap.lines) return 0;
    const char* p = g_rowMap.text + g_rowMap.at[idx];
    size_t k = 0;
    while (p[k] && p[k] != '\n' && k + 1 < cap) {
        out[k] = p[k];
        k++;
    }
    out[k] = 0;
    return k > 0;
}

// A new ImageBox on the row, 64 square at its left.
static void* rowNewBox(void* row) {
    void* img = g_row.objectNew(g_row.domain, g_row.imageBox);
    if (!img) return NULL;
    rowCall(g_row.ctor, img, NULL);
    rowFloat(g_row.setX, img, 10.0f);
    rowFloat(g_row.setY, img, 10.0f);
    rowFloat(g_row.setW, img, 64.0f);
    rowFloat(g_row.setH, img, 64.0f);
    if (g_row.scaleType) {
        int st = 2;  // ImageScaleType.AspectInside
        void* sargs[1] = {&st};
        rowCall(g_row.scaleType, img, sargs);
    }
    void* args[1] = {img};
    rowCall(g_row.appendChild, row, args);
    return img;
}

static void rowArt(Frame* f) {
    if (!g_row.ready) return;
    void* row = (void*)(uintptr_t)f->gpr[0];
    void* data = (void*)(uintptr_t)f->gpr[1];
    if (!row || !data) return;
    g_uiThread = scePthreadGetthreadid();
    // Its list, for the queue's keys (0.67).
    if (g_qk.fListPanel) {
        void* panel = monoGet(row, g_qk.fListPanel);
        if (panel && (!g_qk.panel || g_row.target(g_qk.panel) != panel)) {
            if (g_qk.panel && g_row.freeHandle) g_row.freeHandle(g_qk.panel);
            g_qk.panel = g_row.weak(panel, 0);
        }
    }
    // This row's entry, or a free one.
    // (0.83) Full, as the shell keeps the rows of lists opened before (0.82
    // and before then gave new rows no cover at all): the entry updated
    // longest ago is taken, its row's box removed and its labels put back.
    static uint32_t rowClock;
    int e = -1, free_ = -1, oldest = -1;
    for (int i = 0; i < ROWS; i++) {
        void* t = g_rows[i].row ? g_row.target(g_rows[i].row) : NULL;
        if (t == row) e = i;
        else if (!t && free_ < 0) free_ = i;
        if (t && t != row && (oldest < 0 || g_rows[i].used < g_rows[oldest].used)) oldest = i;
    }
    if (e < 0 && free_ < 0 && oldest >= 0) {
        void* oldRow = g_row.target(g_rows[oldest].row);
        void* oldImg = g_rows[oldest].img ? g_row.target(g_rows[oldest].img) : NULL;
        if (oldImg) {
            rowShow(oldImg, 0);
            if (g_row.removeChild) rowCall(g_row.removeChild, oldImg, NULL);
        }
        if (oldRow && g_rows[oldest].shifted) rowShift(oldRow, oldest, 0, 0.0f);
        if (g_row.freeHandle) {
            if (g_rows[oldest].row) g_row.freeHandle(g_rows[oldest].row);
            if (g_rows[oldest].img) g_row.freeHandle(g_rows[oldest].img);
        }
        memset(&g_rows[oldest], 0, sizeof(g_rows[oldest]));
        free_ = oldest;
    }
    int type = -1;
    if (g_row.fType) g_mono.get(data, g_row.fType, &type);
    void* json = monoGet(data, g_row.fData);
    const int idx = type == 0 ? rowIndex(json) : -1;
    // The track's cover, from the player's ebgm/rowart-<rev>.txt (0.84: a
    // file a track before): a file:// URL of a cover in ebgm/art/, a URL,
    // or nothing.
    const int rev = statusInt("art_rev");
    static char webCover[1048];
    const int has = idx >= 0 && rowCover(idx, rev, webCover, sizeof(webCover));
    if (e < 0 && !has) return;  // nothing to show, nothing shown
    if (e < 0) {
        if (free_ < 0) return;
        void* img = rowNewBox(row);
        if (!img) return;
        e = free_;
        g_rows[e].row = g_row.weak(row, 0);
        g_rows[e].img = g_row.weak(img, 0);
        g_rows[e].idx = -1;
        g_rows[e].shifted = 0;
        g_rows[e].loaded = 0;
    }
    g_rows[e].used = ++rowClock;
    void* img = g_row.target(g_rows[e].img);
    if (!img) {
        g_rows[e].row = 0;
        return;
    }
    if (has) {
        if (g_rows[e].idx != idx || g_rows[e].rev != rev) {
            // A box of its own for each cover (0.75): a load still coming
            // into the row's earlier box (the list reuses rows as it
            // scrolls) put an earlier track's cover on this one. The earlier
            // box is taken off the row.
            // (Any box a load was begun into, 0.76: 0.75 kept one whose row
            // had held a track with no cover meanwhile, its load still coming.)
            if (g_rows[e].loaded && g_row.removeChild) {
                rowShow(img, 0);
                rowCall(g_row.removeChild, img, NULL);
                void* fresh = rowNewBox(row);
                if (fresh) {
                    if (g_row.freeHandle) g_row.freeHandle(g_rows[e].img);
                    g_rows[e].img = g_row.weak(fresh, 0);
                    img = fresh;
                }
            }
            static char url[1100];
            fmt(url, sizeof(url), "%s", webCover);
            void* args[3] = {g_row.stringNew(g_row.domain, url), NULL, rowOptions()};
            g_row.ourLoad = 1;
            rowCall(g_row.loadAsync, img, args);
            g_row.ourLoad = 0;
            g_rows[e].idx = idx;
            g_rows[e].rev = rev;
            g_rows[e].loaded = 1;
        }
        rowShow(img, 1);
        rowShift(row, e, !g_rows[e].shifted, ROW_SHIFT);
        g_rows[e].shifted = 1;
    } else {
        rowShow(img, 0);
        g_rows[e].idx = -1;
        if (g_rows[e].shifted) rowShift(row, e, 0, 0.0f);
        g_rows[e].shifted = 0;
    }
    if (g_rowSaid < 30 && has && g_row.manager) {
        char name[80];
        name[0] = 0;
        void* mgr = g_row.target(g_row.manager);
        void* n = mgr && g_row.managerName ? monoGet(mgr, g_row.managerName) : NULL;
        if (n) monoText((uint64_t)(uintptr_t)n, name, sizeof(name));
        say("rowart: its load given AssetManager %p \"%s\"", mgr, name);
    }
    if (g_rowSaid < 30) {
        // Whether the picture loaded so far (it loads in the background: a
        // row shown again says what came of its earlier load).
        void* asset = g_row.getImage ? rowCall(g_row.getImage, img, NULL) : NULL;
        say("rowart: row %p, track %d: %s; its picture %s", row, idx, has ? "its cover" : "none",
            asset ? "loaded" : "not (yet) loaded");
        g_rowSaid++;
    }
}

// An ImageBox's load finished (its HandleLoadCompleted(args, action,
// setImage), after): ours, said with the shell's own error if it failed, and
// the shell's own first few, to compare (0.64).
static void rowLoaded(Frame* f) {
    if (!g_row.target) return;
    void* box = (void*)(uintptr_t)f->gpr[0];
    void* args = (void*)(uintptr_t)f->gpr[1];
    int ours = -1;
    for (int i = 0; i < ROWS; i++)
        if (g_rows[i].img && g_row.target(g_rows[i].img) == box) ours = i;
    static int others;
    if (ours < 0 && others >= 12) return;
    if (ours < 0) others++;
    char err[1400];
    err[0] = 0;
    void* exc = NULL;
    if (args && g_mono.objectClass && g_mono.fieldByName) {
        void* fe = g_mono.fieldByName(g_mono.objectClass(args), "error");
        exc = fe ? monoGet(args, fe) : NULL;
        if (exc) {
            void* e2 = NULL;
            void* str = g_row.toString(exc, &e2);
            if (str && !e2) monoText((uint64_t)(uintptr_t)str, err, sizeof(err));
            else fmt(err, sizeof(err), "(an error, its text not read)");
        }
    }
    void* asset = g_row.getImage ? rowCall(g_row.getImage, box, NULL) : NULL;
    say("rowart: %s box %p (row entry %d) loaded: %s; its Image %s", ours >= 0 ? "OUR" : "shell's",
        box, ours, exc ? "FAILED" : "ok", asset ? "set" : "empty");
    // The error, in pieces a log line holds; once per text seen.
    static int errSaid;
    if (err[0] && errSaid < 3) {
        errSaid++;
        const size_t n = strlen(err);
        for (size_t at = 0; at < n; at += 200) {
            char part[204];
            size_t k = 0;
            while (k < 200 && at + k < n) {
                part[k] = err[at + k];
                k++;
            }
            part[k] = 0;
            say("rowart:   error %s", part);
        }
    }
}

// The shell's own ImageBox.LoadAsync(uri, action, options), before: its
// options' AssetManager kept, the first seen, for our loads (0.66).
static void rowShellLoad(Frame* f) {
    void* opt = (void*)(uintptr_t)f->gpr[3];
    static int said;
    if (g_row.ourLoad || !opt || !g_row.getManager || !g_row.strong) return;
    void* mgr = rowCall(g_row.getManager, opt, NULL);
    char uri[160], name[80];
    monoText(f->gpr[1], uri, sizeof(uri));
    name[0] = 0;
    if (mgr && g_row.managerName) {
        void* n = monoGet(mgr, g_row.managerName);
        if (n) monoText((uint64_t)(uintptr_t)n, name, sizeof(name));
    }
    if (said < 12) {
        said++;
        say("rowart: shell's LoadAsync(\"%s\"): AssetManager %p \"%s\"", uri, mgr, name);
    }
    // The latest kept: when a track list opens, the Music section's own.
    if (!mgr || (g_row.manager && g_row.target(g_row.manager) == mgr)) return;
    if (g_row.manager && g_row.freeHandle) g_row.freeHandle(g_row.manager);
    g_row.manager = g_row.strong(mgr, 0);
}

// ── The queue edited in the Music section (0.67) ────────────────────────────
//
// Buttons on a track list's focused track, taken in the Music panel's own
// global key handler (ControlPanel.OnGlobalKeyEventReceived, before it: the
// key then marked handled, so □ does not also open the volume, nor △ pause):
//   in the Queue:   □ picks the track up (the list's own move mode, as the
//                   home screen moves a tile): up and down move it, X or □
//                   puts it down, ○ puts it back; △ takes it out.
//   in other lists: □ puts the track next (after the one playing), △ last.
// The player is told (move=, remove=, next=, add=: ps4/bgm); after a move or
// a removal the list asks for the queue again (TrackListPanel.StartQuery),
// the asking held until the player has written it (editsWait).
// mcw-noqueuekeys turns it off.


static int qkInt(void* obj, void* field) {
    int v = 0;
    if (obj && field) g_mono.get(obj, field, &v);
    return v;
}

static int qkBool(void* method, void* obj) {
    void* boxed = rowCall(method, obj, NULL);
    return boxed ? *(uint8_t*)g_row.unbox(boxed) != 0 : 0;
}

// The list's id: its uri's end (queue, all, liked, album<n>).
static void qkListId(void* panel, char* out, size_t cap) {
    out[0] = 0;
    void* data = monoGet(panel, g_qk.fPlayList);
    if (!data) return;
    void* args[1] = {g_row.stringNew(g_row.domain, "uri")};
    void* v = rowCall(g_row.jsonItem, data, args);
    void* exc = NULL;
    void* str = v ? g_row.toString(v, &exc) : NULL;
    if (!str || exc) return;
    char text[160];
    monoText((uint64_t)(uintptr_t)str, text, sizeof(text));
    const char* id = text;
    for (const char* q = text; *q; q++)
        if (*q == ':') id = q + 1;
    size_t k = 0;
    for (; id[k] && id[k] != '"' && k + 1 < cap; k++) out[k] = id[k];
    out[k] = 0;
}

static void qkRequery(void* panel) {
    if (panel) rowCall(g_qk.startQuery, panel, NULL);
}

// ControlPanel.OnGlobalKeyEventReceived(sender, args), before it.
static void qkKey(Frame* f) {
    if (!g_qk.ready) return;
    void* args = (void*)(uintptr_t)f->gpr[2];
    if (!args) return;
    uint8_t handled = 0;
    g_mono.get(args, g_qk.fHandled, &handled);
    const int key = qkInt(args, g_qk.fKey), type = qkInt(args, g_qk.fKeyEvent);
    if (handled || type != 1 /* Down */ || (key != KEY_SQUARE && key != KEY_TRIANGLE)) return;
    void* panel = g_qk.panel ? g_row.target(g_qk.panel) : NULL;
    if (!panel || !qkBool(g_qk.listFocused, panel)) return;
    void* list = rowCall(g_qk.getList, panel, NULL);
    if (!list) return;
    uint8_t yes = 1;
    if (qkBool(g_qk.isMoving, list)) {
        // □ puts the track down; △ does nothing meanwhile.
        // □: puts the held track down (as X), else ends the mode (as ○).
        if (key == KEY_SQUARE) {
            const int held = g_qk.selected ? qkBool(g_qk.selected, list) : 0;
            rowCall(held ? g_qk.decide : g_qk.endMove, list, NULL);
        }
        g_mono.set(args, g_qk.fHandled, &yes);
        return;
    }
    void* boxed = rowCall(g_qk.focusIndex, list, NULL);
    if (!boxed) return;
    int at = *(int*)g_row.unbox(boxed);
    void* margs[1] = {&at};
    void* md = rowCall(g_qk.menuData, panel, margs);
    if (!md || qkInt(md, g_row.fType) != 0 /* a track */) return;
    const int place = qkInt(md, g_qk.fTrackIndex);
    const int lib = rowIndex(monoGet(md, g_row.fData));
    char id[48], cmd[48];
    qkListId(panel, id, sizeof(id));
    if (eq(id, "queue")) {
        if (key == KEY_SQUARE) {
            void* targs[1] = {&yes};
            rowCall(g_qk.withTarget, list, targs);
            rowCall(g_qk.endOnBack, list, targs);
            rowCall(g_qk.beginMove, list, NULL);
            if (g_qk.moving && g_row.freeHandle) g_row.freeHandle(g_qk.moving);
            g_qk.moving = g_row.weak(list, 0);
            g_qk.moveOffset = at - place;
            g_qk.startRow = g_qk.lastRow = at;
            g_qk.cancelled = 0;
            g_qk.moved = 0;
            say("queue: track %d (row %d) picked up", place, at);
        } else {
            fmt(cmd, sizeof(cmd), "remove=%d\n", place);
            sendEdit(cmd);
            qkRequery(panel);
            say("queue: track %d taken out", place);
        }
    } else if (lib >= 0) {
        fmt(cmd, sizeof(cmd), "%s=%d\n", key == KEY_SQUARE ? "next" : "add", lib);
        sendEdit(cmd);
        say("queue: from %s, track %d (library %d): %s", id, place, lib, cmd);
    }
    g_mono.set(args, g_qk.fHandled, &yes);
}

static int qkIndex(void* method, void* list) {
    void* boxed = method ? rowCall(method, list, NULL) : NULL;
    return boxed ? *(int*)g_row.unbox(boxed) : -1;
}

// TrackListMenuPanel.OnItemSelected(menuData), before: the row's own track,
// for PlaySpUri's pick (the list's place alone played another song when the
// list the shell showed was older than the player's).
static void qkChosen(Frame* f) {
    void* md = (void*)(uintptr_t)f->gpr[1];
    g_chosen.lib = -1;
    g_chosen.us = sysNowUs();
    if (!md || !g_row.ready || !g_row.fType) return;
    if (qkInt(md, g_row.fType) == 0) g_chosen.lib = rowIndex(monoGet(md, g_row.fData));
    say("queue: row chosen: track %d", g_chosen.lib);
}

// ListPanelBase.OnItemMoved(from, to), after: logged only. 0.67 took each
// for a move of the queue, but after the first step they read (-1, row):
// the held track is out of the list while it moves.
static void qkMoved(Frame* f) {
    void* list = (void*)(uintptr_t)f->gpr[0];
    if (!g_qk.moving || g_row.target(g_qk.moving) != list) return;
    // A real move reads (row, row): the track dropped (X) at its new row,
    // sent now (0.71). (-1, row) is not one: nothing was held.
    const int fromRow = (int)f->gpr[1], toRow = (int)f->gpr[2];
    const int from = fromRow - g_qk.moveOffset, to = toRow - g_qk.moveOffset;
    say("queue: OnItemMoved(%d, %d): places %d to %d", fromRow, toRow, from, to);
    if (fromRow >= 0 && toRow >= 0 && from >= 0 && from != to) {
        char cmd[48];
        fmt(cmd, sizeof(cmd), "move=%d:%d\n", from, to < 0 ? 0 : to);
        sendEdit(cmd);
        g_qk.moved++;
    }
}

// ListPanelBase.DecideMoveItem(), before: called for each step of a move
// (0.68 took it for the put-down: after the first step its rows read -1).
// Logged only; RollbackMoveTarget (○) marks the move put back.
static void qkDecide(Frame* f) {
    void* list = (void*)(uintptr_t)f->gpr[0];
    if (!g_qk.moving || g_row.target(g_qk.moving) != list) return;
    if (f->hook->json == 15) {
        g_qk.cancelled = 1;
        say("queue: move put back (RollbackMoveTarget)");
        return;
    }
    say("queue: DecideMoveItem: target row %d, from row %d", qkIndex(g_qk.moveTarget, list),
        qkIndex(g_qk.origTarget, list));
}

// ListPanelBase.EndItemMoveMode(), after: our move done; the queue again.
static void qkMoveEnded(Frame* f) {
    void* list = (void*)(uintptr_t)f->gpr[0];
    if (!g_qk.moving || g_row.target(g_qk.moving) != list) return;
    if (g_row.freeHandle) g_row.freeHandle(g_qk.moving);
    g_qk.moving = 0;
    say("queue: move mode ended, %d move(s) sent", g_qk.moved);
    qkRequery(g_qk.panel ? g_row.target(g_qk.panel) : NULL);
}

static void qkSetup(void* pui, void* app, FnMonoClass cls) {
    if (sysExists(SC_BGM_DIR "/mcw-noqueuekeys")) {
        say("queue: keys off (mcw-noqueuekeys)");
        return;
    }
    FnMonoMethod m = g_mono.methodByName;
    void* kea = pui ? cls(pui, "Sce.PlayStation.PUI", "KeyEventArgs") : NULL;
    void* lpb = pui ? cls(pui, "Sce.PlayStation.PUI.UI2", "ListPanelBase") : NULL;
    void* llp = app ? cls(app, "Sce.Vsh.ShellUI.QuickMenu", "LoadingListPanel") : NULL;
    void* tlp = app ? cls(app, "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListPanel") : NULL;
    void* item = app ? cls(app, "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListPanelItem") : NULL;
    void* data = app ? cls(app, "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListMenuData") : NULL;
    if (kea) {
        g_qk.fKey = g_mono.fieldByName(kea, "<KeyType>k__BackingField");
        g_qk.fKeyEvent = g_mono.fieldByName(kea, "<KeyEventType>k__BackingField");
        g_qk.fHandled = g_mono.fieldByName(kea, "handled");
    }
    if (lpb) {
        g_qk.focusIndex = m(lpb, "get_FocusIndex", 0);
        g_qk.isMoving = m(lpb, "get_IsItemMoveMode", 0);
        g_qk.beginMove = m(lpb, "BeginItemMoveMode", 0);
        g_qk.withTarget = m(lpb, "set_BeginItemMoveModeWithTargetSelected", 1);
        g_qk.endOnBack = m(lpb, "set_EndItemMoveModeOnBackButton", 1);
        g_qk.decide = m(lpb, "DecideMoveItem", 0);
        g_qk.moveTarget = m(lpb, "get_MoveTargetIndex", 0);
        g_qk.origTarget = m(lpb, "get_OrigMoveTargetIndex", 0);
        g_qk.endMove = m(lpb, "EndItemMoveMode", 0);
        g_qk.selected = m(lpb, "get_isMoveTargetSelected", 0);
    }
    if (llp) g_qk.getList = m(llp, "get_ListPanel", 0);
    if (tlp) {
        g_qk.listFocused = m(tlp, "get_ListFocused", 0);
        g_qk.menuData = m(tlp, "GetTrackListMenuData", 1);
        g_qk.startQuery = m(tlp, "StartQuery", 0);
        g_qk.fPlayList = g_mono.fieldByName(tlp, "m_playListData");
    }
    if (item) g_qk.fListPanel = g_mono.fieldByName(item, "m_listPanel");
    if (data) g_qk.fTrackIndex = g_mono.fieldByName(data, "TrackIndex");
    g_qk.ready = g_row.ready && g_mono.set && g_qk.fKey && g_qk.fKeyEvent && g_qk.fHandled &&
                 g_qk.focusIndex && g_qk.isMoving && g_qk.beginMove && g_qk.withTarget &&
                 g_qk.endOnBack && g_qk.decide && g_qk.moveTarget && g_qk.origTarget && g_qk.endMove && g_qk.getList && g_qk.listFocused &&
                 g_qk.menuData && g_qk.startQuery && g_qk.fPlayList && g_qk.fListPanel &&
                 g_qk.fTrackIndex;
    say("queue: keys %s (KeyEventArgs %p, ListPanelBase %p, LoadingListPanel %p, TrackListPanel %p)",
        g_qk.ready ? "ready" : "NOT ready", kea, lpb, llp, tlp);
}

// Mono's functions and the classes' methods, once (on the watch's thread,
// attached).
static struct {
    void *mono, *domain, *attach, *detach, *imageLoaded, *classFromName;
    int tries;
} g_setupAgain;

static void rowArtSetup(void* mono, void* domain, void* imageLoaded, void* classFromName);

// From the watch's loop, every 2 s while the rows' covers or the queue's keys
// are not ready, up to a minute.
static void setupAgain(void) {
    if (!g_setupAgain.attach || g_setupAgain.tries >= 30 || (g_row.ready && g_qk.ready)) return;
    if (sysExists(SC_BGM_DIR "/mcw-norowart")) return;
    g_setupAgain.tries++;
    void* thread = ((FnMonoPtr)g_setupAgain.attach)(g_setupAgain.domain);
    if (!thread) return;
    say("rowart: setting up again (%d)", g_setupAgain.tries);
    rowArtSetup(g_setupAgain.mono, g_setupAgain.domain, g_setupAgain.imageLoaded, g_setupAgain.classFromName);
    if (g_setupAgain.detach) ((FnMonoDetach)g_setupAgain.detach)(thread);
}

static void rowArtSetup(void* mono, void* domain, void* imageLoaded, void* classFromName) {
    if (sysExists(SC_BGM_DIR "/mcw-norowart")) {
        say("rowart: off (mcw-norowart)");
        return;
    }
    void* p;
    g_row.domain = domain;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_object_new", &p)) g_row.objectNew = (FnMonoNew)p;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_string_new", &p)) g_row.stringNew = (FnMonoStrNew)p;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_object_to_string", &p)) g_row.toString = (FnMonoToString)p;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_object_unbox", &p)) g_row.unbox = (FnMonoUnbox)p;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_gchandle_new_weakref", &p)) g_row.weak = (FnMonoWeak)p;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_gchandle_get_target", &p)) g_row.target = (FnMonoTarget)p;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_object_get_virtual_method", &p)) g_row.virt = (FnMonoVirtual)p;
    void* pui = ((FnMonoName)imageLoaded)("Sce.PlayStation.PUI");
    void* app = ((FnMonoName)imageLoaded)("app");
    void* json = ((FnMonoName)imageLoaded)("Sce.PlayStation.Json");
    FnMonoClass cls = (FnMonoClass)classFromName;
    FnMonoMethod m = g_mono.methodByName;
    void* box = pui ? cls(pui, "Sce.PlayStation.PUI.UI2", "ImageBox") : NULL;
    void* node = pui ? cls(pui, "Sce.PlayStation.PUI", "UINode") : NULL;
    void* widget = pui ? cls(pui, "Sce.PlayStation.PUI.UI2", "Widget") : NULL;
    void* jobj = json ? cls(json, "Sce.PlayStation.Json", "JsonObject") : NULL;
    void* data = app ? cls(app, "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListMenuData") : NULL;
    void* item = app ? cls(app, "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListPanelItem") : NULL;
    g_row.imageBox = box;
    if (box) {
        g_row.ctor = m(box, ".ctor", 0);
        g_row.loadAsync = m(box, "LoadAsync", 3);
        g_row.scaleType = m(box, "set_ImageScaleType", 1);
        g_row.getImage = m(box, "get_Image", 0);
    }
    void* opts = pui ? cls(pui, "Sce.PlayStation.PUI", "ImageOptions") : NULL;
    FnMonoOffset offset = NULL;
    if (!sceKernelDlsym((int)(intptr_t)mono, "mono_field_get_offset", &p)) offset = (FnMonoOffset)p;
    if (opts && offset && g_mono.set) {
        g_row.optCtor = m(opts, ".ctor", 0);
        g_row.optConvert = m(opts, "set_ConvertOption", 1);
        void* ff = g_mono.fieldByName(opts, "format");
        void* fr = g_mono.fieldByName(opts, "resize");
        void* fu = g_mono.fieldByName(opts, "isUpdatedOptions");
        // Only where the shell's own code writes them (0x5c, 0x68, 0x74),
        // offsets counted from the object's start, its header included.
        const int of = ff ? offset(ff) : -1, or_ = fr ? offset(fr) : -1, ou = fu ? offset(fu) : -1;
        say("rowart: ImageOptions %p: format at 0x%x, resize at 0x%x, isUpdatedOptions at 0x%x", opts,
            of, or_, ou);
        if (of == 0x5c) g_row.fFormat = ff;
        if (or_ == 0x68) g_row.fResize = fr;
        if (ou == 0x74) g_row.fUpdated = fu;
        if (g_row.optCtor) g_row.options = opts;
        g_row.setCache = m(opts, "set_Cache", 1);
        g_row.getManager = m(opts, "get_AssetManager", 0);
        g_row.setManager = m(opts, "set_AssetManager", 1);
        void* am = cls(pui, "Sce.PlayStation.PUI", "AssetManager");
        g_row.managerName = am ? g_mono.fieldByName(am, "name") : NULL;
        if (!sceKernelDlsym((int)(intptr_t)mono, "mono_gchandle_new", &p)) g_row.strong = (FnMonoWeak)p;
        if (!sceKernelDlsym((int)(intptr_t)mono, "mono_gchandle_free", &p))
            g_row.freeHandle = (void (*)(uint32_t))p;
        say("rowart: ImageOptions.get/set_AssetManager %p %p, AssetManager.name %p, gchandle_new %p",
            g_row.getManager, g_row.setManager, g_row.managerName, (void*)g_row.strong);
        void* io = cls(pui, "Sce.PlayStation.PUI", "ImageObject");
        g_row.globalCache = io ? m(io, "get_GlobalCache", 0) : NULL;
        say("rowart: ImageOptions.set_Cache %p, ImageObject.get_GlobalCache %p", g_row.setCache,
            g_row.globalCache);
    }
    if (node) {
        g_row.setX = m(node, "set_X", 1);
        g_row.setY = m(node, "set_Y", 1);
        g_row.setW = m(node, "set_Width", 1);
        g_row.setH = m(node, "set_Height", 1);
        g_row.getX = m(node, "get_X", 0);
        g_row.setVisible = m(node, "set_Visible", 1);
    }
    if (widget) {
        g_row.appendChild = m(widget, "AppendChild", 1);
        g_row.removeChild = m(widget, "RemoveFromParent", 0);
    }
    if (jobj) g_row.jsonItem = m(jobj, "get_Item", 1);
    if (data) {
        g_row.fType = g_mono.fieldByName(data, "Type");
        g_row.fData = g_mono.fieldByName(data, "Data");
    }
    if (item) {
        g_row.fText = g_mono.fieldByName(item, "m_textLabel");
        g_row.fSub = g_mono.fieldByName(item, "m_subTextLabel");
    }
    g_row.ready = g_row.objectNew && g_row.stringNew && g_row.toString && g_row.unbox && g_row.weak &&
                  g_row.target && g_mono.invoke && g_mono.get && g_row.ctor && g_row.loadAsync &&
                  g_row.setX && g_row.setY && g_row.setW && g_row.setH && g_row.getX &&
                  g_row.setVisible && g_row.appendChild && g_row.jsonItem && g_row.fType &&
                  g_row.fData && g_row.fText && g_row.fSub;
    say("rowart: ImageBox %p (.ctor %p, LoadAsync %p), UINode %p, Widget.AppendChild %p, "
        "JsonObject.get_Item %p, TrackListMenuData %p, TrackListPanelItem %p: %s",
        box, g_row.ctor, g_row.loadAsync, node, g_row.appendChild, g_row.jsonItem, data, item,
        g_row.ready ? "ready" : "NOT ready");
    qkSetup(pui, app, cls);
}

// The Downloads list's calls to the download service (BGFT), logged only
// (0.85): what a task of ours in Notifications > Downloads would have to
// answer. After the call: its arguments, its answer, and what it filled in.
static void bgftLog(Frame* f) {
    const Hook* h = f->hook;
    if (f->seq > 150) return;
    const uint64_t* g = f->gpr;
    char extra[400];
    extra[0] = 0;
    if (contains(h->name, "DownloadQueryTaskSection") || contains(h->name, "DownloadQueryTask")) {
        // (..., out uint actualNum, QueryTaskResult[] results): the last two.
        const int sec = contains(h->name, "Section");
        const uint32_t* num = (const uint32_t*)(uintptr_t)(sec ? g[2] : g[4]);
        const uint8_t* arr = (const uint8_t*)(uintptr_t)(sec ? g[3] : g[5]);
        uint32_t n = 0;
        if (hookReadable(num, 4) == 4) n = *num;
        int at = fmt(extra, sizeof(extra), " actualNum %u:", n);
        for (uint32_t k = 0; k < n && k < 12 && arr && hookReadable(arr + 0x20 + k * 8, 8) == 8; k++) {
            int32_t id;
            uint32_t prm;
            memcpy(&id, arr + 0x20 + k * 8, 4);
            memcpy(&prm, arr + 0x24 + k * 8, 4);
            at += fmt(extra + at, sizeof(extra) - at, " [task %d param 0x%x]", id, prm);
        }
    } else if (contains(h->name, "DownloadGetProgress")) {
        // (taskId, TaskProgress progress): the object's fields, raw.
        const uint8_t* o = (const uint8_t*)(uintptr_t)g[1];
        if (o && hookReadable(o + 0x10, 0x48) == 0x48) {
            int at = fmt(extra, sizeof(extra), " progress:");
            for (int k = 0; k < 0x48; k += 4) {
                uint32_t v;
                memcpy(&v, o + 0x10 + k, 4);
                at += fmt(extra + at, sizeof(extra) - at, " %x", v);
            }
        }
    } else if (contains(h->name, "DownloadGetTaskInfo")) {
        // (taskId, out DownloadTaskInfo {string contentTitle, iconPath; ulong tick}).
        const uint64_t* info = (const uint64_t*)(uintptr_t)g[1];
        if (hookReadable(info, 24) == 24) {
            char t[120], icon[160];
            monoText(info[0], t, sizeof(t));
            monoText(info[1], icon, sizeof(icon));
            fmt(extra, sizeof(extra), " title \"%s\", icon \"%s\", tick %lu", t, icon, (unsigned long)info[2]);
        }
    } else if (contains(h->name, "GetTaskInfoValueString")) {
        const uint64_t* out = (const uint64_t*)(uintptr_t)g[2];
        if (hookReadable(out, 8) == 8) {
            char t[200];
            monoText(*out, t, sizeof(t));
            fmt(extra, sizeof(extra), " \"%s\"", t);
        }
    } else if (contains(h->name, "GetTaskInfoValue")) {
        const uint64_t* out = (const uint64_t*)(uintptr_t)g[2];
        if (hookReadable(out, 8) == 8) fmt(extra, sizeof(extra), " value %lu", (unsigned long)*out);
    }
    say("bgft: %s(%lx, %lx, %lx, %lx) = 0x%lx%s  [#%u, thread %d]", h->name, (unsigned long)g[0], (unsigned long)g[1],
        (unsigned long)g[2], (unsigned long)g[3], (unsigned long)f->retRax, extra, f->seq, scePthreadGetthreadid());
}

static void monoLog(Frame* f) {
    const Hook* h = f->hook;
    if (h->json == 1) {
        jsonLog(f);
        return;
    }
    const int first = f->seq <= 20;
    if (h->json == 2) {
        const int was = (int)(f->retRax & 0xFF);
        if (g_forceApp) f->retRax = (f->retRax & ~(uint64_t)0xFF) | 1;
        // Its object's m_appVer, the Spotify app's version it found (0: not
        // installed), at 0xB8 (get_AppVer: vmovss xmm0, [rdi+0xb8]). 0.44's
        // true alone left "Download Spotify" there, X going to it.
        float ver = -1.0f;
        uint8_t* self = (uint8_t*)(uintptr_t)f->gpr[0];
        if (hookReadable(self + APPVER_AT, 4) == 4 && (hookProt(self + APPVER_AT) & 2)) {
            memcpy(&ver, self + APPVER_AT, 4);
            if (g_forceApp && ver < APPVER_SAID) {
                const float said = APPVER_SAID;
                memcpy(self + APPVER_AT, &said, 4);
            }
        }
        if (g_forceApp) spotifyEntryFix(self);
        if (first) {
            const int v100 = (int)(ver * 100.0f);
            say("mono: %s(%lu) = %d%s; app version there %d.%02d%s  [#%u, thread %d]", h->name,
                f->gpr[1] & 0xFF, was, g_forceApp && !was ? ", answered 1" : "", v100 / 100,
                (v100 < 0 ? -v100 : v100) % 100,
                g_forceApp && ver < APPVER_SAID ? ", made 9.99" : "", f->seq,
                scePthreadGetthreadid());
        }
    } else if (h->json == 6) {
        // IsInstalled(string titleId), static: the title in rdi.
        char title[40];
        monoText(f->gpr[0], title, sizeof(title));
        const int was = (int)(f->retRax & 0xFF);
        const int ours = g_forceApp && !was && eq(title, "CUSA01780");
        if (ours) f->retRax = (f->retRax & ~(uint64_t)0xFF) | 1;
        if (f->seq <= 40)
            say("mono: IsInstalled(\"%s\") = %d%s  [#%u, thread %d]", title, was,
                ours ? ", answered 1" : "", f->seq, scePthreadGetthreadid());
    } else if (h->json == 7) {
        // An int returned: 0, or the error the shell shows.
        say("mono: %s(%lx) = 0x%X  [#%u, thread %d]", h->name, f->gpr[1], (uint32_t)f->retRax,
            f->seq, scePthreadGetthreadid());
    } else if (h->json == 8) {
        rowArt(f);
    } else if (h->json == 9) {
        rowLoaded(f);
    } else if (h->json == 10) {
        rowShellLoad(f);
    } else if (h->json == 17) {
        bgftLog(f);
    } else if (h->json == 12) {
        qkMoved(f);
    } else if (h->json == 13) {
        qkMoveEnded(f);
    } else if (h->json == 5) {
        if (f->seq <= 50)
            say("mono: %s: %d (0x%X), 0x%X  [#%u, thread %d]", h->name, (int32_t)f->gpr[1],
                (uint32_t)f->gpr[1], (uint32_t)f->gpr[2], f->seq, scePthreadGetthreadid());
    } else if (h->json == 3 && first) {
        float v;
        memcpy(&v, f->retXmm0, 4);
        const int hundredths = (int)(v * 100.0f);
        say("mono: %s = %d.%02d  [#%u, thread %d]", h->name, hundredths / 100,
            (hundredths < 0 ? -hundredths : hundredths) % 100, f->seq, scePthreadGetthreadid());
    }
}

// ── The Music section's own code, for reading (0.49) ─────────────────────────
//
// Its C# is compiled away (full AOT): what decides "Download Spotify", and
// what X on a playlist does, is only in its machine code. For each class
// below, through Mono: its fields' offsets; each method's code (its address
// from mono_compile_method, its start and size from mono_jit_info_table_find),
// into code/code.bin; and each call in it, walked with HDE64, followed through
// a jmp [rip] stub, named by mono_jit_info_table_find. code/methods.map has
// it all, to disassemble beside. Once: delete code/ to have it again.
//
// mono_compile_method on a method with no AOT code would abort this
// precompiled runtime: abstract, P/Invoke and runtime methods are skipped,
// and the method about to be asked for is kept in code/dumping; one the shell
// went down on is in code/skip.txt from then on.

#define CODE_DIR SC_BGM_DIR "/code"
typedef void* (*FnMonoIter)(void*, void**);
typedef char* (*FnMonoFullName)(void*, int);
typedef uint32_t (*FnMonoFlags)(void*, uint32_t*);
typedef void* (*FnMonoJitFind)(void*, void*);
typedef void* (*FnMonoJitPtr)(void*);
typedef int (*FnMonoJitSize)(void*);
typedef int (*FnMonoFieldOffset)(void*);
static struct {
    FnMonoIter methods;
    FnMonoFullName fullName;
    FnMonoFlags flags;
    FnMonoJitFind jitFind;
    FnMonoJitPtr jitStart, jitMethod;
    FnMonoJitSize jitSize;
    FnMonoFieldOffset fieldOffset;
    FnMonoIter nested;
    FnMonoFieldName className;
} g_dump;

#ifdef SC_DEBUG  // the code dump (0.49): SC_DEBUG builds only (0.95)
static int dumpPut(int fd, const char* f, ...) __attribute__((format(printf, 2, 3)));
static int dumpPut(int fd, const char* f, ...) {
    char line[700];
    va_list ap;
    va_start(ap, f);
    int n = fmtv(line, sizeof(line), f, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    return sysWrite(fd, line, (size_t)n);
}

// The managed method at addr, by name, into out: "" if Mono has none.
static void dumpName(void* domain, uint64_t addr, char* out, size_t cap) {
    out[0] = 0;
    void* ji = g_dump.jitFind(domain, (void*)(uintptr_t)addr);
    void* m = ji ? g_dump.jitMethod(ji) : NULL;
    char* n = m ? g_dump.fullName(m, 1) : NULL;
    if (n) fmt(out, cap, "%s", n);
}

static uint64_t followStub(uint64_t t) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)t;
    if (hookReadable(p, 6) == 6 && p[0] == 0xFF && p[1] == 0x25) {  // jmp [rip+d]: a PLT entry
        int32_t d;
        memcpy(&d, p + 2, 4);
        const uint64_t* slot = (const uint64_t*)(uintptr_t)(t + 6 + (int64_t)d);
        if (hookReadable(slot, 8) == 8) return *slot;
    }
    return t;
}

static void codeDump(void* domain, void* imageLoaded, void* classFromName, void* compile) {
    if (sysExists(CODE_DIR "/methods3.map")) {
        say("code: code/methods3.map is there already: not again");
        return;
    }
    if (!g_dump.methods || !g_dump.fullName || !g_dump.jitFind || !g_dump.jitStart ||
        !g_dump.jitSize || !g_dump.jitMethod) {
        say("code: Mono's API for it is not all there");
        return;
    }
    sysMkdir(CODE_DIR, 0777);
    // A method the shell went down on last time: skipped from now on.
    char last[300];
    int fd = sysOpen(CODE_DIR "/dumping", SC_O_RDONLY, 0);
    if (fd >= 0) {
        const int n = sysRead(fd, last, sizeof(last) - 1);
        sysClose(fd);
        if (n > 0) {
            last[n] = 0;
            const int sk = sysOpen(CODE_DIR "/skip.txt", SC_O_WRONLY | SC_O_CREAT, 0666);
            if (sk >= 0) {
                sysSeek(sk, 0, 2);
                dumpPut(sk, "%s", last);
                sysClose(sk);
            }
            say("code: the last dump stopped at %s: skipped from now on", last);
        }
        sysUnlink(CODE_DIR "/dumping");
    }
    static char skips[4096];
    skips[0] = 0;
    fd = sysOpen(CODE_DIR "/skip.txt", SC_O_RDONLY, 0);
    if (fd >= 0) {
        const int n = sysRead(fd, skips, sizeof(skips) - 1);
        skips[n > 0 ? n : 0] = 0;
        sysClose(fd);
    }
    const int map = sysOpen(CODE_DIR "/methods3.map.new", SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    const int bin = sysOpen(CODE_DIR "/code3.bin", SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (map < 0 || bin < 0) return;
    static const char* const want[][2] = {
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayStationMusicPanel"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayListPanel"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayListPanelItem"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayListMenuData"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayStationMusicContext"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "MusicPlayerPanel"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "ControlPanel"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListPanel"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListMenuData"},
        {"Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListMenuPanel"},
        {"Sce.Vsh.ShellUI.QuickMenu", "UserContext"},
        {"Sce.Vsh.ShellUI.QuickMenu", "AppBrowseItemData"},
        {"Sce.Vsh.ShellUI.QuickMenu", "DirectPlayback"},
        // What a track chosen asks (0.56): PlaySpotifyUri, and what it checks.
        {"Sce.Vsh.ShellUI.MusicCoreServer", "MusicCoreServerController"},
        {"Sce.Vsh.ShellUI.MusicCoreServer", "MusicCoreServerControllerImpl"},
        {"Sce.Vsh.ShellUI.MusicCoreServer", "MusicCoreServerControllerStorage"},
    };
    void* image = ((FnMonoName)imageLoaded)("app");
    uint64_t at = 0;
    int methods = 0, calls = 0;
    // The classes, and those nested in them (lambdas, display classes, async
    // state machines: where StartQuery's task is), two levels down.
    static void* classes[256];
    static char names[256][160];
    int nClasses = 0;
    for (size_t k = 0; image && k < sizeof(want) / sizeof(want[0]) && nClasses < 256; k++) {
        void* klass = ((FnMonoClass)classFromName)(image, want[k][0], want[k][1]);
        dumpPut(map, "class %s.%s %p", want[k][0], want[k][1], klass);
        if (!klass) continue;
        const int top = nClasses;
        fmt(names[nClasses], sizeof(names[0]), "%s", want[k][1]);
        classes[nClasses++] = klass;
        for (int level = 0; level < 2; level++) {
            const int end = nClasses;
            for (int c = level ? top + 1 : top; c < end && nClasses < 256; c++) {
                if (!g_dump.nested) break;
                void* iter = NULL;
                void* n;
                while (nClasses < 256 && (n = g_dump.nested(classes[c], &iter))) {
                    fmt(names[nClasses], sizeof(names[0]), "%s/%s", names[c],
                        g_dump.className ? g_dump.className(n) : "?");
                    classes[nClasses++] = n;
                }
            }
        }
    }
    for (int k = 0; k < nClasses; k++) {
        void* klass = classes[k];
        dumpPut(map, "class %s %p", names[k], klass);
        // A generic class (`1 in its name) has no code of its own, only each
        // instance's: asking for it aborted the runtime on a PS4 (0.50, at
        // ControlPanel/<>c__DisplayClass41_0`1:.ctor).
        if (contains(names[k], "`")) {
            dumpPut(map, "skipclass %s (generic)", names[k]);
            continue;
        }
        void* iter = NULL;
        void* f;
        while (g_dump.fieldOffset && g_mono.fields && (f = g_mono.fields(klass, &iter)))
            dumpPut(map, "field 0x%x %s", (unsigned)g_dump.fieldOffset(f),
                    g_mono.fieldName ? g_mono.fieldName(f) : "?");
        iter = NULL;
        void* m;
        while ((m = g_dump.methods(klass, &iter))) {
            char* full = g_dump.fullName(m, 1);
            if (!full) continue;
            uint32_t iflags = 0;
            const uint32_t flags = g_dump.flags ? g_dump.flags(m, &iflags) : 0;
            // abstract 0x400, pinvoke 0x2000; internal call 0x1000, runtime 0x3
            if ((flags & 0x2400) || (iflags & 0x1003)) {
                dumpPut(map, "skip %s (flags 0x%x, 0x%x)", full, flags, iflags);
                continue;
            }
            // A generic method: Name<T> (...) or a ` in it.
            const char* paren = full;
            while (*paren && *paren != '(') paren++;
            const int generic = contains(full, "`") || (paren > full + 1 && paren[-2] == '>');
            if (generic) {
                dumpPut(map, "skip %s (generic)", full);
                continue;
            }
            if (contains(skips, full)) {
                dumpPut(map, "skip %s (it went down last time)", full);
                continue;
            }
            fd = sysOpen(CODE_DIR "/dumping", SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
            if (fd >= 0) {
                sysWrite(fd, full, strlen(full));
                sysFsync(fd);
                sysClose(fd);
            }
            void* code = ((FnMonoPtr)compile)(m);
            void* ji = code ? g_dump.jitFind(domain, code) : NULL;
            const uint8_t* start = ji ? g_dump.jitStart(ji) : NULL;
            const int size = ji ? g_dump.jitSize(ji) : 0;
            if (!start || size <= 0 || size > (1 << 20) || hookReadable(start, (size_t)size) < (size_t)size) {
                dumpPut(map, "nocode %p %s", code, full);
                continue;
            }
            sysWrite(bin, start, (size_t)size);
            dumpPut(map, "method 0x%lx %p 0x%x %s", at, start, (unsigned)size, full);
            // Its calls, named.
            for (int o = 0; o < size;) {
                hde64s hs;
                const unsigned len = hde64_disasm(start + o, &hs);
                if (!len || (hs.flags & F_ERROR)) break;
                uint64_t to = 0;
                const uint64_t end = (uint64_t)(uintptr_t)(start + o + len);
                if (hs.opcode == 0xE8 && (hs.flags & F_IMM32)) to = end + (int64_t)(int32_t)hs.imm.imm32;
                else if (hs.opcode == 0xFF && hs.modrm_reg == 2 && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
                    const uint64_t* slot = (const uint64_t*)(uintptr_t)(end + (int64_t)(int32_t)hs.disp.disp32);
                    if (hookReadable(slot, 8) == 8) to = *slot;
                }
                if (to) {
                    char name[400];
                    const uint64_t real = followStub(to);
                    dumpName(domain, real, name, sizeof(name));
                    dumpPut(map, "  call +0x%x %lx %s", (unsigned)o, real, name[0] ? name : "?");
                    calls++;
                }
                o += (int)len;
            }
            at += (uint64_t)size;
            methods++;
        }
        say("code: %s: done, %d methods so far", names[k], methods);
        drain();
    }
    sysClose(bin);
    sysClose(map);
    sysUnlink(CODE_DIR "/dumping");
    sysRename(CODE_DIR "/methods3.map.new", CODE_DIR "/methods3.map");
    say("code: %d methods, %lu bytes, %d calls named: code/methods3.map and code/code3.bin",
        methods, at, calls);
    notify("Music watch: the Music section's code is in ShellCompanion/ps4/code");
}

#endif  // SC_DEBUG

static void monoHooks(void) {
    g_forceApp = sysExists(EBGM_DIR);
    int mono = -1;
    for (int i = 0; i < 2 && mono < 0; i++) {
        char path[160];
        const char* word = sceKernelGetFsSandboxRandomWord();
        fmt(path, sizeof(path), "/%s/common/lib/libmonosgen-2.0.sprx", word ? word : "?");
        int res = 0;
        mono = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
        say("mono: %s: 0x%08X", path, (unsigned)mono);
    }
    if (mono < 0) return;
    void *rootDomain = NULL, *attach = NULL, *detach = NULL, *imageLoaded = NULL,
         *classFromName = NULL, *methodFromName = NULL, *compile = NULL;
    sceKernelDlsym(mono, "mono_get_root_domain", &rootDomain);
    sceKernelDlsym(mono, "mono_thread_attach", &attach);
    sceKernelDlsym(mono, "mono_thread_detach", &detach);
    sceKernelDlsym(mono, "mono_image_loaded", &imageLoaded);
    sceKernelDlsym(mono, "mono_class_from_name", &classFromName);
    sceKernelDlsym(mono, "mono_class_get_method_from_name", &methodFromName);
    sceKernelDlsym(mono, "mono_compile_method", &compile);
    void* p;
    if (!sceKernelDlsym(mono, "mono_object_get_class", &p)) g_mono.objectClass = (FnMonoGetClass)p;
    if (!sceKernelDlsym(mono, "mono_class_get_field_from_name", &p)) g_mono.fieldByName = (FnMonoFieldByName)p;
    if (!sceKernelDlsym(mono, "mono_field_get_value", &p)) g_mono.get = (FnMonoFieldValue)p;
    if (!sceKernelDlsym(mono, "mono_field_set_value", &p)) g_mono.set = (FnMonoFieldValue)p;
    if (!sceKernelDlsym(mono, "mono_class_get_fields", &p)) g_mono.fields = (FnMonoFields)p;
    if (!sceKernelDlsym(mono, "mono_field_get_name", &p)) g_mono.fieldName = (FnMonoFieldName)p;
    if (!sceKernelDlsym(mono, "mono_runtime_invoke", &p)) g_mono.invoke = (FnMonoInvoke)p;
    g_mono.methodByName = (FnMonoMethod)methodFromName;
    if (!sceKernelDlsym(mono, "mono_class_get_methods", &p)) g_dump.methods = (FnMonoIter)p;
    if (!sceKernelDlsym(mono, "mono_method_full_name", &p)) g_dump.fullName = (FnMonoFullName)p;
    if (!sceKernelDlsym(mono, "mono_method_get_flags", &p)) g_dump.flags = (FnMonoFlags)p;
    if (!sceKernelDlsym(mono, "mono_jit_info_table_find", &p)) g_dump.jitFind = (FnMonoJitFind)p;
    if (!sceKernelDlsym(mono, "mono_jit_info_get_code_start", &p)) g_dump.jitStart = (FnMonoJitPtr)p;
    if (!sceKernelDlsym(mono, "mono_jit_info_get_code_size", &p)) g_dump.jitSize = (FnMonoJitSize)p;
    if (!sceKernelDlsym(mono, "mono_jit_info_get_method", &p)) g_dump.jitMethod = (FnMonoJitPtr)p;
    if (!sceKernelDlsym(mono, "mono_field_get_offset", &p)) g_dump.fieldOffset = (FnMonoFieldOffset)p;
    if (!sceKernelDlsym(mono, "mono_class_get_nested_types", &p)) g_dump.nested = (FnMonoIter)p;
    if (!sceKernelDlsym(mono, "mono_class_get_name", &p)) g_dump.className = (FnMonoFieldName)p;
    say("mono: object class %p, field by name %p, get %p, set %p, fields %p, field name %p, invoke %p",
        (void*)g_mono.objectClass, (void*)g_mono.fieldByName, (void*)g_mono.get, (void*)g_mono.set,
        (void*)g_mono.fields, (void*)g_mono.fieldName, (void*)g_mono.invoke);
    drain();
    if (!rootDomain || !attach || !imageLoaded || !classFromName || !methodFromName || !compile) {
        say("mono: its API is not all there");
        return;
    }
    void* domain = ((FnMonoVoid)rootDomain)();
    void* thread = domain ? ((FnMonoPtr)attach)(domain) : NULL;
    say("mono: domain %p, this thread attached: %p", domain, thread);
    drain();
    if (!thread) return;
    rowArtSetup((void*)(intptr_t)mono, domain, imageLoaded, classFromName);
    // Kept to try again (0.88): loaded early, as the shell starts again, the
    // UI library was not there yet, and the rows' covers and the queue's keys
    // stayed off.
    g_setupAgain.mono = (void*)(intptr_t)mono;
    g_setupAgain.domain = domain;
    g_setupAgain.attach = attach;
    g_setupAgain.detach = detach;
    g_setupAgain.imageLoaded = imageLoaded;
    g_setupAgain.classFromName = classFromName;
    drain();
    static const struct {
        const char *image, *ns, *klass, *method;
        int args, kind;
        const char* name;
    } want[MONO_HOOKS] = {
        {"Sce.PlayStation.Json", "Sce.PlayStation.Json", "JsonObject", "get_Item", 1, 1, "json.Item"},
        {"Sce.PlayStation.Json", "Sce.PlayStation.Json", "JsonObject", "ValueOrDefault", 1, 1,
         "json.ValueOrDefault"},
        {"Sce.PlayStation.Json", "Sce.PlayStation.Json", "JsonObject", "ContainsKey", 1, 1,
         "json.ContainsKey"},
        {"Sce.PlayStation.Json", "Sce.PlayStation.Json", "JsonObject", "TryGetValue", 2, 1,
         "json.TryGetValue"},
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayStationMusicContext",
         "IsValidAppVer", 1, 2, "IsValidAppVer"},
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayStationMusicContext",
         "get_AppVer", 0, 3, "AppVer"},
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "PlayStationMusicContext",
         "NotifyAppVerChanged", 1, 4, "NotifyAppVerChanged"},
        // The track list's query, as it ends (0.50): its error code chooses
        // the panel's message ("Download Spotify": 0x80BA0027-29).
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListPanel", "OnQueryFinish", 2,
         5, "TrackListPanel.OnQueryFinish(errorCode, list)"},
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListMenuPanel",
         "OnTotalChanged", 2, 5, "TrackListMenuPanel.OnTotalChanged(total, errorCode)"},
        // The track list's query first asks whether Spotify's app is
        // installed, 0x80BA0028 ("Download Spotify") if not (0.51's dump,
        // TrackListPanel/<>c__DisplayClass25_0:<StartQuery>b__0 at +0x23c):
        // answered true for CUSA01780 while ebgm/ is there.
        {"app", "Sce.Vsh.ShellUI.AppSystem", "ApplicationMonitor/Util/AppInfo", "IsInstalled", 1, 6,
         "AppInfo.IsInstalled"},
        // A track chosen: the press's task asks this, then shows an error
        // dialog if it is not 0 (0.55: an error, no PlaySpUri reached).
        {"app", "Sce.Vsh.ShellUI.MusicCoreServer", "MusicCoreServerController", "PlaySpotifyUri", 1,
         7, "MusicCoreServerController.PlaySpotifyUri"},
        // A track's row filled in: given its cover (0.62, rowArt).
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListPanelItem", "UpdateData", 1,
         8, "TrackListPanelItem.UpdateData"},
        // An ImageBox's load finished: what came of ours (0.64, rowLoaded).
        {"Sce.PlayStation.PUI", "Sce.PlayStation.PUI.UI2", "ImageBox", "HandleLoadCompleted", 3, 9,
         "ImageBox.HandleLoadCompleted"},
        // The shell's own loads: their AssetManager, for ours (0.66, rowShellLoad).
        {"Sce.PlayStation.PUI", "Sce.PlayStation.PUI.UI2", "ImageBox", "LoadAsync", 3, 10,
         "ImageBox.LoadAsync"},
        // The queue edited by keys in the Music section (0.67, qkKey).
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "ControlPanel", "OnGlobalKeyEventReceived",
         2, 11, "ControlPanel.OnGlobalKeyEventReceived"},
        {"Sce.PlayStation.PUI", "Sce.PlayStation.PUI.UI2", "ListPanelBase", "OnItemMoved", 2, 12,
         "ListPanelBase.OnItemMoved"},
        {"Sce.PlayStation.PUI", "Sce.PlayStation.PUI.UI2", "ListPanelBase", "EndItemMoveMode", 0, 13,
         "ListPanelBase.EndItemMoveMode"},
        {"Sce.PlayStation.PUI", "Sce.PlayStation.PUI.UI2", "ListPanelBase", "DecideMoveItem", 0, 14,
         "ListPanelBase.DecideMoveItem"},
        {"Sce.PlayStation.PUI", "Sce.PlayStation.PUI.UI2", "ListPanelBase", "RollbackMoveTarget", 0, 15,
         "ListPanelBase.RollbackMoveTarget"},
        // A row chosen: its own track, for the pick (0.77, qkChosen).
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListMenuPanel", "OnItemSelected", 1, 16,
         "TrackListMenuPanel.OnItemSelected"},
        {"app", "Sce.Vsh.ShellUI.QuickMenu.PlayStationMusic", "TrackListMenuPanel", "OnItemSelected", 2, 16,
         "TrackListMenuPanel.OnItemSelected(withLog)"},
        // The Downloads list's calls to the download service, logged (0.85).
        {"Sce.Vsh.Orbis.Bgft", "Sce.Vsh.Orbis.Bgft", "BgftWrapper", "DownloadQueryTask", 6, 17,
         "BgftWrapper.DownloadQueryTask"},
        {"Sce.Vsh.Orbis.Bgft", "Sce.Vsh.Orbis.Bgft", "BgftWrapper", "DownloadQueryTaskSection", 4, 17,
         "BgftWrapper.DownloadQueryTaskSection"},
        {"Sce.Vsh.Orbis.Bgft", "Sce.Vsh.Orbis.Bgft", "BgftWrapper", "DownloadGetProgress", 2, 17,
         "BgftWrapper.DownloadGetProgress"},
        {"Sce.Vsh.Orbis.Bgft", "Sce.Vsh.Orbis.Bgft", "BgftWrapper", "DownloadGetTaskInfo", 2, 17,
         "BgftWrapper.DownloadGetTaskInfo"},
        {"Sce.Vsh.Orbis.Bgft", "Sce.Vsh.Orbis.Bgft", "BgftWrapper", "GetTaskInfoValueString", 3, 17,
         "BgftWrapper.GetTaskInfoValueString"},
        {"Sce.Vsh.Orbis.Bgft", "Sce.Vsh.Orbis.Bgft", "BgftWrapper", "GetTaskInfoValueInt", 3, 17,
         "BgftWrapper.GetTaskInfoValueInt"},
        {"Sce.Vsh.Orbis.BgftAccessor", "Sce.Vsh.Orbis.Bgft", "DownloadItemAccessor2", "GetTotalCount", 0, 17,
         "DownloadItemAccessor2.GetTotalCount"},
        {"Sce.Vsh.Orbis.BgftAccessor", "Sce.Vsh.Orbis.Bgft", "DownloadItemAccessor2", "GetTaskParam", 1, 17,
         "DownloadItemAccessor2.GetTaskParam"},
        {"Sce.Vsh.Orbis.BgftAccessor", "Sce.Vsh.Orbis.Bgft", "DownloadItemAccessor", "GetTotalCount", 0, 17,
         "DownloadItemAccessor.GetTotalCount"},
    };
    // The download service's (kind 17), logged only when asked (0.87): with a
    // download going, Notifications > Downloads crashed the shell under
    // 0.85's hooks. mcw-bgftlog for all but DownloadGetProgress, which
    // aligns its stack for AVX; mcw-bgftprogress for that one too.
#ifdef SC_DEBUG
    const int bgftLogOn = sysExists(SC_BGM_DIR "/mcw-bgftlog") || sysExists(SC_BGM_DIR "/mcw-bgftprogress");
#else
    const int bgftLogOn = 0;  // SC_DEBUG builds only (0.95)
#endif
    const int bgftProgressOn = sysExists(SC_BGM_DIR "/mcw-bgftprogress");
    say("bgft: logging %s", bgftProgressOn ? "on, DownloadGetProgress too" : bgftLogOn ? "on, not DownloadGetProgress"
                                                                                       : "off (mcw-bgftlog, mcw-bgftprogress)");
    for (int i = 0; i < MONO_HOOKS; i++) {
        if (want[i].kind == 17 &&
            (!bgftLogOn || (contains(want[i].name, "DownloadGetProgress") && !bgftProgressOn))) {
            g_monoHooks[i].name = want[i].name;
            continue;
        }
        void* image = ((FnMonoName)imageLoaded)(want[i].image);
        void* klass = image ? ((FnMonoClass)classFromName)(image, want[i].ns, want[i].klass) : NULL;
        if (!klass && image && contains(want[i].klass, "/")) {
            // Outer/Inner/...: the outer class, then each nested one by name.
            char path[128];
            fmt(path, sizeof(path), "%s", want[i].klass);
            char* part = path;
            char* slash = path;
            while (*slash && *slash != '/') slash++;
            *slash = 0;
            klass = ((FnMonoClass)classFromName)(image, want[i].ns, part);
            part = slash + 1;
            while (klass && *part && g_dump.nested && g_dump.className) {
                char* end = part;
                while (*end && *end != '/') end++;
                const char c = *end;
                *end = 0;
                void* iter = NULL;
                void* n;
                void* found = NULL;
                while (!found && (n = g_dump.nested(klass, &iter)))
                    if (eq(g_dump.className(n), part)) found = n;
                klass = found;
                part = c ? end + 1 : end;
            }
        }
        void* m = klass ? ((FnMonoMethod)methodFromName)(klass, want[i].method, want[i].args) : NULL;
        void* code = m ? ((FnMonoPtr)compile)(m) : NULL;
        Hook* h = &g_monoHooks[i];
        h->name = want[i].name;
        h->json = want[i].kind;
        char bytes[64] = "";
        if (code && hookReadable(code, 16) == 16) hex16(bytes, sizeof(bytes), code);
        const char* why = "no code";
        int rc = -1;
        if (code) {
            lock(&g_installLock);
            rc = hookInstall(h, code, &why);
            unlock(&g_installLock);
        }
        say("mono: %s.%s (%s): image %p, class %p, method %p, code %p: %s; %s%s", want[i].klass,
            want[i].method, want[i].image, image, klass, m, code, bytes,
            rc == 0 ? "hooked" : "NOT hooked: ", rc == 0 ? "" : why);
        drain();
    }
    drain();
#ifdef SC_DEBUG
    codeDump(domain, imageLoaded, classFromName, compile);
#else
    (void)compile;
#endif
    drain();
    if (detach) ((FnMonoDetach)detach)(thread);
    say("mono: this thread detached; IsValidAppVer %s", g_forceApp ? "answered true (ebgm/ is there)" : "only logged");
}

void mcwPre(Frame* f) {
    f->t0 = sysNowUs();
    Hook* h = f->hook;
    f->seq = __atomic_add_fetch(&h->calls, 1, __ATOMIC_RELAXED);
    if (h->json == 4 && g_forceApp) {
        // NotifyAppVerChanged(float ver), ver in xmm0, changed before the
        // call: the panels told of an installed Spotify, never of 0.
        float v;
        memcpy(&v, f->xmm[0], 4);
        if (v < APPVER_SAID) {
            const float said = APPVER_SAID;
            memcpy(f->xmm[0], &said, 4);
            if (f->seq <= 20) {
                const int v100 = (int)(v * 100.0f);
                say("mono: NotifyAppVerChanged(%d.%02d) made 9.99  [#%u]", v100 / 100,
                    (v100 < 0 ? -v100 : v100) % 100, f->seq);
            }
        }
    }
    if (h->json == 11) {
        qkKey(f);  // a key, before the Music panel's handler sees it (0.67)
        return;
    }
    if (h->json == 16) {
        qkChosen(f);  // a row chosen, before it is played (0.77)
        return;
    }
    if (h->json == 14 || h->json == 15) {
        qkDecide(f);  // a track put down, before the list does it (0.68)
        return;
    }
    if (h->json) return;  // the key, after: nothing read here
    for (int i = 0; i < 6; i++) {
        f->preLen[i] = 0;
        if (g_quiet || deadStack(f, f->gpr[i])) continue;
        const void* p = (const void*)(uintptr_t)f->gpr[i];
        size_t n = hookReadable(p, HOOK_PRE_BYTES);
        if (n && (hookProt(p) & 4)) n = 0;  // code: a callback, not data
        if (n) memcpy(f->pre[i], p, n);
        f->preLen[i] = (int32_t)n;
    }
    if (!g_quiet && isRegist(h))
        for (int i = 0; i < 6; i++)
            if (f->gpr[i] && hookProt((const void*)(uintptr_t)f->gpr[i]) > 0 &&
                (hookProt((const void*)(uintptr_t)f->gpr[i]) & 4))
                hookCallback(h, i, f->gpr[i]);
}

#ifdef SC_COMPANION
// The player's, in the same module (ps4/bgm/dllist.c, 0.96): the download
// service's progress call, answered for a download list's placeholders.
void dlListHook(Frame* f);
void dlListQueryHook(Frame* f);

// A native function hooked for the player, under the install lock.
int mcwHookNative(Hook* h, void* fn, const char** why) {
    lock(&g_installLock);
    const int rc = hookInstall(h, fn, why);
    unlock(&g_installLock);
    return rc;
}
#endif

void mcwPost(Frame* f) {
#ifdef SC_COMPANION
    if (f->hook->json == 18) {
        dlListHook(f);
        return;
    }
    if (f->hook->json == 19) {
        dlListQueryHook(f);
        return;
    }
#endif
    if (f->hook->json) {
        if (!g_quiet || f->hook->json == 2 || f->hook->json == 6 || f->hook->json == 7 ||
            f->hook->json == 8 || f->hook->json == 9 || f->hook->json == 10 ||
            f->hook->json == 12 || f->hook->json == 13 || f->hook->json == 17)
            monoLog(f);
        return;
    }
    fakeApply(f);
    ebgmApply(f);
    if (g_quiet) return;
    Hook* h = f->hook;
    const uint64_t us = sysNowUs() - f->t0;
    uint8_t post[6][POST_BYTES];  // on the caller's stack: 3 KB
    size_t postLen[6];
    // The answer: what it returned, and the rows of 16 bytes the call
    // changed. Not the rest of what an argument points to: a register left
    // over from before can point to memory other threads change (on a PS4,
    // every GetPlayStatus and GetEvent was logged for that).
    uint64_t hash = hashBytes(0xCBF29CE484222325ull, &f->retRax, 8);
    for (int i = 0; i < 6; i++) {
        postLen[i] = 0;
        if (!f->preLen[i]) continue;
        postLen[i] = hookReadable((const void*)(uintptr_t)f->gpr[i], POST_BYTES);
        if (postLen[i]) memcpy(post[i], (const void*)(uintptr_t)f->gpr[i], postLen[i]);
        const size_t cmp = postLen[i] < (size_t)f->preLen[i] ? postLen[i] : (size_t)f->preLen[i];
        for (size_t at = 0; at + 16 <= cmp; at += 16)
            if (memcmp(post[i] + at, f->pre[i] + at, 16) != 0) {
                hash = hashBytes(hash + (uint64_t)(i * 64 + at), post[i] + at, 16);
            }
    }
    const uint64_t now = sysNowUs();
    if (h->quiet) {
        // A getter: when its answer changes, and not more than 5 times a
        // second.
        if ((hash == h->lastHash || now - h->lastLogUs < 200000u) && f->seq > 1) {
            __atomic_add_fetch(&h->same, 1, __ATOMIC_RELAXED);
            return;
        }
        h->lastHash = hash;
    }
    h->lastLogUs = now;
    const int first = h->logged++ == 0;
    const uint32_t same = __atomic_exchange_n(&h->same, 0, __ATOMIC_RELAXED);
    if (same) say("   (%s: %u calls since, not logged: the same answer)", h->name, same);
    say("%s #%u (%lx, %lx, %lx, %lx, %lx, %lx) = 0x%lX  [%u us, thread %d]", h->name, f->seq,
        f->gpr[0], f->gpr[1], f->gpr[2], f->gpr[3], f->gpr[4], f->gpr[5], f->retRax,
        (unsigned)us, scePthreadGetthreadid());
    if (!h->quiet) {
        const uint64_t* s = f->callerStack;
        if (hookReadable(s, 32) == 32)
            say("   stack: %lx %lx %lx %lx", s[0], s[1], s[2], s[3]);
    }
    for (int i = 0; i < 6; i++) {
        if (!f->preLen[i]) continue;
        char label[24];
        fmt(label, sizeof(label), "a%d in ", i);
        const size_t inLen = f->preLen[i] < IN_SHOWN ? (size_t)f->preLen[i] : IN_SHOWN;
        const size_t cmp = postLen[i] < (size_t)f->preLen[i] ? postLen[i] : (size_t)f->preLen[i];
        const int out = memcmp(post[i], f->pre[i], cmp) != 0 || postLen[i] > cmp;
        // What it pointed to before: always for a call that is not a
        // getter's; a getter's the first time only, else only the rows the
        // call changed.
        if (!h->quiet || first || out) say("   a%d -> %lx", i, f->gpr[i]);
        if (!h->quiet || first) dump(label, f->pre[i], inLen, NULL, 0);
        if (out) {
            fmt(label, sizeof(label), "a%d out", i);
            dump(label, post[i], postLen[i], f->pre[i], cmp);
        }
    }
}

#ifdef SC_DEBUG  // the assemblies carved from memory (mcw.carve): SC_DEBUG builds only (0.95)
// ── The shell's .NET assemblies, from its memory ─────────────────────────────
//
// On disk they are signed executables (.dll.sprx, encrypted); the shell runs
// them, so their metadata, the code's structures with their layouts, is in
// its memory as a plain PE file. Every readable region that is not direct
// (GPU) memory is searched, 16 bytes at a time, for "MZ" with a .NET PE
// header after it, and each one found is written to shell/asm-<address>.dll
// for a decompiler.

typedef struct {
    void* start;
    void* end;
    int64_t offset;
    int32_t prot;
    int32_t mtype;
    uint8_t flags;  // 1 flexible, 2 direct, 4 stack, 8 pooled, 16 committed
    char name[32];  // at 0x21: on a PS4 (11.50) the flags were one byte, the name right after
} VirtualQueryInfo;  // OpenOrbis's OrbisKernelVirtualQueryInfo
int sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, size_t size);
_Static_assert(__builtin_offsetof(VirtualQueryInfo, name) == 0x21, "the name after one byte of flags");

static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

// A .NET PE file at p (left bytes readable): its size, or 0.
static size_t netPe(const uint8_t* p, size_t left) {
    if (left < 0x200 || p[0] != 'M' || p[1] != 'Z') return 0;
    const uint32_t pe = rd32(p + 0x3C);
    if (pe < 0x40 || pe > 0x400 || pe + 0x18 + 0xF0 > left) return 0;
    if (rd32(p + pe) != 0x00004550u) return 0;  // "PE\0\0"
    const uint16_t sections = rd16(p + pe + 6), optSize = rd16(p + pe + 0x14);
    const uint8_t* opt = p + pe + 0x18;
    const uint16_t magic = rd16(opt);
    uint32_t cli;
    if (magic == 0x10B && optSize >= 0xE0)
        cli = rd32(opt + 0x60 + 14 * 8);
    else if (magic == 0x20B && optSize >= 0xF0)
        cli = rd32(opt + 0x70 + 14 * 8);
    else
        return 0;
    if (!cli || !sections || sections > 32) return 0;  // not .NET
    const uint8_t* sec = opt + optSize;
    if ((size_t)(sec - p) + sections * 40u > left) return 0;
    size_t size = 0;
    for (uint16_t i = 0; i < sections; i++) {
        const uint32_t raw = rd32(sec + i * 40 + 20), rawSize = rd32(sec + i * 40 + 16);
        if (raw + (size_t)rawSize > size) size = raw + (size_t)rawSize;
    }
    return size >= 0x200 && size <= (64u << 20) ? size : 0;
}

// The region being searched, written before and deleted after: if the
// shell goes down reading one, the next carve finds it here and adds it to
// mcw-carve-skip.txt, whose regions are never read again.
#define CARVING SC_BGM_DIR "/mcw.carving"
#define CARVE_SKIP SC_BGM_DIR "/mcw-carve-skip.txt"
static uint64_t g_skip[64];
static int g_nSkip;

static uint64_t parseHex(const char* s) {
    uint64_t v = 0;
    for (;; s++) {
        const char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (uint64_t)(c - 'A' + 10);
        else if (c == 'x' || c == 'X') v = 0;
        else return v;
    }
}

static void readSkips(void) {
    static char text[2048];
    char last[40];
    int fd = sysOpen(CARVING, SC_O_RDONLY, 0);
    if (fd >= 0) {
        const int n = sysRead(fd, last, sizeof(last) - 1);
        sysClose(fd);
        if (n > 0) {
            last[n] = 0;
            say("carve: the last one stopped in region %s: never read again", last);
            fd = sysOpen(CARVE_SKIP, SC_O_WRONLY | SC_O_CREAT, 0666);
            if (fd >= 0) {
                sysSeek(fd, 0, 2);
                sysWrite(fd, last, (size_t)n);
                sysWrite(fd, "\n", 1);
                sysClose(fd);
            }
        }
        sysUnlink(CARVING);
    }
    g_nSkip = 0;
    fd = sysOpen(CARVE_SKIP, SC_O_RDONLY, 0);
    if (fd < 0) return;
    const int n = sysRead(fd, text, sizeof(text) - 1);
    sysClose(fd);
    if (n <= 0) return;
    text[n] = 0;
    for (char* p = text; *p && g_nSkip < 64;) {
        const uint64_t v = parseHex(p);
        if (v) g_skip[g_nSkip++] = v;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    say("carve: %d region(s) skipped (mcw-carve-skip.txt)", g_nSkip);
}

static void carve(void) {
    sysMkdir(SC_BGM_DIR "/shell", 0777);
    readSkips();
    say("carve: looking for .NET assemblies in the shell's memory");
    drain();
    const uint8_t* at = (const uint8_t*)0x10000;
    int regions = 0, found = 0;
    uint64_t scanned = 0, lastHash[64];
    int nHash = 0;
    VirtualQueryInfo vq;
    for (int guard = 0; guard < 20000; guard++) {
        memset(&vq, 0, sizeof(vq));
        if (sceKernelVirtualQuery(at, 1 /* the next region */, &vq, sizeof(vq)) != 0) break;
        const uint8_t* start = vq.start;
        const uint8_t* end = vq.end;
        if (end <= at) break;
        at = end;
        vq.name[31] = 0;
        regions++;
        const size_t len = (size_t)(end - start);
        int skipped = 0;
        for (int i = 0; i < g_nSkip; i++) skipped |= g_skip[i] == (uint64_t)(uintptr_t)start;
        // CPU-readable, not direct (GPU) memory. (0.31 also asked for
        // "committed", which on a PS4 (11.50) most of the shell's memory is
        // not marked: 3 MB of it was searched.)
        const int readable = (vq.prot & 1) && !(vq.flags & 2) && len <= (512u << 20) && !skipped;
        if (len >= (1u << 20) || vq.name[0])
            say("  region %p-%p prot 0x%X type %d flags 0x%X %s%s", (const void*)start,
                (const void*)end, (unsigned)vq.prot, vq.mtype, vq.flags, vq.name,
                readable ? "" : " (skipped)");
        if (!readable) continue;
        char mark[40];
        const int mn = fmt(mark, sizeof(mark), "%p", (const void*)start);
        const int mfd = sysOpen(CARVING, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
        if (mfd >= 0) {
            sysWrite(mfd, mark, (size_t)mn);
            sysFsync(mfd);
            sysClose(mfd);
        }
        // Readable as it was queried; checked again before each 1 MB.
        for (size_t off = 0; off + 0x200 <= len; off += 16) {
            if ((off & 0xFFFFF) == 0 && !(hookProt(start + off) & 1)) break;
            const uint8_t* p = start + off;
            if (p[0] != 'M' || p[1] != 'Z') continue;
            const size_t size = netPe(p, len - off);
            if (!size || size > len - off) continue;
            const uint64_t h = hashBytes(0xCBF29CE484222325ull, p, size);
            int dup = 0;
            for (int i = 0; i < nHash; i++) dup |= lastHash[i] == h;
            if (dup) continue;
            if (nHash < 64) lastHash[nHash++] = h;
            char path[160];
            fmt(path, sizeof(path), SC_BGM_DIR "/shell/asm-%lx.dll", (uint64_t)(uintptr_t)p);
            const int fd = sysOpen(path, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
            const int wrote = fd >= 0 ? sysWrite(fd, p, size) : fd;
            if (fd >= 0) sysClose(fd);
            found++;
            say("  assembly at %p, %u bytes (in %s) -> %s: %d", (const void*)p, (unsigned)size,
                vq.name[0] ? vq.name : "?", path, wrote);
            drain();
            off += (size - 16) & ~(size_t)15;
        }
        scanned += len;
        drain();
    }
    sysUnlink(CARVING);
    say("carve: %d regions, %lu MB searched, %d assemblies written to shell/", regions,
        scanned >> 20, found);
    notify("Music watch: %d .NET assemblies copied to ShellCompanion/ps4/shell", found);
    drain();
}

#endif  // SC_DEBUG

// ── Finding the functions ────────────────────────────────────────────────────

static int contains(const char* s, const char* part) {
    const size_t n = strlen(part);
    for (; *s; s++)
        if (memcmp(s, part, n) == 0) return 1;
    return 0;
}

#define MAX_MODULES 16
static int g_mods[MAX_MODULES];
static char g_modNames[MAX_MODULES][64];
static int g_nMods;

// The loaded modules with Music in their name; all of them logged the first
// time.
static int findModules(int logAll) {
    static int handles[512];
    size_t count = 0;
    int rc = sceKernelGetModuleListInternal(handles, 512, &count);
    if (logAll) say("sceKernelGetModuleListInternal: 0x%08X, %u", (unsigned)rc, (unsigned)count);
    if (rc != 0 || count <= 1) {
        rc = sceKernelGetModuleList(handles, 512, &count);
        if (logAll) say("sceKernelGetModuleList: 0x%08X, %u", (unsigned)rc, (unsigned)count);
    }
    if (rc != 0) return 0;
    if (logAll) say("%u modules in the shell:", (unsigned)count);
    g_nMods = 0;
    static ModuleInfo info;
    for (size_t i = 0; i < count && i < 512; i++) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[i], &info) != 0) continue;
        info.name[255] = 0;
        if (logAll)
            say("  0x%08X %s at %p", (unsigned)handles[i], info.name,
                info.segmentCount ? info.segment[0].address : NULL);
        if ((contains(info.name, "Music") || contains(info.name, "libSceNpManager") ||
             contains(info.name, "libSceNpWebApi")) &&
            g_nMods < MAX_MODULES) {
            g_mods[g_nMods] = handles[i];
            fmt(g_modNames[g_nMods], sizeof(g_modNames[0]), "%s", info.name);
            g_nMods++;
        }
    }
    return g_nMods;
}

// A library by its path in the shell's sandbox, added to the music
// libraries. One the shell has loaded already comes back as its handle
// (0x122 for libSceMusicCoreServerClient on a PS4, 11.50): the same code
// the shell calls.
static void loadModule(const char* name) {
    char path[160];
    const char* word = sceKernelGetFsSandboxRandomWord();
    fmt(path, sizeof(path), "/%s/common/lib/%s", word ? word : "?", name);
    int res = 0;
    const int h = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
    say("loaded %s: 0x%08X (start 0x%X)", path, (unsigned)h, (unsigned)res);
    if (h < 0) return;
    for (int i = 0; i < g_nMods; i++)
        if (g_mods[i] == h) return;
    if (g_nMods >= MAX_MODULES) return;
    g_mods[g_nMods] = h;
    fmt(g_modNames[g_nMods], sizeof(g_modNames[0]), "%s", name);
    g_nMods++;
}

// The names to hook: mcw-names.txt's, else the default list. A short name
// gets sceMusicCoreServer before it.
static int g_nNames;
static char g_want[MAX_HOOKS][80];

static void readNames(void) {
    static char text[4096];
    const int fd = sysOpen(SC_BGM_DIR "/mcw-names.txt", SC_O_RDONLY, 0);
    int n = 0;
    if (fd >= 0) {
        n = sysRead(fd, text, sizeof(text) - 1);
        sysClose(fd);
    }
    g_nNames = 0;
    if (n > 0) {
        text[n] = 0;
        char* p = text;
        while (*p && g_nNames < MAX_HOOKS) {
            char* e = p;
            while (*e && *e != '\n' && *e != '\r') e++;
            const char c = *e;
            *e = 0;
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != '#') {
                if (p[0] == 's' && p[1] == 'c' && p[2] == 'e')
                    fmt(g_want[g_nNames++], sizeof(g_want[0]), "%s", p);
                else
                    fmt(g_want[g_nNames++], sizeof(g_want[0]), "sceMusicCoreServer%s", p);
            }
            p = c ? e + 1 : e;
        }
        say("functions from mcw-names.txt: %d", g_nNames);
        return;
    }
    for (int i = 0; i < N_DEFAULT && g_nNames < MAX_HOOKS; i++) {
        const char* n = g_defaultNames[i];
        if (n[0] == 's' && n[1] == 'c' && n[2] == 'e')
            fmt(g_want[g_nNames++], sizeof(g_want[0]), "%s", n);
        else
            fmt(g_want[g_nNames++], sizeof(g_want[0]), "sceMusicCoreServer%s", n);
    }
}

static int isGetter(const char* name) {
    if (contains(name, "BgmOutput")) return 1;  // once an audio buffer
    // PSN's: any that only asks (sceNpGetState, sceNpWebApiReadData, ...)
    if (name[0] == 's' && name[3] == 'N' && name[4] == 'p')
        return contains(name, "Get") || contains(name, "Check") || contains(name, "Read") ||
               contains(name, "IsPlus") || contains(name, "HasSigned");
    const char* s = name;
    if (contains(s, "sceMusicCoreServer")) s += 18;
    return (s[0] == 'G' && s[1] == 'e' && s[2] == 't') ||
           (s[0] == 'C' && s[1] == 'h' && s[2] == 'e' && s[3] == 'c' && s[4] == 'k');
}

static void hex16(char* out, size_t cap, const uint8_t* p) {
    size_t n = 0;
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 16 && n + 3 < cap; i++) {
        out[n++] = d[p[i] >> 4];
        out[n++] = d[p[i] & 15];
        out[n++] = ' ';
    }
    out[n ? n - 1 : 0] = 0;
}

static void installAll(void) {
    for (int w = 0; w < g_nNames; w++) {
        for (int m = 0; m < g_nMods && g_nHooks < MAX_HOOKS; m++) {
            void* fn = NULL;
            if (sceKernelDlsym(g_mods[m], g_want[w], &fn) != 0 || !fn) continue;
            int dup = 0;
            for (int i = 0; i < g_nHooks; i++)
                if (g_hooks[i].target == fn) dup = 1;
            if (dup) {
                say("%s (%s): %p, hooked already", g_want[w], g_modNames[m], fn);
                continue;
            }
            Hook* h = &g_hooks[g_nHooks];
            const int skip = contains(g_want[w], "sceMusicCoreServer") ? 18 : 0;
            fmt(g_names[g_nHooks], sizeof(g_names[0]), "%s", g_want[w] + skip);
            h->name = g_names[g_nHooks];
            h->quiet = isGetter(g_want[w]);
            char bytes[64];
            hex16(bytes, sizeof(bytes), fn);
            say("%s (%s): %p: %s", g_want[w], g_modNames[m], fn, bytes);
            const char* why = "";
            lock(&g_installLock);
            const int rc = hookInstall(h, fn, &why);
            if (rc == 0) g_nHooks++;
            unlock(&g_installLock);
            if (rc == 0)
                say("   hooked: %u bytes moved, stub %p%s", h->stolen, h->stub,
                    h->quiet ? ", logged when its answer changes" : "");
            else
                say("   NOT hooked (%d): %s", rc, why);
            drain();
        }
    }
    say("%d functions hooked; %s", g_nHooks, g_hookHow);
}

static void* watchMain(void* arg) {
    (void)arg;
    g_began = sysNowUs();
    sysMkdir("/user/data/ShellCompanion", 0777);
    sysMkdir(SC_BGM_DIR, 0777);
    g_buf[0] = sysMap(BUF);
    g_buf[1] = sysMap(BUF);
    sysUnlink(LOG_OLD);
    g_fd = sysOpen(LOG_PATH, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    char fw[64];
    firmware(fw, sizeof(fw));
    say("music watch: inside the shell, firmware %s", fw);
    g_quiet = !sysExists(SC_BGM_DIR "/mcw-verbose");
    drain();
    if (sysExists(SC_BGM_DIR "/mcw.off")) {
        say("mcw.off is there: nothing hooked");
        drain();
        return NULL;
    }
    readNames();
    // The music server's client libraries: those the list shows, then each
    // by its path, which gives the shell's own handle if it has it.
    findModules(1);
    drain();
    loadModule("libSceMusicCoreServerClient.sprx");
    loadModule("libSceMusicCoreServerClientJsEx.sprx");
    for (int i = 0; i < g_nMods; i++)
        say("music library: 0x%08X %s", (unsigned)g_mods[i], g_modNames[i]);
    drain();
    installAll();
    drain();
    if (!sysExists(SC_BGM_DIR "/mcw-nojson")) monoHooks();
    drain();
    notify("Music watch: %d functions watched. Play music, then use the quick menu", g_nHooks);
    drain();

    // Writing the log, and how often each was called.
    uint64_t lastSummary = sysNowUs();
    uint32_t lastCalls[MAX_HOOKS];
    memset(lastCalls, 0, sizeof(lastCalls));
    uint64_t lastBeat = 0;
    for (;;) {
        sysSleepMs(200);
        fakeRefresh();
        const uint64_t now = sysNowUs();
        static uint64_t lastSetup;
        if (now - lastSetup >= 2000000u) {
            lastSetup = now;
            setupAgain();
        }
        if (now - lastBeat >= 1000000u) {
            // Alive, for the client library (scOpen loads the watch only
            // when this is old: a second copy would hook everything twice).
            lastBeat = now;
            char beat[48];
            const int bn = fmt(beat, sizeof(beat), "beat_ms=%lu\n", (unsigned long)(now / 1000u));
            const int bfd = sysOpen(SC_BGM_DIR "/mcw-beat.txt", SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
            if (bfd >= 0) {
                sysWrite(bfd, beat, (size_t)bn);
                sysClose(bfd);
            }
        }
        if (now - lastSummary >= 10000000u) {
            lastSummary = now;
            // Each call logged only with mcw-verbose (0.95: always unless
            // mcw.quiet before; mcw.log grew by megabytes).
            g_quiet = !sysExists(SC_BGM_DIR "/mcw-verbose");
#ifdef SC_DEBUG
            if (sysExists(SC_BGM_DIR "/mcw.carve")) {
                sysUnlink(SC_BGM_DIR "/mcw.carve");
                carve();
            }
#endif
            if (!g_quiet) {
                char line[512];
                int n = 0;
                for (int i = 0; i < g_nHooks && n < (int)sizeof(line) - 64; i++) {
                    const uint32_t c = g_hooks[i].calls;
                    if (c != lastCalls[i])
                        n += fmt(line + n, sizeof(line) - n, " %s %u", g_hooks[i].name,
                                 c - lastCalls[i]);
                    lastCalls[i] = c;
                }
                if (n) say("calls in the last 10 s:%s", line);
            }
        }
        drain();
    }
    return NULL;
}

// The watch's start: its thread (ps4/companion.c).
void mcwStart(void) {
    threadStart(watchMain, NULL, "ScMcw");
}

#ifndef SC_COMPANION
__attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    mcwStart();
    return 0;
}

__attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}
#endif
