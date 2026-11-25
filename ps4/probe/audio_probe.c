/**
 * PS4 probe 1: can a payload's audio be heard at all, and on which port?
 * The first run (GoldHEN 2.4b18.6, firmware 11.50) opened the MAIN and BGM
 * ports and had its output taken at the hardware's pace, and nothing was
 * heard on either, from the home screen: the ports' volume read -1, never
 * set, and the BGM port's output 0.
 *
 * So each port is now given full volume (0 dB) as it opens, and six port
 * types are tried in turn, 20 s each, all with the same 440 Hz tone counted
 * out in beeps: as many beeps as the port's place, then a pause, so that
 * which is heard is a count, not a pitch.
 *
 *   1 beep: MAIN (0)       2: BGM (1)       3: VOICE (2)
 *   4: PERSONAL (3)        5: AUX (127)     6: type 14
 *
 * Stay on the home screen and count. Its log is
 * /data/ShellCompanion/ps4probe-audio.log; /data/ShellCompanion/ps4probe.stop
 * ends it early. No user service: the ports are opened for the system user
 * (255), as GoldHEN 2.4b18.6 hangs a payload's user service calls.
 */

#include "sys.h"

typedef int32_t (*FnInit)(void);
typedef int32_t (*FnOpen)(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                          uint32_t param);
typedef int32_t (*FnOutput)(int32_t handle, const void* p);
typedef int32_t (*FnClose)(int32_t handle);
typedef int32_t (*FnPortState)(int32_t handle, void* state);
typedef int32_t (*FnSetVolume)(int32_t handle, int32_t flag, int32_t* vol);

typedef struct {
    uint16_t output;
    uint8_t channel;
    uint8_t unk3;
    int16_t volume;
    uint16_t rerouteCounter;
    uint64_t flag;
    uint64_t unk7[2];
} PortState;  // OpenOrbis's OrbisAudioOutPostState

#define GRAIN 256    // samples a channel per output: 5.33 ms at 48 kHz
#define RATE 48000
#define USER_SYSTEM 255

static FnOpen g_open;
static FnOutput g_output;
static FnClose g_close;
static FnPortState g_state;
static FnSetVolume g_volume;

static int16_t g_buf[GRAIN * 2];
static uint32_t g_samples;

// A grain of the tone: a triangle wave, counted out in `beeps` beeps of
// 150 ms, 150 ms apart, in each 3 s.
static int g_beeps;
static void fill(uint32_t hz) {
    const uint32_t step = hz * 65536u / RATE;
    static uint16_t phase;
    for (int i = 0; i < GRAIN; i++) {
        int16_t v = 0;
        const uint32_t at = g_samples % (RATE * 3);   // in the 3 s
        const uint32_t beep = at / (RATE * 3 / 10);    // 300 ms slots
        if ((int)beep < g_beeps && at % (RATE * 3 / 10) < RATE * 15 / 100) {
            phase = (uint16_t)(phase + step);
            const int tri = phase < 32768 ? phase : 65535 - phase;
            v = (int16_t)((tri - 16384) / 3);
        }
        g_buf[2 * i] = g_buf[2 * i + 1] = v;
        g_samples++;
    }
}

static int stopAsked(void) {
    return sysExists("/data/ShellCompanion/ps4probe.stop");
}

