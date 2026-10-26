/**
 * vita-shell-companion, protocol 1: what an app says to the plugin in
 * SceShell, and what the plugin says back. docs/PROTOCOL.md describes it in
 * full; this is the part both sides compile against. Plain C.
 */

#ifndef SC_PROTOCOL_H
#define SC_PROTOCOL_H

#include <stdint.h>

// The protocol's version, in the pipes' names. A plugin of version N serves
// apps of every version up to N; an app looks for its own version's pipes,
// then higher ones (client/shell_companion.c).
#define SC_PROTOCOL 1
#define SC_PIPE_TO_PLUGIN_FMT   "ShellCompanion%u::ToPlugin"
#define SC_PIPE_FROM_PLUGIN_FMT "ShellCompanion%u::FromPlugin"
#define SC_PIPE_TO_PLUGIN       "ShellCompanion1::ToPlugin"
#define SC_PIPE_FROM_PLUGIN     "ShellCompanion1::FromPlugin"

// What an app asks for in SC_CMD_HELLO.
#define SC_FEAT_ROW       0x1u  // its song, cover and working buttons on the Music row
#define SC_FEAT_MUSIC     0x2u  // its queue played on by the shell once it has gone
#define SC_FEAT_DOWNLOADS 0x4u  // its downloads carried on, with one notification
#define SC_FEAT_UPDATE    0x8u  // its update installed once it has gone
#define SC_FEAT_SENDSPIN  0x10u // its Sendspin player (Music Assistant) carried on once it has gone

enum {
    // arg: SC_FEAT_* bits; text1: the app's data folder (all its files below
    // live there); text2: its name, for the log and the notification.
    SC_CMD_HELLO = 1,
    // The Music row (SC_FEAT_ROW).
    SC_CMD_ROW_ACTIVATE,  // take it while the quick menu is open; every second as a heartbeat
    SC_CMD_ROW_RELEASE,   // give it back now
    SC_CMD_ROW_SONG,      // text1 title, text2 artist, text3 album
    SC_CMD_ROW_PLAYING,   // arg: 1 playing, 0 paused
    SC_CMD_ROW_ART,       // arg: serial, 0 for none: SC_ART_FMT, written first
    // Background music (SC_FEAT_MUSIC).
    SC_CMD_MUSIC_PLAY,       // play SC_QUEUE from its index and place
    SC_CMD_MUSIC_TAKE_BACK,  // stop, and write the status with the place
    // Background downloads (SC_FEAT_DOWNLOADS).
    SC_CMD_DL_START,         // carry SC_DOWNLOADS on once the app has gone
    SC_CMD_DL_TAKE_BACK,     // stop after the piece being written
    SC_CMD_DL_PROGRESS,      // text1, text2: the notification's lines; arg: 0..1000 or SC_PROGRESS_UNKNOWN
    SC_CMD_DL_PROGRESS_END,  // text1, text2: its last two lines
    // Self-update (SC_FEAT_UPDATE).
    SC_CMD_UPDATE_INSTALL,   // install SC_UPDATE_DIR once the app has gone, then launch it;
                             // text1: a VPK to unpack into it first (caps "update.vpk")
    // Debug (SC_FEAT_MUSIC): arg SC_PROBE_*; text1 a WAV on the card to stream
    // (44.1 kHz, 16-bit, stereo PCM), empty for a tone, or SC_PROBE_FILE's
    // file. docs/PROTOCOL.md §10.
    SC_CMD_PROBE_STREAM,
    // Sendspin (SC_FEAT_SENDSPIN).
    SC_CMD_SENDSPIN_START,      // be the player SC_SENDSPIN describes once the app has gone
    SC_CMD_SENDSPIN_TAKE_BACK   // stop being it; SC_SENDSPIN_STATUS says stopped once it has
};
#define SC_PROGRESS_UNKNOWN 0xFFFFFFFFu

// SC_CMD_PROBE_STREAM's runs: does the shell play a WAV still being written?
#define SC_PROBE_STOP    0  // stop the one running
#define SC_PROBE_HUGE    1  // its header gives a huge size
#define SC_PROBE_GROWING 2  // its header gives the size written, rewritten as it grows
#define SC_PROBE_REOPEN  3  // as 2, opened again where it was at each stall and every 10 s
#define SC_PROBE_ALL     4  // 1, 2 and 3 in turn
#define SC_PROBE_FILE    5  // text1, a file on the card, played as it is (an .m4b, say)
#define SC_PROBE_HTTP    6  // the request in <folder>/sc-probe-http.txt sent once

