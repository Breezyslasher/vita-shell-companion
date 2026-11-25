/**
 * PS4 probe 2: will the system's own music service play a track handed to
 * it from homebrew, and does it then appear in the quick menu, with its
 * buttons working? On the Vita the plugin plays its queue through SceShell's
 * music service, which gives it the quick menu's Music row; this asks
 * whether the PS4's libSceMusicPlayerService (SceMusicCoreServer, NPXS21007)
 * can do the same.
 *
 * The calls' arguments are shadPS4's emulation of the library (its
 * src/core/libraries/music, PR #5392), not known from a console: so every
 * call is logged before it is made, and a hang shows as a last line with
 * no answer. The track list holds the same file twice, so that the quick
 * menu's next and previous show as a change of track, and pause as a change
 * of state, in the status read every 500 ms; ReceiveEvent, which may wait,
 * is not called.
 *
 * Files tried, each as a path and as a file:// URI: test.mp3 (copied there
 * by you) if it is there, then probe-tone.wav, made here: 30 s of 440 Hz
 * beeps. All in /data/ShellCompanion; the log is ps4probe-music.log there.
 *
 * The service is driven from a thread of its own, the main thread watching
 * it: each call is a step (step()), and a step that has not returned is
 * logged every 2 s, so that a call that hangs shows as one, a crash as the
 * log's end. The first run hung, or crashed, in Initialize3 with its last
 * two arguments 0 (GoldHEN 2.4b18.6, firmware 11.50): Initialize, with the
 * pool alone, is tried first now.
 */

#include "sys.h"

typedef int32_t (*FnInit)(size_t poolSize, void* pool, uint64_t a, uint64_t b);
typedef int32_t (*FnSetTrackList)(int32_t player, const void* tracks, uint32_t count, uint32_t flags);
typedef int32_t (*FnPlay)(int32_t player, uint32_t track, int64_t startS, int32_t flags);
typedef int32_t (*FnVoid)(void);
typedef int32_t (*FnStatus)(void* status);
typedef int32_t (*FnLoadModule)(uint32_t id);

#define DIR "/data/ShellCompanion"
#define ENTRY 0xC2F   // SetTrackList's entry, its URI at its start (shadPS4)
#define ENTRY2 0xC48  // SetTrackList2's
#define POOL (4u * 1024u * 1024u)

typedef struct {
    uint32_t result, playStatus, trackIndex, tracklistId;
    uint64_t playbackTimeS;
    uint32_t shuffle, repeat;
} CurrentStatus;  // sceMusicPlayerServiceGetCurrentPlayStatus's, 0x20

typedef struct {
    uint64_t size;  // 0x48
    uint32_t errorCode, playState;
    uint64_t totalS, currentS;
    uint32_t tracklistId, trackIndex, playIndex, trackCount, volume, repeat, shuffle, cannotNext,
        cannotPrevious, loading;
} StatusExt;  // sceMusicPlayerServiceGetPlayStatusExtension's, 0x48

static FnInit g_init3, g_init2, g_init;

// The step the service's thread is in, and since when; done once it ends.
static const char* volatile g_step = "starting";
static volatile uint64_t g_stepUs;
static volatile int g_done;

static void step(const char* what) {
    g_step = what;
    g_stepUs = sysNowUs();
    logLine("calling %s", what);
}
static FnSetTrackList g_set, g_set2;
static FnPlay g_play;
static FnVoid g_stop, g_terminate;
static FnStatus g_current, g_ext;

