/**
 * vita-shell-companion's client: what an app links to load the plugin into
 * SceShell and talk to it (docs/PROTOCOL.md). Plain C, for any app; every
 * call is safe from any thread once scInit has run, and none waits for long.
 *
 *   scInit("ABCD00001", "ux0:data/MyApp", "My App",
 *          SC_FEAT_ROW | SC_FEAT_MUSIC, "app0:module/shell_companion.suprx");
 *   char what[256];
 *   scOpen(what, sizeof(what));       // loads the plugin if it is not there
 *
 * Apache-2.0 (client/LICENSE), so that apps under any licence can link it;
 * the plugin itself is GPL-3.0, and apps only talk to it through pipes and
 * files.
 */

#ifndef SHELL_COMPANION_H
#define SHELL_COMPANION_H

#include <stddef.h>
#include <stdint.h>

#include "sc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Setting up ───────────────────────────────────────────────────────────────

// Once, before any other call: who the app is. titleId its title id; dataDir
// its data folder, where every file below goes (made if missing); name its
// name for the plugin's log and notification; features the SC_FEAT_* it
// wants; bundledPluginPath the plugin's .suprx in its own package, loaded
// when no plugin of this protocol or a later one is in SceShell yet. No I/O:
// the plugin is found by scOpen. 0, or -1 when an argument is missing.
int scInit(const char* titleId, const char* dataDir, const char* name, uint32_t features,
           const char* bundledPluginPath);

// Finds the plugin in SceShell, or loads the bundled one, opens its pipes and
// registers the app with it (SC_CMD_HELLO). 0 when open, else the code that
// stopped it; what happened, for a log, in what. Loading needs taiHEN and the
// app to be unsafe homebrew.
int scOpen(char* what, size_t size);
int scIsOpen(void);

// A file in the app's data folder, into out. 0, or -1 before scInit or when
// it does not fit.
int scPath(const char* name, char* out, size_t size);

// ── The Music row (SC_FEAT_ROW) ──────────────────────────────────────────────
// A press waiting on the row.
enum { SC_NONE = 0, SC_PLAY_PAUSE, SC_NEXT, SC_PREVIOUS };

// The row taken whenever the quick menu opens. Send every second while it is
// wanted, as a heartbeat: the plugin gives the row back by itself when this
// stops for SC_ROW_TIMEOUT_S. Never waits.
int scRowActivate(void);
// The row given back now.
int scRowRelease(void);
// The song on the row: the title above, "artist / album" below. UTF-8.
int scRowSong(const char* title, const char* artist, const char* album);
// Whether it plays (1, ⏸ shown) or is paused (0).
int scRowPlaying(int playing);
// The song's cover, written first to SC_ART_FMT under this serial
// (sc_cover.h's scCoverWrite); 0 for none.
int scRowArt(uint32_t serial);
// A press waiting for this app, or SC_NONE. Never waits.
int scPoll(void);

// ── Background music (SC_FEAT_MUSIC) ─────────────────────────────────────────
// As the app goes: the shell plays the queue written with scWriteQueue.
int scMusicPlay(void);
// As the app opens, its player ready: the shell stops and writes where it
// had got to (scReadQueueStatus). Sent only to a plugin that is in SceShell
// already, and never loads one: 0 when sent, else the code (the pipe's
// absence when no plugin is there, and so nothing carried on).
int scMusicTakeBack(void);

// ── Background downloads (SC_FEAT_DOWNLOADS) ─────────────────────────────────
// As the app goes: the shell carries on the list written with
// scWriteDownloads.
int scDownloadsStart(void);
// As the app opens: the shell stops after the piece it is writing, then
// writes running=0 to the status, which the app waits for before it touches
// the files. As scMusicTakeBack, never loads the plugin.
int scDownloadsTakeBack(void);
// The one progress notification, shown by the plugin while the app itself
// downloads, so that it goes on once the app has gone. Two lines, UTF-8, and
// how far, 0 to 1000, or SC_PROGRESS_UNKNOWN.
int scDownloadProgress(const char* line1, const char* line2, uint32_t permille);
// The notification ended, with these two lines.
int scDownloadProgressEnd(const char* line1, const char* line2);
// Whether "Pause downloads?" was answered Yes while the app downloads: the
// pause file, removed as it is seen. 1 when it was.
int scDownloadPauseAsked(void);

