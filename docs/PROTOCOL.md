# vita-shell-companion — protocol 1 (draft for review)

`shell_companion.suprx` runs inside SceShell, which no app or game closes.
Any app loads it once per boot and uses only the features it wants:

| Feature | Bit | What the app gets |
|---|---|---|
| Music row | `SC_FEAT_ROW` | its song, cover and working ⏮ ⏯ ⏭ in the quick menu's Music row |
| Background music | `SC_FEAT_MUSIC` | its queue keeps playing through the shell's music player after it closes |
| Background downloads | `SC_FEAT_DOWNLOADS` | its downloads continue after it closes, with one progress notification |
| Self-update | `SC_FEAT_UPDATE` | its update is installed after it closes, and it is launched again |

Nothing in the plugin knows about Plex, Audiobookshelf, Music Assistant or
Suwayomi. Everything server-specific is in plain URLs, request headers and
report templates that the app writes.

## 1. Transport

- **Pipes:** `"ShellCompanion1::ToPlugin"` and `"ShellCompanion1::FromPlugin"`.
  The `1` is the protocol version.
  - **Compatibility:** a plugin of protocol N also serves apps of every lower
    version. An app's client library looks for its own version's pipes first,
    then any higher version's. Only if it finds none does it load the copy it
    ships in its own VPK.
- **One app at a time:** only one app or game runs at a time on the Vita, so
  the shared pipe pair carries only that app's messages. The plugin may still
  be working for an app that has closed, playing its music or finishing its
  downloads.
- **Files:** larger data goes through files in the app's own data folder,
  which the app registers with `SC_CMD_HELLO`.
  - **Writing:** write each file in full to `<name>.tmp`, then rename it into
    place.
  - **Completeness:** each file ends with an `end` line, and a file without one
    is ignored.
  - **Format:** one `key=value` per line. Unknown keys are skipped, so newer
    writers stay readable.

```c
typedef struct ScMessage {
    uint32_t cmd;
    uint32_t arg;
    char     titleId[12];   // the sending app, e.g. "VPLEX0001", NUL-terminated
    uint16_t text1[256];    // UTF-16, NUL-terminated (meaning depends on cmd)
    uint16_t text2[256];
    uint16_t text3[256];
} ScMessage;               // 1548 bytes

typedef struct ScPress {   // plugin -> app: a press on the Music row
    uint32_t press;        // SC_PRESS_PLAY_PAUSE, SC_PRESS_NEXT, SC_PRESS_PREVIOUS
    char     titleId[12];  // the row's owner the press was for
} ScPress;
```

## 2. Registering: `SC_CMD_HELLO`

Send this first, every time the app starts.

- `arg`: the `SC_FEAT_*` bits the app wants.
- `text1`: its data folder, for example `ux0:data/VitaPlex`. All files below
  live there.
- `text2`: its display name, used in the plugin's log.

The plugin keeps a record for up to 16 apps (title ID, folder, features). A
command for a feature the app didn't ask for is ignored and logged.

## 3. Music row

| Command | Fields |
|---|---|
| `SC_CMD_ROW_ACTIVATE` | take the row while the quick menu is open; send every second as a heartbeat. With none for 5 s, the row goes back to the system. |
| `SC_CMD_ROW_RELEASE` | give the row back now |
| `SC_CMD_ROW_SONG` | `text1` title, `text2` artist, `text3` album |
| `SC_CMD_ROW_PLAYING` | `arg` 1 playing (⏸ shown), 0 paused |
| `SC_CMD_ROW_ART` | `arg` serial, 0 for none: a 64×64 PNG at `<folder>/sc-art-<serial & 1>.png` |

- **Ownership:** the latest app to send `ROW_ACTIVATE` owns the row. A press
  goes to the owner as an `ScPress` on the FromPlugin pipe.
- **When the shell is playing an app's queue:** the presses act on that queue
  directly, and nothing is sent.

## 4. Background music

### Handing over (`SC_CMD_MUSIC_PLAY`)

When the app closes, it writes `<folder>/sc-queue.txt`, then sends
`SC_CMD_MUSIC_PLAY`:

