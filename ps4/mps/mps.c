/**
 * The PS4's own music service from inside the shell: a probe, loaded into
 * SceShellUI by the test app (L2), as the background music module is. If the
 * system's music service (libSceMusicPlayerService, a client of
 * SceMusicCoreServer, NPXS21007) will play a track handed to it from here,
 * the quick menu's Music section should show it, with its own buttons, as
 * the Vita's quick menu does for the shell's music player.
 *
 * Arguments are shadPS4's emulation of the library (its
 * src/core/libraries/music), not Sony's; a payload's try hung in
 * Initialize3. So every call runs on the probe's thread and is logged
 * before it is made, and a second thread logs every 2 s the call still
 * running: a hang shows as one, a crash as the log's end, and the shell is
 * never waited on.
 *
 * Tracks tried, each as a path and as a file:// URI: music/yoursong.mp3,
 * then music/test-tone.wav, in /user/data/ShellCompanion/ps4. Once the
 * service's clock moves it is left playing for 60 s, its status logged
 * each second, for the quick menu to be looked at and its buttons pressed;
 * then stopped, unless mps.keep is there. The log is ps4/mps.log.
 */

#include "../bgm/bgm.h"
#include "../probe/sys.h"

int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, uint32_t flags,
                             const void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addr);

typedef int32_t (*FnInit3)(size_t poolSize, void* pool, uint64_t a, uint64_t b);
typedef int32_t (*FnInit)(size_t poolSize, void* pool);
typedef int32_t (*FnSetTrackList)(int32_t player, const void* tracks, uint32_t count);
typedef int32_t (*FnSetTrackList2)(int32_t player, const void* tracks, uint32_t count,
                                   uint32_t flags);
typedef int32_t (*FnPlay)(int32_t player, uint32_t track, int64_t startS, int32_t flags);
typedef int32_t (*FnVoid)(void);
typedef int32_t (*FnStatus)(void* status);

#define ENTRY 0xC2F   // SetTrackList's entry, its URI at its start (shadPS4)
#define ENTRY2 0xC48  // SetTrackList2's
#define POOL (4u * 1024u * 1024u)

typedef struct {
    uint32_t result, playStatus, trackIndex, tracklistId;
    uint64_t playbackTimeS;
    uint32_t shuffle, repeat;
} CurrentStatus;  // GetCurrentPlayStatus's, 0x20

typedef struct {
    uint64_t size;  // 0x48
    uint32_t errorCode, playState;
    uint64_t totalS, currentS;
    uint32_t tracklistId, trackIndex, playIndex, trackCount, volume, repeat, shuffle, cannotNext,
        cannotPrevious, loading;
} StatusExt;  // GetPlayStatusExtension's, 0x48

static FnInit3 g_init3;
static FnInit g_init, g_init2;
static FnSetTrackList g_set;
static FnSetTrackList2 g_set2;
static FnPlay g_play;
static FnVoid g_stop, g_terminate;
static FnStatus g_current, g_ext;

static const char* volatile g_step = "starting";
static volatile uint64_t g_stepUs;
static volatile int g_done;

static void step(const char* what) {
    g_step = what;
    g_stepUs = sysNowUs();
    logLine("calling %s", what);
}

static void* watch(void* arg) {
    (void)arg;
    while (!g_done) {
        sysSleepMs(2000);
        if (g_done) break;
        logLine("  ... still in %s, %u ms", g_step,
                (unsigned)((sysNowUs() - g_stepUs) / 1000u));
    }
    return NULL;
}

static CurrentStatus g_cs;
static StatusExt g_es;