// 30 s of 440 Hz beeps as a WAV: 48 kHz, 16-bit, stereo.
static int writeTone(const char* path) {
    const uint32_t rate = 48000, frames = rate * 30, data = frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    const uint32_t riff = data + 36, sixteen = 16, byteRate = rate * 4;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &sixteen, 4);
    const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byteRate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    const int fd = sysOpen(path, SC_O_WRONLY | SC_O_CREAT | SC_O_TRUNC, 0666);
    if (fd < 0) return fd;
    int rc = sysWrite(fd, h, sizeof(h)) == (int)sizeof(h) ? 0 : -1;
    static int16_t buf[4800 * 2];
    uint16_t phase = 0;
    const uint32_t step = 440u * 65536u / rate;
    for (uint32_t f = 0; f < frames && rc == 0; f += 4800) {
        for (int i = 0; i < 4800; i++) {
            int16_t v = 0;
            if ((f + i) % rate < rate * 8 / 10) {
                phase = (uint16_t)(phase + step);
                const int tri = phase < 32768 ? phase : 65535 - phase;
                v = (int16_t)((tri - 16384) / 3);
            }
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        if (sysWrite(fd, buf, sizeof(buf)) != (int)sizeof(buf)) rc = -1;
    }
    sysClose(fd);
    return rc;
}

static CurrentStatus g_cs;
static StatusExt g_es;

// The status, both ways, as one line; 1 when it differs from the last.
static int readStatus(char* out, size_t cap) {
    static char last[256];
    CurrentStatus cs;
    StatusExt es;
    memset(&cs, 0, sizeof(cs));
    memset(&es, 0, sizeof(es));
    es.size = sizeof(es);
    const int32_t rc1 = g_current ? g_current(&cs) : -1;
    const int32_t rc2 = g_ext ? g_ext(&es) : -1;
    g_cs = cs;
    g_es = es;
    fmt(out, cap,
        "current (0x%08X): result 0x%X, status %u, track %u, list %u, at %lu s, shuffle %u, repeat "
        "%u | ext (0x%08X): error 0x%X, state %u, %lu/%lu s, track %u of %u, volume %u, "
        "no-next %u, no-previous %u, loading %u",
        (unsigned)rc1, cs.result, cs.playStatus, cs.trackIndex, cs.tracklistId,
        (uint64_t)cs.playbackTimeS, cs.shuffle, cs.repeat, (unsigned)rc2, es.errorCode,
        es.playState, (uint64_t)es.currentS, (uint64_t)es.totalS, es.trackIndex, es.trackCount,
        es.volume, es.cannotNext, es.cannotPrevious, es.loading);
    // Only what changes: not the clock, which goes on.
    char key[256];
    fmt(key, sizeof(key), "%X %X %u %u %u %u %u %u", (unsigned)rc1, (unsigned)rc2, cs.playStatus,
        cs.trackIndex, es.playState, es.trackIndex, es.errorCode, es.loading);
    int changed = 0;
    for (size_t i = 0; i < sizeof(key); i++) {
        if (key[i] != last[i]) changed = 1;
        if (!key[i]) break;
    }
    memcpy(last, key, sizeof(last));
    return changed;
}

// One try: the track list (the URI twice), play, and 15 s of looks. 1 when
// the clock was seen to move, as the service played it.
static int tryUri(const char* uri) {
    static uint8_t entries[ENTRY2 * 2];
    memset(entries, 0, sizeof(entries));
    const size_t n = strlen(uri) + 1;
    memcpy(entries, uri, n);
    memcpy(entries + ENTRY, uri, n);
    logLine("try \"%s\"", uri);
    step("SetTrackList(0, 2 entries of 0xC2F)");
    int32_t rc = g_set ? g_set(0, entries, 2, 0) : -1;
    logLine("  SetTrackList: 0x%08X", (unsigned)rc);
    if (rc < 0 && g_set2) {
        memset(entries, 0, sizeof(entries));
        memcpy(entries, uri, n);
        memcpy(entries + ENTRY2, uri, n);
        step("SetTrackList2(0, 2 entries of 0xC48, flags 0)");
        rc = g_set2(0, entries, 2, 0);
        logLine("  SetTrackList2: 0x%08X", (unsigned)rc);
    }
    if (rc < 0) return 0;
    step("Play(0, track 0, from 0 s, flags 0)");
    rc = g_play(0, 0, 0, 0);
    logLine("  Play: 0x%08X", (unsigned)rc);
    if (rc < 0) return 0;
    char line[512];
    uint64_t firstTime = (uint64_t)-1;
    for (int i = 0; i < 30; i++) {
        sysSleepMs(500);
        step("the play status");
        const int changed = readStatus(line, sizeof(line));
        if (changed || i == 0 || i == 29) logLine("  %3d: %s", i, line);
        const uint64_t t = g_es.currentS ? g_es.currentS : g_cs.playbackTimeS;
        if (firstTime == (uint64_t)-1) firstTime = t;
        if (t > firstTime) {
            logLine("  its clock moves (%lu s to %lu s): the service plays it", firstTime, t);
            return 1;
        }
    }
    logLine("  its clock did not move in 15 s");
    if (g_stop) logLine("  Stop: 0x%08X", (unsigned)g_stop());
    return 0;
}