```
written=<sceKernelGetSystemTimeWide()/1e6, %.6f>
index=<track to start with, from 0>
positionMs=<where in it>
repeat=<0 off, 1 one, 2 all>
offlineSkip=<0/1: when a track cannot be fetched, skip to the next one on the card>
headers=<header lines sent with every request below, "Name: value" separated by \r\n, URL-encoded>
report.state=<optional: URL template, see §4.3>
report.state.method=<GET (default), POST, PUT or PATCH>
report.state.body=<optional body template>
report.state.every=<seconds between repeats while playing, default 10>
report.played=<optional: URL template when a track plays to its end>
report.played.method=..., report.played.body=...
track=<fields, '|'-separated, URL-encoded, below>
...                                         (up to 1000 tracks)
end
```

A `track=` line has these fields:

| # | Field | Meaning |
|---|---|---|
| 0 | `id` | the app's own ID for the track (used in file names and reports) |
| 1 | `durationMs` | its length |
| 2 | `local` | a file on the card to play as-is, or empty |
| 3 | `url` | where to fetch it in its own format, or empty |
| 4 | `ext` | its extension (`mp3`, `m4a`, `aac`, `ogg`, `wav`, `at9`), or empty to take it from the URL |
| 5 | `altUrl` | used when `ext` is a format the shell doesn't play, such as FLAC: a URL that returns MP3, or empty to skip such tracks |
| 6 | `altPrep` | an optional URL fetched (and discarded) just before `altUrl`, for example Plex's transcode decision |
| 7 | `title` | for the Music row |
| 8 | `artist` | for the Music row |
| 9 | `album` | for the Music row |
| 10 | `coverUrl` | the cover for the row, fetched and scaled to 64×64, or empty |
| 11 | `coverPng` | a 64×64 PNG on the card already, or empty |

`{session}` in `altUrl` and `altPrep` is replaced by a number unique to the
fetch.

### Playback

- **Order:** the plugin plays the queue in order, starting from `index` at
  `positionMs`.
- **Sources:** for each track it uses `local` if present, otherwise a fetched
  copy at `<folder>/sc-songs/<id>.<ext>` (from `url`, or from `altUrl` as
  `.mp3`).
- **Fetching ahead:** it fetches the next track while the current one plays.
- **On the card:** it keeps the previous, current and next tracks' files.
- **Missing tracks:** a track it can't get is skipped. With `offlineSkip=1` it
  skips to the next track on the card; with none, it holds paused.

### 4.3 Report templates

Optional. They're how the app's server learns the playback position while
the app is closed. Placeholders are filled in when the report is sent:

| Placeholder | Value |
|---|---|
| `{id}` | the track's `id` |
| `{state}` | `playing`, `paused`, `buffering` or `stopped` |
| `{positionMs}` / `{positionS}` | position in ms / seconds (with 3 decimals) |
| `{durationMs}` / `{durationS}` | duration in ms / seconds |
| `{index}` | the track's place in the queue |

- **When:** at every state change, and every `report.state.every` seconds while
  playing.
- **Order:** reports are sent one at a time, in order.
- **Failures:** a server that doesn't answer is logged once and doesn't hold
  up playback.

Examples:

- **Plex:**
  `report.state=<server>/:/timeline?ratingKey={id}&key=/library/metadata/{id}&state={state}&time={positionMs}&duration={durationMs}`
  and `report.played=<server>/:/scrobble?key={id}&identifier=com.plexapp.plugins.library`.
- **Audiobookshelf:** `report.state=<server>/api/me/progress/{id}`, with
  `report.state.method=PATCH` and
  `report.state.body={"currentTime":{positionS},"duration":{durationS}}`.

### Status and taking back

- **Status:** `<folder>/sc-queue-status.txt` is written every 2 s and at every
  change:

  ```
  written=, queue=<the queue's written>, index=, id=, positionMs=, durationMs=,
  state=playing|over|stopped, fetching=0|1, paused=0|1, end
  ```

- **`SC_CMD_MUSIC_TAKE_BACK`:** the app sends it once its own player is ready.
  The plugin stops, writes the status one last time with the exact position,
  and the app continues from there.

## 5. Background downloads

### Handing over (`SC_CMD_DL_START`)