static void statusLine(char* out, size_t cap) {
    memset(&g_cs, 0, sizeof(g_cs));
    memset(&g_es, 0, sizeof(g_es));
    g_es.size = sizeof(g_es);
    const int32_t rc1 = g_current ? g_current(&g_cs) : -1;
    const int32_t rc2 = g_ext ? g_ext(&g_es) : -1;
    fmt(out, cap,
        "current 0x%08X: status %u, track %u, at %lu s | ext 0x%08X: error 0x%X, state %u, "
        "%lu/%lu s, track %u of %u, loading %u",
        (unsigned)rc1, g_cs.playStatus, g_cs.trackIndex, (uint64_t)g_cs.playbackTimeS,
        (unsigned)rc2, g_es.errorCode, g_es.playState, (uint64_t)g_es.currentS,
        (uint64_t)g_es.totalS, g_es.trackIndex, g_es.trackCount, g_es.loading);
}

// One try: the track list (the URI twice, so next and previous show), play,
// and 15 s of looks. 1 when its clock moves.
static int tryUri(const char* uri) {
    static uint8_t entries[ENTRY2 * 2];
    const size_t n = strlen(uri) + 1;
    memset(entries, 0, sizeof(entries));
    memcpy(entries, uri, n);
    memcpy(entries + ENTRY, uri, n);
    logLine("try \"%s\"", uri);
    int32_t rc = -1;
    if (g_set) {
        step("SetTrackList(0, 2 entries of 0xC2F)");
        rc = g_set(0, entries, 2);
        logLine("  SetTrackList: 0x%08X", (unsigned)rc);
    }
    if (rc < 0 && g_set2) {
        memset(entries, 0, sizeof(entries));
        memcpy(entries, uri, n);
        memcpy(entries + ENTRY2, uri, n);
        step("SetTrackList2(0, 2 entries of 0xC48, flags 0)");
        rc = g_set2(0, entries, 2, 0);
        logLine("  SetTrackList2: 0x%08X", (unsigned)rc);
    }
    if (rc < 0 || !g_play) return 0;
    step("Play(0, track 0, from 0 s, flags 0)");
    rc = g_play(0, 0, 0, 0);
    logLine("  Play: 0x%08X", (unsigned)rc);
    if (rc < 0) return 0;
    char line[512];
    uint64_t first = (uint64_t)-1;
    for (int i = 0; i < 30; i++) {
        sysSleepMs(500);
        step("the play status");
        statusLine(line, sizeof(line));
        if (i % 4 == 0) logLine("  %2d: %s", i, line);
        const uint64_t t = g_es.currentS ? g_es.currentS : g_cs.playbackTimeS;
        if (first == (uint64_t)-1) first = t;
        if (t > first) {
            logLine("  its clock moves (%lu s to %lu s): the service plays it", first, t);
            return 1;
        }
    }
    logLine("  its clock did not move in 15 s");
    if (g_stop) logLine("  Stop: 0x%08X", (unsigned)g_stop());
    return 0;
}

static void* sym(int lib, const char* name) {
    void* p = NULL;
    const int rc = sceKernelDlsym(lib, name, &p);
    logLine("  %s: %p (0x%08X)", name, p, (unsigned)rc);
    return rc == 0 ? p : NULL;
}