static int musicRun(void) {
    sysMkdir(DIR, 0777);
    const int rcTone = writeTone(DIR "/probe-tone.wav");
    logLine("probe-tone.wav written: %d", rcTone);

    int lib = libLoad("libSceMusicPlayerService.sprx");
    logLine("libSceMusicPlayerService.sprx: handle 0x%X", lib);
    if (!lib) {
        // Its sysmodule (0xDD, psdevwiki), loaded for this process.
        const int sm = libLoad("libSceSysmodule.sprx");
        FnLoadModule load = (FnLoadModule)libSym(sm, "sceSysmoduleLoadModule");
        logLine("libSceSysmodule: 0x%X, sceSysmoduleLoadModule %p", sm, (void*)load);
        if (load) logLine("sceSysmoduleLoadModule(0xDD): 0x%08X", (unsigned)load(0xDD));
        lib = libLoad("libSceMusicPlayerService.sprx");
        logLine("libSceMusicPlayerService.sprx again: handle 0x%X", lib);
    }
    if (!lib) {
        notify("Music probe: the music service would not load; see its log");
        return 1;
    }
    g_init3 = (FnInit)libSym(lib, "sceMusicPlayerServiceInitialize3");
    g_init2 = (FnInit)libSym(lib, "sceMusicPlayerServiceInitialize2");
    g_init = (FnInit)libSym(lib, "sceMusicPlayerServiceInitialize");
    g_set = (FnSetTrackList)libSym(lib, "sceMusicPlayerServiceSetTrackList");
    g_set2 = (FnSetTrackList)libSym(lib, "sceMusicPlayerServiceSetTrackList2");
    g_play = (FnPlay)libSym(lib, "sceMusicPlayerServicePlay");
    g_stop = (FnVoid)libSym(lib, "sceMusicPlayerServiceStop");
    g_terminate = (FnVoid)libSym(lib, "sceMusicPlayerServiceTerminate");
    g_current = (FnStatus)libSym(lib, "sceMusicPlayerServiceGetCurrentPlayStatus");
    g_ext = (FnStatus)libSym(lib, "sceMusicPlayerServiceGetPlayStatusExtension");
    logLine("found: init3 %p, init2 %p, init %p, set %p, set2 %p, play %p, stop %p, terminate %p, "
            "current %p, ext %p",
            (void*)g_init3, (void*)g_init2, (void*)g_init, (void*)g_set, (void*)g_set2,
            (void*)g_play, (void*)g_stop, (void*)g_terminate, (void*)g_current, (void*)g_ext);
    if (!g_play || (!g_set && !g_set2)) {
        notify("Music probe: the music service lacks its calls; see its log");
        return 1;
    }

    void* pool = sysMap(POOL);
    logLine("pool: %p, %u KB", pool, POOL / 1024);
    int32_t rc = -1;
    if (g_init) {
        step("Initialize(pool)");
        rc = g_init(POOL, pool, 0, 0);
        logLine("Initialize: 0x%08X", (unsigned)rc);
    }
    if (rc < 0 && g_init2) {
        step("Initialize2(pool)");
        rc = g_init2(POOL, pool, 0, 0);
        logLine("Initialize2: 0x%08X", (unsigned)rc);
    }
    if (rc < 0 && g_init3) {
        step("Initialize3(pool)");
        rc = g_init3(POOL, pool, 0, 0);
        logLine("Initialize3: 0x%08X", (unsigned)rc);
    }
    char line[512];
    step("the play status, before playing");
    readStatus(line, sizeof(line));
    logLine("status before playing: %s", line);
    notify("Music probe: trying the system music service");

    const char* files[2] = {DIR "/test.mp3", DIR "/probe-tone.wav"};
    int played = 0;
    for (int f = 0; f < 2 && !played; f++) {
        if (!sysExists(files[f])) {
            logLine("%s is not there", files[f]);
            continue;
        }
        char uri[160];
        played = tryUri(files[f]);
        if (!played) {
            fmt(uri, sizeof(uri), "file://%s", files[f]);
            played = tryUri(uri);
        }
    }
    if (!played) {
        logLine("music probe: nothing played");
        notify("Music probe: the service played nothing; send ps4probe-music.log");
        if (g_terminate) logLine("Terminate: 0x%08X", (unsigned)g_terminate());
        return 0;
    }

    // The quick menu: 2 minutes of looks, every change logged.
    notify("Music probe: playing. Open the quick menu: is there Music? Try pause, next, previous");
    logLine("watching the status for 2 min: press the quick menu's buttons now");
    for (int i = 0; i < 240; i++) {
        sysSleepMs(500);
        step("the play status");
        if (readStatus(line, sizeof(line))) logLine("  %3d s: %s", i / 2, line);
        if (sysExists(DIR "/ps4probe.stop")) break;
    }
    if (g_stop) logLine("Stop: 0x%08X", (unsigned)g_stop());
    if (g_terminate) logLine("Terminate: 0x%08X", (unsigned)g_terminate());
    logLine("music probe: done");
    notify("Music probe done: send /data/ShellCompanion/ps4probe-music.log");
    return 0;
}

