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
// PROCESS_PRX_LOAD. 0 when both run; else -1, what happened in what, for a
// log.
//
// The sandbox (1.54): the files these calls share with the shell are in
// /user/data, which an app cannot reach from its sandbox. Each call here
// that touches them jailbreaks the app (GoldHEN's JAILBREAK) on the way in
// when /user/data cannot be written, and puts the sandbox back on the way
// out (UNJAILBREAK, with the backup it kept), so the app is in its sandbox
// between calls. While a call runs (scOpen up to ~3 s, scMusicTakeBack's
// status reads) the whole app is out of it: another thread of the app
// opening /app0 or its own /data paths then may not find them. Before 1.54,
// scOpen jailbroke the app and left it so until it exited.
int scOpen(char* what, size_t size);

// 1: jailbroken now and kept so (as before 1.54), for an app that reaches
// /user/data itself; 0: back to each call's own (the sandbox restored once
// no call is running).
void scStayJailbroken(int on);

// The sandbox put back now if the app was left jailbroken (scStayJailbroken),
// and each call's own after. 0, or -1 if GoldHEN refused.
int scClose(void);

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

// ── Background downloads ─────────────────────────────────────────────────────
// Files fetched in the shell after the app has gone, as the Vita client's
// (docs/PROTOCOL.md §5): a partial file continued (HTTP Range), waited for
// and tried again, a notification at the start, as each item ends and at
// the end. ps4/README.md, "Background downloads".

typedef struct ScDownload {
    const char* id;        // the app's id for it
    const char* file;      // the file to write (/data/...), continued from its size;
                           // for "pages" a template with {page} or {page:N}
    uint64_t size;         // its full size, 0 if unknown
    const char* title;     // for the notifications
    const char* kind;      // "url", "pages" or "pkg" (below)
    const char* args[3];   // url: the URL, extra header lines; pages: the URL with
                           // {page}, "count" or "count,first", extra header lines
    const char* icon;      // optional: its picture in Notifications > Downloads, a PNG
                           // (a path on the console, /data/..., or a URL); NULL for
                           // the app's own icon (a package's own if it has one)
} ScDownload;

// The list, written whole (downloads.txt): up to 200 items, in order.
// notify 1 for the notifications; headers "Name: value" lines, \r\n
// between, sent with every request (a token, say). 0, or -1.
int scWriteDownloads(int notify, const char* headers, const ScDownload* items, uint32_t count);
// As the app goes: the shell begins the list written. 0, or -1.
int scDownloadsStart(void);
// As the app opens: the shell stops after the piece it is writing; this waits
// (up to 5 s) for running=0 in the status, after which the files are the
// app's. 0 when stopped, -1 if it did not say so in time.
int scDownloadsTakeBack(void);

typedef struct ScDownloadStatus {
    char id[64];
    char state[12];        // going, waiting, paused, done, gone, failed or cancelled
    uint64_t bytes;
    uint64_t size;         // 0 if not known
    uint32_t pagesDone;    // pages: those whole, of pageCount
    uint32_t pageCount;
} ScDownloadStatus;
// What the shell says now (downloads-status.txt): running 1 while it
// fetches; up to cap items into items, how many in *count. 0, or -1.
int scReadDownloadsStatus(int* running, ScDownloadStatus* items, uint32_t cap, uint32_t* count);

// ── Packages and self-update ─────────────────────────────────────────────────
// A download list's item of kind "pkg" (args[0] its URL or a path on the
// console, args[1] extra header lines; file may be NULL) goes to the PS4's
// own download service: it downloads and installs it as the Store's are,
// with its entry and progress bar in Notifications > Downloads. Its status
// says how far, and its task id with the service in the fifth field.

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
