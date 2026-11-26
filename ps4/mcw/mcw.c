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
 *   mcw-fake.txt   while there, and our player (sc_bgm) plays, the quick menu
 *                  is answered for it (fakeApply): its first word is the
 *                  title the answers name, SCMP00001 (the test app) if none
 *   mcw.carve      when it appears (the test app's DOWN), the .NET assemblies
 *                  in the shell's memory are copied to shell/, then it is
 *                  deleted
 */

#include "../bgm/bgm.h"
#include "../probe/sys.h"
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
};
#define N_DEFAULT (int)(sizeof(g_defaultNames) / sizeof(g_defaultNames[0]))

#define MAX_HOOKS 72
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
// As the USB Music Player is answered (README, "the protocol"), after the
// real call: GetPlayStatus's 0x70 bytes with our player's state, length and
// position and a title of our choosing; GetEvent's event 2 ("look again")
// when that changes and once a second; PlayControl's play (1) and pause
// (3) sent to our player as its cmd file. Only while mcw-fake.txt is there
// and our player has a track, playing or paused.

#define FAKE SC_BGM_DIR "/mcw-fake.txt"
static struct {
    volatile int active;
    volatile uint32_t state;  // 2 playing, 3 paused
    volatile uint64_t durS, posS;
    volatile int event;  // event 2 owed to the next GetEvent
    char title[16];
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

static void fakeRefresh(void) {
    static char text[1024];
    char v[64];
    int on = 0;
    int fd = sysOpen(FAKE, SC_O_RDONLY, 0);
    if (fd >= 0) {
        const int n = sysRead(fd, v, sizeof(v) - 1);
        sysClose(fd);
        v[n > 0 ? n : 0] = 0;
        size_t k = 0;
        for (const char* p = v; *p && *p != '\n' && *p != '\r' && *p != ' ' && k < 15; p++)
            g_fake.title[k++] = *p;
        if (!k) memcpy(g_fake.title, "SCMP00001", (k = 9));
        g_fake.title[k] = 0;
        fd = sysOpen(SC_BGM_STATUS, SC_O_RDONLY, 0);
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
                if (state != g_fake.state || pos != g_fake.posS || !g_fake.active) g_fake.event = 1;
                if (state != g_fake.state || !g_fake.active)
                    say("fake: answering for our player as %s: %s, %lu of %lu s", g_fake.title,
                        state == 2 ? "playing" : "paused", pos, g_fake.durS);
                g_fake.state = state;
                g_fake.posS = pos;
            }
        }
    }
    if (!on && g_fake.active) {
        say("fake: off (%s)", fd >= 0 || sysExists(FAKE) ? "our player has no track" : "no mcw-fake.txt");
        g_fake.event = 1;
    }
    g_fake.active = on;
}

static void put32At(uint8_t* p, size_t at, uint32_t v) {
    memcpy(p + at, &v, 4);
}
static void put64At(uint8_t* p, size_t at, uint64_t v) {
    memcpy(p + at, &v, 8);
}

static void sendPlayer(const char* cmd) {
    const int fd = sysOpen(SC_BGM_CMD, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (fd >= 0) {
        sysWrite(fd, cmd, strlen(cmd));
        sysClose(fd);
    }
}

// After the real call, before it is logged: the answer changed for ours.
static void fakeApply(Frame* f) {
    if (!g_fake.active) return;
    const Hook* h = f->hook;
    if (eq(h->name, "GetPlayStatus")) {
        uint8_t* p = (uint8_t*)(uintptr_t)f->gpr[0];
        if (hookReadable(p, 0x70) < 0x70 || !(hookProt(p) & 2)) return;
        memset(p, 0, 0x70);
        put32At(p, 0x04, g_fake.state);
        put32At(p, 0x08, 100);
        put64At(p, 0x10, g_fake.durS);
        put64At(p, 0x18, g_fake.posS);
        put32At(p, 0x2C, 1);
        put32At(p, 0x30, 0x8000);
        put32At(p, 0x44, 1);
        put32At(p, 0x48, 0xFFFFFFFEu);  // the core's id, as the USB Music Player's
        put32At(p, 0x54, 1);
        put32At(p, 0x58, 1);
        memcpy(p + 0x60, g_fake.title, 16);
        f->retRax = 0;
    } else if (eq(h->name, "GetEvent")) {
        uint8_t* p = (uint8_t*)(uintptr_t)f->gpr[0];
        if ((uint32_t)f->retRax != 0x80FD0014u || !g_fake.event) return;
        if (hookReadable(p, 4) < 4 || !(hookProt(p) & 2)) return;
        g_fake.event = 0;
        put32At(p, 0, 2);
        f->retRax = 0;
    } else if (eq(h->name, "PlayControl")) {
        const uint32_t cmd = (uint32_t)f->gpr[1];
        say("fake: PlayControl command %u (core %d): %s", cmd, (int32_t)f->gpr[0],
            cmd == 1 ? "play" : cmd == 3 ? "pause" : "not known yet, nothing sent");
        if (cmd == 1) sendPlayer("play\n");
        if (cmd == 3) sendPlayer("pause\n");
        g_fake.event = 1;
        f->retRax = 0;
    }
}

void mcwPre(Frame* f) {
    f->t0 = sysNowUs();
    Hook* h = f->hook;
    f->seq = __atomic_add_fetch(&h->calls, 1, __ATOMIC_RELAXED);
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

void mcwPost(Frame* f) {
    fakeApply(f);
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
    uint32_t flags;  // 1 flexible, 2 direct, 4 stack, 8 pooled, 16 committed
    char name[32];
} VirtualQueryInfo;  // OpenOrbis's OrbisKernelVirtualQueryInfo
int sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, size_t size);

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
        // CPU-readable, committed, not direct (GPU) memory.
        const int readable = (vq.prot & 1) && (vq.flags & 16) && !(vq.flags & 2) &&
                             len <= (512u << 20) && !skipped;
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

// ── Finding the functions ────────────────────────────────────────────────────

static int contains(const char* s, const char* part) {
    const size_t n = strlen(part);
    for (; *s; s++)
        if (memcmp(s, part, n) == 0) return 1;
    return 0;
}

#define MAX_MODULES 8
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
        if (contains(info.name, "Music") && g_nMods < MAX_MODULES) {
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
    notify("Music watch: %d functions watched. Play music, then use the quick menu", g_nHooks);
    drain();

    // Writing the log, and how often each was called.
    uint64_t lastSummary = sysNowUs();
    uint32_t lastCalls[MAX_HOOKS];
    memset(lastCalls, 0, sizeof(lastCalls));
    for (;;) {
        sysSleepMs(200);
        fakeRefresh();
        const uint64_t now = sysNowUs();
        if (now - lastSummary >= 10000000u) {
            lastSummary = now;
            g_quiet = sysExists(SC_BGM_DIR "/mcw.quiet");
            if (sysExists(SC_BGM_DIR "/mcw.carve")) {
                sysUnlink(SC_BGM_DIR "/mcw.carve");
                carve();
            }
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

__attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    threadStart(watchMain, NULL, "ScMcw");
    return 0;
}

__attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}