// Files, in the app's data folder (SC_CMD_HELLO's text1). Each is written
// whole as "<name>.tmp", then renamed into place, and ends with an "end"
// line; one key=value a line, unknown keys skipped.
#define SC_ART_FMT          "sc-art-%u.png"       // the row's cover, %u = serial & 1
#define SC_QUEUE            "sc-queue.txt"        // the music handed over
#define SC_QUEUE_STATUS     "sc-queue-status.txt" // where the shell has got to
#define SC_SONGS            "sc-songs"            // tracks and covers the plugin fetched
#define SC_DOWNLOADS        "sc-downloads.txt"
#define SC_DOWNLOADS_STATUS "sc-downloads-status.txt"
#define SC_DOWNLOADS_PAUSE  "sc-downloads-pause"  // made when "Pause downloads?" is answered Yes
#define SC_UPDATE_DIR       "sc-update"
#define SC_UPDATE_RESULT    "sc-update-result.txt"
#define SC_PLUGIN_INFO      "sc-plugin.txt"       // the plugin's version and features, at each HELLO
#define SC_SENDSPIN         "sc-sendspin.txt"     // the Sendspin player handed over
#define SC_SENDSPIN_STATUS  "sc-sendspin-status.txt"

#define SC_ART_SIZE         64
#define SC_ROW_TIMEOUT_S    5
#define SC_MAX_APPS         16
#define SC_MAX_TRACKS       1000
#define SC_MAX_DOWNLOADS    200
#define SC_MAX_PAGES        1000  // a "pages" download's page count
// The downloads' battery= by default: they wait below this percent while the
// Vita is not on power.
#define SC_BATTERY_PAUSE_PERCENT 15
// The downloads status's paused=: by the notification's Yes (the app goes on
// with them), or waiting for power (the plugin goes on by itself).
#define SC_PAUSED_ASKED   1
#define SC_PAUSED_BATTERY 2

// The queue's repeat=.
#define SC_REPEAT_OFF 0
#define SC_REPEAT_ONE 1
#define SC_REPEAT_ALL 2

// A track= line's fields, '|'-separated, each URL-encoded.
enum {
    SC_TRACK_ID,
    SC_TRACK_DURATION_MS,
    SC_TRACK_LOCAL,
    SC_TRACK_URL,
    SC_TRACK_EXT,
    SC_TRACK_ALT_URL,
    SC_TRACK_ALT_PREP,
    SC_TRACK_TITLE,
    SC_TRACK_ARTIST,
    SC_TRACK_ALBUM,
    SC_TRACK_COVER_URL,
    SC_TRACK_COVER_PNG,
    SC_TRACK_FIELDS
};

// The status's state=.
#define SC_STATE_PLAYING "playing"
#define SC_STATE_OVER    "over"
#define SC_STATE_STOPPED "stopped"

// To the plugin: a whole ScMessage each time.
typedef struct ScMessage {
    uint32_t cmd;
    uint32_t arg;
    char titleId[12];   // the sending app, NUL-terminated
    uint16_t text1[256];  // UTF-16, NUL-terminated
    uint16_t text2[256];
    uint16_t text3[256];
} ScMessage;

// From the plugin: a press on the row, for the app that owns it; or, with
// SC_REPLY_HELLO, the answer to an app's SC_CMD_HELLO, its SC_PLUGIN_INFO
// written first. A client that knows no SC_REPLY_HELLO passes it over.
enum { SC_PRESS_PLAY_PAUSE = 1, SC_PRESS_NEXT, SC_PRESS_PREVIOUS };
#define SC_REPLY_HELLO 0x100u
typedef struct ScPress {
    uint32_t press;
    char titleId[12];
} ScPress;

// How many messages each pipe holds. A press is sent without waiting and
// dropped when the pipe is full, so an app that stopped reading never holds
// the quick menu up.
#define SC_TO_PLUGIN_SLOTS   4
#define SC_FROM_PLUGIN_SLOTS 16

#endif