// ── Self-update (SC_FEAT_UPDATE) ─────────────────────────────────────────────
// The update unpacked to SC_UPDATE_DIR in the data folder, its
// sce_sys/package/head.bin made: installed once the app has quit, which it
// does at once after this. The app is launched again either way, and reads
// scReadUpdateResult.
int scUpdateInstall(void);

// ── The files ────────────────────────────────────────────────────────────────
// Each written whole beside its name and moved in, with its "end" line, its
// fields URL-encoded where docs/PROTOCOL.md says: 0, or -1 when it could not
// be written. Strings may be NULL for "".

typedef struct ScTrack {
    const char* id;        // the app's id for it: its files' names, {id} in the reports
    uint32_t durationMs;   // 0 if unknown
    const char* local;     // a file on the card to play as it is
    const char* url;       // where to fetch it in its own format
    const char* ext;       // that format's extension, NULL to take it from url
    const char* altUrl;    // a URL that answers with it as MP3, for formats the shell does not play
    const char* altPrep;   // fetched and passed over just before altUrl
    const char* title;
    const char* artist;
    const char* album;
    const char* coverUrl;  // a PNG of it for the row, ideally SC_ART_SIZE square
    const char* coverPng;  // the same, on the card already (scCoverWriteTo)
} ScTrack;

typedef struct ScReport {
    const char* url;       // a template (docs/PROTOCOL.md §4.3), NULL for none
    const char* method;    // GET when NULL; POST, PUT, PATCH, DELETE
    const char* body;      // a template too, NULL for none
    const char* type;      // the body's Content-Type, application/json when NULL
} ScReport;

typedef struct ScQueue {
    uint32_t index;        // the track to start with
    uint32_t positionMs;   // where in it
    int repeat;            // SC_REPEAT_*
    int offlineSkip;       // 1: a track that cannot be fetched skips to the next on the card
    const char* headers;   // "Name: value" lines, \r\n between, sent with every request
    ScReport state;        // where the music is
    uint32_t stateEveryS;  // how often it is repeated while it plays; 0 for 10
    ScReport played;       // a track played to its end
} ScQueue;

int scWriteQueue(const ScQueue* queue, const ScTrack* tracks, uint32_t count);

typedef struct ScDownload {
    const char* id;
    const char* file;      // the file to write, continued from its size
    uint64_t size;         // its full size, 0 if unknown
    const char* title;     // for the notification
    const char* kind;      // "url", "plexqueue" or "plexadd"
    const char* args[3];   // url: the URL, extra header lines; plexqueue: server, queue id,
                           // item id; plexadd: server, query, profile extra
} ScDownload;

int scWriteDownloads(int notify, const char* headers, const ScDownload* items, uint32_t count);

// What the files the plugin writes say. 0 when the file is there and whole.
typedef struct ScQueueStatus {
    char written[32];
    char queue[32];        // the written= of the queue it plays
    int index;
    char id[64];
    uint32_t positionMs;
    uint32_t durationMs;
    char state[16];        // SC_STATE_*
    int fetching;          // 1: waiting for that track, to play from positionMs
    int paused;            // 1: paused from the row
} ScQueueStatus;
int scReadQueueStatus(ScQueueStatus* out);

typedef struct ScDownloadStatus {
    char id[64];
    char state[12];        // going, waiting, done, gone or failed
    uint64_t bytes;
    uint64_t size;
    char queueId[16];      // plexqueue/plexadd: the queue's and item's ids it used
    char itemId[16];
} ScDownloadStatus;
// Up to cap items into items, how many in *count.
int scReadDownloadsStatus(int* running, int* paused, ScDownloadStatus* items, uint32_t cap,
                          uint32_t* count);

// result 0 when installed, else the installer's code; why says what failed.
int scReadUpdateResult(int* result, char* why, size_t whySize);

#ifdef __cplusplus
}
#endif

#endif
