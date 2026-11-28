/**
 * Shell Companion's PS4 client: what an app (an OpenOrbis homebrew, with
 * GoldHEN) links to hand its music to the shell. The player and the music
 * watch (one module, sc_companion.prx) are loaded into SceShellUI; the player plays
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
 * The calls may be made from more than one thread (one runs at a time).
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

// The player and the music watch in SceShellUI: Shell Companion's module,
// sc_companion.prx, loaded unless it is there already (their heartbeats less
// than 3 s old; a module loads once a boot, and a second copy would play and
// hook the shell twice). It is taken from the app's package (at its root),
// else from /data/GoldHEN/plugins/, copied there and loaded with GoldHEN's
// PROCESS_PRX_LOAD. Jailbreaks the app first (GoldHEN) if /data cannot be
// written, and does not undo it: the app stays out of its sandbox until it
// exits (the later calls need /data). 0 when both run; else -1, what
// happened in what, for a log.
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
    const char* id;        // the app's own id for it: {id} in ScPlus's templates, and in
                           // scReadPlusEvents; NULL for none
} ScTrack;

// What the Music section's "+" (R3, "Add to Your Music") does for this app's
// tracks. Every press is recorded either way (scReadPlusEvents), and the
// track added to or taken from Liked (liked.txt), which lights the "+".
enum { SC_PLUS_LIKED = 0, SC_PLUS_HTTP = 1 };
typedef struct ScPlus {
    int mode;              // SC_PLUS_LIKED, or SC_PLUS_HTTP: the app's server told at once
    // SC_PLUS_HTTP: URL templates, with {id}, {title}, {artist}, {album} and
    // {file} (the track's path or URL), each URL-encoded where it goes.
    const char* saveUrl;   // "+" lit: the track saved
    const char* deleteUrl; // "+" put out: the track taken away
    const char* method;    // for both: NULL for GET; POST, PUT, DELETE, PATCH
    const char* body;      // a template too (its placeholders escaped as in a JSON string), NULL for none
    const char* type;      // the body's Content-Type, application/json when NULL
    const char* headers;   // "Name: value" lines, \r\n between (a token, say)
    // Asked (GET) as each track begins, if given: a 2xx answer (whose body
    // holds containsMatch, if given) lights the "+", any other puts it out.
    const char* containsUrl;
    const char* containsMatch;
} ScPlus;

typedef struct ScQueue {
    uint32_t index;        // the track to begin with, from 0
    uint32_t positionMs;   // where in it
    const ScPlus* plus;    // NULL: SC_PLUS_LIKED
} ScQueue;

// The queue, written whole (queue.txt): MP3 and 16-bit WAV, up to 10000
// tracks (the Music section groups them in up to 1000 albums; a local file's
// tags are read once and cached). A field may not hold a tab, a line break or
// a | (they are made spaces). Every track needs local or url: -1, nothing
// written, if one has neither. 0, or -1 if it could not be written.
int scWriteQueue(const ScQueue* queue, const ScTrack* tracks, uint32_t count);

// ── Handing over and taking back ─────────────────────────────────────────────

// The player loads the queue written and plays it: as the app goes (or now,
// for an app that has no player of its own). 0, or -1.
int scMusicPlay(void);

// As the app opens, its own player ready: the player stops and keeps where it
// was, for scReadQueueStatus (state "stopped", positionMs). Sent even when the
// player's heartbeat is late (it may be waiting on a server); waits up to 5 s
// for a status written since that says "stopped". 0 then; -1 if it did not
// say so in time (it will stop once it reads it), or there is no player.
int scMusicTakeBack(void);

// A command to the player, as the quick menu sends them: "play", "pause",
// "toggle", "next", "prev", "stop", "volume=<0-32768>", "repeat=<0 none, 1
// all, 2 one>", "shuffle=<0|1>", "index=<n>". Each call's commands are
// written whole into a file of their own (cmd, else cmd.<ms>-<pid>-<n>),
// which the player reads in order. 0, or -1.
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

// ── Self-update ──────────────────────────────────────────────────────────────
// The app's own update, a package (a /data/... path or a URL), installed by
// the shell once the app has gone, which it does at once after this (the
// shell waits 3 s). relaunch 1: the app started again once installed. 0, or
// -1 when the player is not there to do it.
int scUpdateInstallPkg(const char* pkg, int relaunch);
// What came of it (update-result.txt): result 0 when installed, else the
// code; why says what happened. 0, or -1 when there is none yet.
int scReadUpdateResult(int* result, char* why, size_t whySize);

// "+" pressed while the app was away, oldest first, each taken out of the
// file as it is read: up to cap into events, how many in *count; those past
// cap are kept for the next call. 0, or -1.
typedef struct ScPlusEvent {
    uint64_t ms;           // when (the monotonic clock, ms)
    int saved;             // 1 "+" lit, 0 put out
    char id[128];          // the track's ScTrack.id, "" if it had none
    char file[768];        // its path or URL
} ScPlusEvent;
int scReadPlusEvents(ScPlusEvent* events, uint32_t cap, uint32_t* count);

// Liked tracks (R3 "+" in the Music section): /data/ShellCompanion/ps4/liked.txt,
// a path or URL a line, as the queue gave them.
#define SC_LIKED_FILE "/data/ShellCompanion/ps4/liked.txt"

#ifdef __cplusplus
}
#endif

#endif
