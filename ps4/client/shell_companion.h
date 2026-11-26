/**
 * Shell Companion's PS4 client: what an app (an OpenOrbis homebrew, with
 * GoldHEN) links to hand its music to the shell. The player (sc_bgm.prx) and
 * the music watch (sc_mcw.prx) are loaded into SceShellUI; the player plays
 * the app's queue, local files or streams by URL, after the app has closed,
 * and the quick menu's Music section shows it, as the Vita client
 * (vita/client) does for the Vita's shell. ps4/README.md, "Client library".
 *
 *   scInit("ABCD00001", "My App");
 *   char what[256];
 *   if (scOpen(what, sizeof(what)) != 0) log(what);   // loads what is not there
 *   ScTrack t[2] = {{.url = "https://.../a.mp3", .title = "A", .artist = "Me",
 *                    .coverUrl = "https://.../a.jpg"},
 *                   {.local = "/data/MyApp/b.mp3"}};
 *   ScQueue q = {.index = 0, .positionMs = 12000};
 *   scWriteQueue(&q, t, 2);
 *   scMusicPlay();                                   // as the app goes
 *   ...
 *   scMusicTakeBack();                               // as it comes back
 *   ScQueueStatus st;
 *   scReadQueueStatus(&st);                          // where it had got to
 *
 * Plain C, libc only. Apache-2.0 (client/LICENSE), as the Vita client, so
 * that apps under any licence can link it; the modules are the project's own.
 */

#ifndef SC_PS4_CLIENT_H
#define SC_PS4_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Setting up ───────────────────────────────────────────────────────────────

// Once, before any other call: the app's title id (its package's, for where
// the modules are, below) and its name, for the logs. 0, or -1 when one is
// missing.
int scInit(const char* titleId, const char* name);

// The player and the music watch in SceShellUI: each loaded unless it is
// there already (its heartbeat less than 3 s old; a module loads once a
// boot, and a second copy would hook the shell twice). The modules are taken
// from the app's package (sc_bgm.prx and sc_mcw.prx at its root), else from
// /data/GoldHEN/plugins/, copied there and loaded with GoldHEN's
// PROCESS_PRX_LOAD. Jailbreaks the app first (GoldHEN) if /data cannot be
// written. 0 when both run; else -1, what happened in what, for a log.
int scOpen(char* what, size_t size);

// 1 if the player answers (its heartbeat), else 0.
int scPlayerRunning(void);
// 1 if the music watch answers, else 0.
int scWatchRunning(void);

// ── The queue ────────────────────────────────────────────────────────────────

typedef struct ScTrack {
    const char* local;     // a file the shell can read (/data/...), or NULL
    const char* url;       // else where to stream it from (http:// or https://)
    const char* title;     // NULL: the file's tags (local), else its name
    const char* artist;
    const char* album;     // tracks of one album are grouped in the Music section
    const char* coverUrl;  // its cover by URL (JPEG or PNG), fetched by the shell; NULL for none
} ScTrack;

typedef struct ScQueue {
    uint32_t index;        // the track to begin with, from 0
    uint32_t positionMs;   // where in it
} ScQueue;

// The queue, written whole (queue.txt): MP3 and 16-bit WAV, up to 512
// tracks. A field may not hold a tab or a line break (they are made
// spaces). 0, or -1 if it could not be written.
int scWriteQueue(const ScQueue* queue, const ScTrack* tracks, uint32_t count);

// ── Handing over and taking back ─────────────────────────────────────────────

// The player loads the queue written and plays it: as the app goes (or now,
// for an app that has no player of its own). 0, or -1.
int scMusicPlay(void);

// As the app opens, its own player ready: the player stops and keeps where it
// was, for scReadQueueStatus (state "stopped", positionMs). 0, or -1.
int scMusicTakeBack(void);

// A command to the player, as the quick menu sends them: "play", "pause",
// "toggle", "next", "prev", "stop", "volume=<0-32768>", "repeat=<0 none, 1
// all, 2 one>", "shuffle=<0|1>", "index=<n>". 0, or -1.
int scCommand(const char* command);

typedef struct ScQueueStatus {
    char state[16];        // playing, paused or stopped
    int index;             // the track's place in the queue
    int count;             // the queue's length
    int track;             // its number in the queue written (the Music section can reorder)
    uint32_t positionMs;   // where in it
    uint32_t durationMs;   // its length, 0 if not known
    char file[768];        // its path or URL
    char title[160], artist[160], album[160];
    int volume;            // 0-32768
    int repeat, shuffle;
    uint64_t beatMs;       // when the player last wrote it (the monotonic clock, ms)
} ScQueueStatus;

// What the player says now (status.txt). 0, or -1 if there is none.
int scReadQueueStatus(ScQueueStatus* out);

// Liked tracks (R3 "+" in the Music section): /data/ShellCompanion/ps4/liked.txt,
// a path or URL a line, as the queue gave them.
#define SC_LIKED_FILE "/data/ShellCompanion/ps4/liked.txt"

#ifdef __cplusplus
}
#endif

#endif