static void* musicThread(void* arg) {
    (void)arg;
    const int rc = musicRun();
    logLine("the service's thread: done (%d)", rc);
    g_done = 1;
    return NULL;
}

// The service's thread started, and watched: up to 6 min, a line every 2 s
// for a step that has gone on 2 s or more.
int probeMain(void) {
    logOpen("ps4probe-music.log");
    char fw[64];
    firmware(fw, sizeof(fw));
    logLine("music probe: firmware %s", fw);
    g_stepUs = sysNowUs();
    const int rc = threadStart(musicThread, NULL, "ScProbeMusic");
    logLine("the service's thread: 0x%08X", (unsigned)rc);
    if (rc != 0) return musicRun();  // no thread: on this one, unwatched
    const char* lastLogged = NULL;
    for (int t = 0; t < 180 && !g_done; t++) {
        sysSleepMs(2000);
        const char* now = g_step;
        const uint32_t secs = (uint32_t)((sysNowUs() - g_stepUs) / 1000000u);
        // A status read every 500 ms is a step that is never long.
        if (secs >= 2) {
            logLine("watch: in %s for %u s", now, (unsigned)secs);
            if (now != lastLogged && secs >= 10)
                notify("Music probe: waiting in %s (%u s)", now, (unsigned)secs);
            if (secs >= 10) lastLogged = now;
        }
    }
    if (g_done) {
        logLine("music probe: the watch ends");
        return 0;
    }
    // Still in a call: the payload's memory is not given back to the loader
    // under it, which a call that ended later would run into. It waits here,
    // logging a line a minute, until the PS4 restarts.
    logLine("music probe: the service's thread is still in %s; waiting for good", g_step);
    notify("Music probe: stuck in %s. Send the log, then restart the PS4", g_step);
    for (;;) {
        sysSleepMs(60000);
        logLine("watch: in %s for %u s", g_step, (unsigned)((sysNowUs() - g_stepUs) / 1000000u));
        if (g_done) return 0;
    }
}