When the app closes, it writes `<folder>/sc-downloads.txt`, then sends
`SC_CMD_DL_START`:

```
written=...
notify=<1: show the progress notification>
headers=<sent with every request, as in §4>
item=<fields, tab-separated, below>     (up to 2000 items, in order)
end
```

Each item has these tab-separated fields:

| # | Field | Meaning |
|---|---|---|
| 0 | `id` | the app's ID for the download |
| 1 | `file` | the file to write; a partial file is continued with HTTP Range from its size |
| 2 | `size` | the expected full size, or 0 if unknown |
| 3 | `title` | for the notification |
| 4 | `kind` | `url` (§5.1), or `plexqueue` / `plexadd` (§5.2) |
| 5… | kind's own fields | see below |

#### 5.1 `url`

Field 5 is the URL. Field 6 is optional extra header lines for this item.

- **Resuming:** a server that answers a Range request with the whole file has
  it rewritten from the start.
- **Retries:** after 5, 15 and 60 s; getting any bytes resets the count.

#### 5.2 `plexqueue` / `plexadd`

These keep VitaPlex's existing behaviour: Plex's server-side download queue,
which prepares a transcoded file before it can be fetched.

- **`plexqueue`:** server, queue ID, item ID.
- **`plexadd`:** server, query, profile extra.

### Status and taking back

- **Status:** `<folder>/sc-downloads-status.txt` is written every 2 s:

  ```
  written=, running=0|1, paused=0|1,
  item=<id>\t<going|waiting|done|gone|failed>\t<bytes>\t<size>\t<kind's state>, end
  ```

- **`SC_CMD_DL_TAKE_BACK`:** the app sends it when it opens. The plugin stops
  after the piece it is writing and writes `running=0`. The app waits for
  that before touching the files.
- **Several apps:** downloads from different apps run one app's list at a
  time, in the order they were handed over.

### 5.3 The notification

| Command | Fields |
|---|---|
| `SC_CMD_DL_PROGRESS` | while the app itself downloads: `text1` first line, `text2` second line, `arg` progress 0–1000 or `SC_PROGRESS_UNKNOWN` |
| `SC_CMD_DL_PROGRESS_END` | the notification's last two lines |

- **One notification:** the plugin owns it, so the same notification carries
  on after the app closes.
- **Icon:** it shows the app's own icon.
- **"Pause downloads?":** answering Yes pauses the plugin's own downloads
  (`paused=1`). While the app is running, the plugin creates
  `<folder>/sc-downloads-pause` instead, which the app checks for.

## 6. Self-update: `SC_CMD_UPDATE_INSTALL`

1. The app unpacks its update to `<folder>/sc-update/` and forges
   `sce_sys/package/head.bin`. The client library includes `makeHeadBin`.
2. It sends `SC_CMD_UPDATE_INSTALL`, then quits.
3. The plugin waits up to 30 s for the app to exit, installs the update
   (retrying while the title still counts as running), and writes
   `<folder>/sc-update-result.txt`: `result=0x…`, `why=…`, `end`.
4. It relaunches the app either way.

## 7. Client library (`client/`)

Plain C, for any app:

```c
int  scInit(const char* titleId, const char* dataDir, const char* name, uint32_t features,
            const char* bundledPluginPath);   // finds or loads the plugin, sends HELLO
int  scIsOpen(void);
// row
int  scRowActivate(void);  int scRowRelease(void);
int  scRowSong(const char* title, const char* artist, const char* album);
int  scRowPlaying(int playing);  int scRowArt(uint32_t serial);  int scPoll(void);
// music
int  scMusicPlay(void);  int scMusicTakeBack(void);
// downloads
int  scDownloadsStart(void);  int scDownloadsTakeBack(void);
int  scDownloadProgress(const char* l1, const char* l2, uint32_t permille);
int  scDownloadProgressEnd(const char* l1, const char* l2);
// update
int  scUpdateInstall(void);
```

Plus small helpers to write the queue and download-list files correctly
(temporary name, `end` line, URL-encoding), so no app has to get the format
right by hand.

## 8. Logs

`ux0:data/ShellCompanion/plugin.log` holds the current boot, and
`plugin.prev.log` the boot before. Every line about an app's work names the
app.