static void* probe(void* arg) {
    (void)arg;
    sysMkdir("/user/data/ShellCompanion", 0777);
    sysMkdir(SC_BGM_DIR, 0777);
    logOpen("ps4/mps.log");
    char fw[64];
    firmware(fw, sizeof(fw));
    logLine("music service probe: inside the shell, firmware %s", fw);
    threadStart(watch, NULL, "ScMpsWatch");

    int res = 0, lib = -1;
    step("sceKernelLoadStartModule(libSceMusicPlayerService.sprx)");
    lib = sceKernelLoadStartModule("libSceMusicPlayerService.sprx", 0, NULL, 0, NULL, &res);
    logLine("libSceMusicPlayerService.sprx: 0x%08X (start 0x%X)", (unsigned)lib, res);
    if (lib < 0) {
        step("sceKernelLoadStartModule(/system/common/lib/...)");
        lib = sceKernelLoadStartModule("/system/common/lib/libSceMusicPlayerService.sprx", 0, NULL,
                                       0, NULL, &res);
        logLine("by its path: 0x%08X (start 0x%X)", (unsigned)lib, res);
    }
    if (lib < 0) {
        notify("Music service probe: no libSceMusicPlayerService (0x%08X)", (unsigned)lib);
        g_done = 1;
        return NULL;
    }
    g_init3 = (FnInit3)sym(lib, "sceMusicPlayerServiceInitialize3");
    g_init2 = (FnInit)sym(lib, "sceMusicPlayerServiceInitialize2");
    g_init = (FnInit)sym(lib, "sceMusicPlayerServiceInitialize");
    g_set = (FnSetTrackList)sym(lib, "sceMusicPlayerServiceSetTrackList");
    g_set2 = (FnSetTrackList2)sym(lib, "sceMusicPlayerServiceSetTrackList2");
    g_play = (FnPlay)sym(lib, "sceMusicPlayerServicePlay");
    g_stop = (FnVoid)sym(lib, "sceMusicPlayerServiceStop");
    g_terminate = (FnVoid)sym(lib, "sceMusicPlayerServiceTerminate");
    g_current = (FnStatus)sym(lib, "sceMusicPlayerServiceGetCurrentPlayStatus");
    g_ext = (FnStatus)sym(lib, "sceMusicPlayerServiceGetPlayStatusExtension");

    void* pool = sysMap(POOL);
    logLine("pool: %p, %u bytes", pool, POOL);
    int32_t rc = -1;
    if (g_init && pool) {
        step("Initialize(pool)");
        rc = g_init(POOL, pool);
        logLine("Initialize: 0x%08X", (unsigned)rc);
    }
    if (rc < 0 && g_init2 && pool) {
        step("Initialize2(pool)");
        rc = g_init2(POOL, pool);
        logLine("Initialize2: 0x%08X", (unsigned)rc);
    }
    if (rc < 0 && g_init3 && pool) {
        step("Initialize3(pool, 0, 0)");
        rc = g_init3(POOL, pool, 0, 0);
        logLine("Initialize3: 0x%08X", (unsigned)rc);
    }
    if (rc < 0 && (uint32_t)rc != 0x812F0002u /* already */) {
        notify("Music service probe: it would not initialise (0x%08X)", (unsigned)rc);
        g_done = 1;
        return NULL;
    }
    char line[512];
    step("the play status, before");
    statusLine(line, sizeof(line));
    logLine("before: %s", line);

    static const char* const names[2] = {"yoursong.mp3", "test-tone.wav"};
    int played = 0;
    for (int i = 0; i < 2 && !played; i++) {
        char path[160], uri[180];
        fmt(path, sizeof(path), SC_BGM_MUSIC "/%s", names[i]);
        if (!sysExists(path)) {
            logLine("%s is not there", path);
            continue;
        }
        played = tryUri(path);
        if (!played) {
            fmt(uri, sizeof(uri), "file://%s", path);
            played = tryUri(uri);
        }
    }
    if (!played) {
        notify("Music service probe: the service played nothing; send mps.log");
        g_done = 1;
        return NULL;
    }
    notify("Music service probe: playing. Open the quick menu (hold PS)");
    // 60 s for the quick menu: its buttons show as changes here.
    for (int i = 0; i < 60; i++) {
        sysSleepMs(1000);
        step("the play status, for the quick menu");
        statusLine(line, sizeof(line));
        logLine("  %2d s: %s", i, line);
    }
    if (!sysExists(SC_BGM_DIR "/mps.keep") && g_stop) {
        step("Stop");
        logLine("Stop: 0x%08X", (unsigned)g_stop());
    }
    logLine("music service probe: done");
    notify("Music service probe done: send mps.log");
    g_done = 1;
    return NULL;
}

__attribute__((visibility("default"))) int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    threadStart(probe, NULL, "ScMps");
    return 0;
}

__attribute__((visibility("default"))) int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}
