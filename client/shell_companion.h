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
// registers the app with it (SC_CMD_HELLO), waiting up to half a second for
// its answer (scPluginInfo). 0 when open, else the code that stopped it;
// what happened, for a log, in what. Loading needs taiHEN and the app to be
// unsafe homebrew.
int scOpen(char* what, size_t size);

// The plugin that answered this app's HELLO: several apps ship a plugin,
// and the first one loaded in a boot is the one in SceShell until the Vita
// restarts, which may be older than the app's own. The features it has, the
// app's that it took, and its caps, things an app checks before it counts on
// them (scPluginHas: "downloads.battery", "sendspin.controller", ...). 0, or
// -1 when it never answered: a plugin older than version 0.2.0, which has
// the features of protocol 1 as it first was.
typedef struct ScPluginInfo {
    char written[32];
    unsigned protocol;
    char version[16];      // "0.2.0"
    uint32_t versionCode;  // major * 10000 + minor * 100 + patch
    uint32_t features;     // SC_FEAT_* it has
    uint32_t granted;      // of those this app asked for
    char caps[192];        // ',' between
} ScPluginInfo;
int scPluginInfo(ScPluginInfo* out);
int scPluginHas(const ScPluginInfo* info, const char* cap);
// The same, read from the file whether or not the answer has come.
int scReadPluginInfo(ScPluginInfo* out);
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
// The same for a VPK as it was downloaded (UTF-8 path on the card): the plugin
// unpacks it, checks that its TITLE_ID is this app's, makes its head.bin, and
// installs it once the app has quit, which it does at once after this. Sent
// only to a plugin whose answer to HELLO lists "update.vpk" (0.3.0 on): else
// -2, with nothing sent, as an older one would install an SC_UPDATE_DIR left
// from before. The VPK is left where it is.
int scUpdateInstallVpk(const char* vpkPath);

// ── Sendspin (SC_FEAT_SENDSPIN) ──────────────────────────────────────────────
// As the app goes, having closed its own connection to the server: the
// plugin becomes the Sendspin player scWriteSendspin described, with the
// same client id, so that Music Assistant sees the same player
// (docs/PROTOCOL.md §8).
typedef struct ScSendspin {
    const char* host;          // the server's IPv4 address: resolve a name first
    uint16_t port;             // 0 for 8927
    const char* path;          // NULL for /sendspin
    const char* clientId;      // the app's own, as its client/hello has it
    const char* name;          // the player's name
    const char* product;       // device_info; NULL for "PS Vita", "Sony", the plugin's
    const char* manufacturer;
    const char* software;
    int metadata;              // 1: metadata@v1, the song on the Music row
    int controller;            // 1: controller@v1, the row's buttons to the server
    uint32_t bufferBytes;      // buffer_capacity; 0 for 2 MB (about 12 s of PCM)
    int staticDelayMs;
    int volume;                // 0-100, as the app had it; the plugin goes on from there
    int muted;
} ScSendspin;
int scWriteSendspin(const ScSendspin* s);
int scSendspinStart(void);
// As the app opens: the plugin says goodbye and closes its connection, then
// writes state=stopped to the status, which the app waits for (a second or
// two at most) before it connects itself. As scMusicTakeBack, never loads
// the plugin.
int scSendspinTakeBack(void);

typedef struct ScSendspinStatus {
    char written[32];
    char state[16];        // waiting, connecting, connected, streaming, reconnecting,
                           // stopped or failed
    char server[64];
    int streaming, playing;
    int volume, muted;     // as the server last set them: the app goes on from there
    uint32_t positionMs, durationMs;  // the song's, with metadata
} ScSendspinStatus;
int scReadSendspinStatus(ScSendspinStatus* out);

// ── Debug ────────────────────────────────────────────────────────────────────
// SC_CMD_PROBE_STREAM (SC_FEAT_MUSIC): the plugin streams a growing WAV
// through the shell's music player and logs what the player makes of it
// (docs/PROTOCOL.md §10). mode SC_PROBE_*; sourceWav a 44.1 kHz 16-bit stereo
// WAV on the card to stream, NULL or "" for a tone; for SC_PROBE_FILE, the
// file to play as it is. SC_PROBE_HTTP sends the request scPath's
// "sc-probe-http.txt" describes.
int scProbeStream(int mode, const char* sourceWav);

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

// A line of the queue's head the app writes as it is, its value encoded:
// the remote control's control.* lines (docs/PROTOCOL.md §4.4).
typedef struct ScHeadLine {
    const char* key;
    const char* value;
} ScHeadLine;

typedef struct ScQueue {
    uint32_t index;        // the track to start with
    uint32_t positionMs;   // where in it
    int repeat;            // SC_REPEAT_*
    int offlineSkip;       // 1: a track that cannot be fetched skips to the next on the card
    // What the row's ⏮ ⏭ do while the shell plays the queue: 0 skip tracks;
    // 1 seek back and forward within the track by these (0 for 10 s and
    // 30 s), on to the next track past its end, as for an audiobook.
    int seekButtons;
    uint32_t seekBackMs, seekForwardMs;
    const char* headers;   // "Name: value" lines, \r\n between, sent with every request
    ScReport state;        // where the music is
    uint32_t stateEveryS;  // how often it is repeated while it plays; 0 for 10
    ScReport played;       // a track played to its end
    const ScHeadLine* extra;  // more head lines, such as control.*; NULL for none
    uint32_t extraCount;
} ScQueue;

int scWriteQueue(const ScQueue* queue, const ScTrack* tracks, uint32_t count);

typedef struct ScDownload {
    const char* id;
    const char* file;      // the file to write, continued from its size
    uint64_t size;         // its full size, 0 if unknown
    const char* title;     // for the notification
    const char* kind;      // "url", "pages", "plexqueue" or "plexadd"
    const char* args[3];   // url: the URL, extra header lines; pages: the URL with {page},
                           // "count" or "count,first", extra header lines; plexqueue:
                           // server, queue id, item id; plexadd: server, query, profile extra
    // For "pages" (a manga chapter, say), file is a template too: {page} is
    // the page number from first on (0 by default), {page:3} the same padded
    // to 3 digits; each page goes to its own file. size is 0 or the chapter's
    // whole size.
} ScDownload;

int scWriteDownloads(int notify, const char* headers, const ScDownload* items, uint32_t count);
// The battery= scWriteDownloads writes: the plugin's downloads wait below
// this percent while the Vita is off power, and go on by themselves once it
// is on power or 5 points above it; 0 never waits. SC_BATTERY_PAUSE_PERCENT
// unless changed.
extern int scDownloadsBatteryPercent;

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

// paused: 0, SC_PAUSED_ASKED (the notification's Yes: the app goes on with
// them), or SC_PAUSED_BATTERY (waiting for power: the plugin goes on).
typedef struct ScDownloadStatus {
    char id[64];
    char state[12];        // going, waiting, done, gone or failed
    uint64_t bytes;
    uint64_t size;
    char queueId[16];      // plexqueue/plexadd: the queue's and item's ids it used
    char itemId[16];
    uint32_t pagesDone;    // pages: those whose files are whole, from the first
    uint32_t pageCount;
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