// One phase: the port opened, the tone for `secs`, a line a second.
static int phase(const char* name, int type, uint32_t hz, int secs) {
    const int h = g_open(USER_SYSTEM, type, 0, GRAIN, RATE, 1 /* S16 stereo */);
    logLine("phase %s: port type %d opened: 0x%08X", name, type, (unsigned)h);
    if (h < 0) {
        notify("Probe: %s would not open (0x%08X)", name, (unsigned)h);
        for (int s = 0; s < secs && !stopAsked(); s++) sysSleepMs(1000);
        return 0;
    }
    // Full volume, on every channel: 0 dB is 32768.
    int32_t vol[8];
    for (int i = 0; i < 8; i++) vol[i] = 32768;
    const int32_t rcVol = g_volume ? g_volume(h, 0xFF, vol) : -1;
    logLine("phase %s: volume to 0 dB: 0x%08X", name, (unsigned)rcVol);
    notify("Probe: %s. Count %d beeps", name, g_beeps);
    g_samples = 0;
    for (int s = 0; s < secs; s++) {
        const uint64_t began = sysNowUs();
        int outputs = 0, errors = 0;
        int32_t lastErr = 0;
        // About a second of grains: each output waits for room, so this
        // keeps to the hardware's clock. Fewer in the second than 187 means
        // outputs took longer than real time; much quicker, that they were
        // not waited for (taken without being played?).
        for (int g = 0; g < RATE / GRAIN; g++) {
            fill(hz);
            const int32_t rc = g_output(h, g_buf);
            outputs++;
            if (rc < 0) {
                errors++;
                lastErr = rc;
            }
        }
        PortState st;
        memset(&st, 0, sizeof(st));
        const int32_t rcState = g_state ? g_state(h, &st) : -1;
        logLine("phase %s, %2d s: %d outputs in %u ms, %d failed (last 0x%08X); port: output %u, "
                "channel %u, volume %d, reroutes %u, flag 0x%lX (0x%08X)",
                name, s + 1, outputs, (unsigned)((sysNowUs() - began) / 1000u), errors,
                (unsigned)lastErr, st.output, st.channel, st.volume, st.rerouteCounter,
                (uint64_t)st.flag, (unsigned)rcState);
        if (stopAsked()) {
            g_close(h);
            return 1;
        }
    }
    const int32_t rcClose = g_close(h);
    logLine("phase %s: port closed: 0x%08X", name, (unsigned)rcClose);
    return 0;
}

int probeMain(void) {
    logOpen("ps4probe-audio.log");
    char fw[64];
    firmware(fw, sizeof(fw));
    logLine("audio probe: firmware %s", fw);
    const int lib = libLoad("libSceAudioOut.sprx");
    FnInit init = (FnInit)libSym(lib, "sceAudioOutInit");
    g_open = (FnOpen)libSym(lib, "sceAudioOutOpen");
    g_output = (FnOutput)libSym(lib, "sceAudioOutOutput");
    g_close = (FnClose)libSym(lib, "sceAudioOutClose");
    g_state = (FnPortState)libSym(lib, "sceAudioOutGetPortState");
    g_volume = (FnSetVolume)libSym(lib, "sceAudioOutSetVolume");
    logLine("libSceAudioOut: handle 0x%X; init %p, open %p, output %p, close %p, state %p", lib,
            (void*)init, (void*)g_open, (void*)g_output, (void*)g_close, (void*)g_state);
    if (!init || !g_open || !g_output || !g_close) {
        notify("Audio probe: no audio output here; see its log");
        return 1;
    }
    logLine("sceAudioOutInit: 0x%08X (0x8026000E is already initialised)", (unsigned)init());
    logLine("sceAudioOutSetVolume: %p", (void*)g_volume);
    notify("Audio probe: stay here and count the beeps. 6 ports, 20 s each");
    // In code, not a table of names: a raw payload has no relocations, so
    // a pointer kept in data would point where it was linked, not loaded.
    for (int i = 0; i < 6; i++) {
        const char* name = i == 0 ? "1: MAIN" : i == 1 ? "2: BGM" : i == 2 ? "3: VOICE"
                         : i == 3 ? "4: PERSONAL" : i == 4 ? "5: AUX" : "6: type 14";
        const int type = i < 4 ? i : i == 4 ? 127 : 14;
        g_beeps = i + 1;
        if (phase(name, type, 440, 20)) break;
    }
    logLine("audio probe: done");
    notify("Audio probe done: send /data/ShellCompanion/ps4probe-audio.log");
    return 0;
}
