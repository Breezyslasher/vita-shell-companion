# vita-shell-companion — protocol 1

`shell_companion.suprx` runs inside SceShell, which no app or game closes.
Any app loads it once per boot and uses only the features it wants:

| Feature | Bit | What the app gets |
|---|---|---|
| Music row | `SC_FEAT_ROW` | its song, cover and working ⏮ ⏯ ⏭ in the quick menu's Music row |
| Background music | `SC_FEAT_MUSIC` | its queue keeps playing through the shell's music player after it closes |
| Background downloads | `SC_FEAT_DOWNLOADS` | its downloads continue after it closes, with one progress notification |
| Self-update | `SC_FEAT_UPDATE` | its update is installed after it closes, and it is launched again |

Nothing in the plugin knows about Plex, Audiobookshelf, Music Assistant or
Suwayomi. Everything server-specific is in plain URLs, request header lines
and report templates that the app writes. The one exception is the
`plexqueue`/`plexadd` download kinds (§5.2), which drive Plex's server-side
download queue for VitaPlex.

`include/sc_protocol.h` holds the constants and structures below;
`client/` is a C library that does all of it for an app (§7).

> **Status:** implemented, not yet tested on a console. It is VitaPlex's
> quick menu plugin generalised; the parts that talk to SceShell are
> unchanged from it.

## 1. Transport

- **Pipes:** `"ShellCompanion1::ToPlugin"` and `"ShellCompanion1::FromPlugin"`.
  The `1` is the protocol version.
  - **Compatibility:** a plugin of protocol N will also serve apps of every
    lower version. The client library looks for its own version's pipes
    first, then up to 7 versions higher. Only if it finds none does it load
    the copy the app ships in its own package.
- **One app at a time:** only one app or game runs at a time on the Vita, so
  the shared pipe pair carries only that app's messages. The plugin may still
  be working for an app that has closed, playing its music or finishing its
  downloads.
- **Files:** larger data goes through files in the app's own data folder,
  which the app registers with `SC_CMD_HELLO`.
  - **Writing:** each file is written in full beside its name (`<name>.tmp`),
    then renamed into place.
  - **Completeness:** each file ends with an `end` line, and a file without one
    is ignored.
  - **Format:** one `key=value` per line. Unknown keys are skipped, so newer
    writers stay readable.
  - **Encoding:** where a value below is marked *encoded*, the characters
    `%`, `|`, tab and every control character (line ends included) are
    written as `%XX`. Any other `%XX` is decoded too, so fully URL-encoded
    values also work.

```c
typedef struct ScMessage {
    uint32_t cmd;
    uint32_t arg;
    char     titleId[12];   // the sending app, e.g. "VPLEX0001", NUL-terminated
    uint16_t text1[256];    // UTF-16, NUL-terminated (meaning depends on cmd)
    uint16_t text2[256];
    uint16_t text3[256];
} ScMessage;               // 1556 bytes

typedef struct ScPress {   // plugin -> app: a press on the Music row
    uint32_t press;        // SC_PRESS_PLAY_PAUSE, SC_PRESS_NEXT, SC_PRESS_PREVIOUS
    char     titleId[12];  // the row's owner the press was for
} ScPress;                 // 16 bytes
```

## 2. Registering: `SC_CMD_HELLO`

Send this first, every time the app opens the pipes (the client library does).

- `arg`: the `SC_FEAT_*` bits the app wants.
- `text1`: its data folder, for example `ux0:data/VitaPlex`. All files below
  live there. It must name a device (`ux0:` …) and have no `..` in it. The
  plugin makes it if it is missing.
- `text2`: its display name, used in the plugin's log and notification.

The plugin keeps a record of up to 16 apps a boot (title ID, folder, name,
features); a second HELLO from the same title updates it. A command from an
app that has not said HELLO this boot, or for a feature it didn't ask for,
is ignored and logged.

## 3. Music row

| Command | Fields |
|---|---|
| `SC_CMD_ROW_ACTIVATE` | take the row while the quick menu is open; send every second as a heartbeat. With none for 5 s, the row goes back to the system. |
| `SC_CMD_ROW_RELEASE` | give the row back now |
| `SC_CMD_ROW_SONG` | `text1` title, `text2` artist, `text3` album |
| `SC_CMD_ROW_PLAYING` | `arg` 1 playing (⏸ shown), 0 paused |
| `SC_CMD_ROW_ART` | `arg` serial, 0 for none: a 64×64 PNG at `<folder>/sc-art-<serial & 1>.png` |

- **Ownership:** the latest app to send `ROW_ACTIVATE` owns the row; the
  other row commands are ignored from any other app. A press goes to the
  owner as an `ScPress` on the FromPlugin pipe; the client drops presses
  meant for another title.
- **Taking the row stops background music:** an app that takes the row plays
  its own music, so a queue the shell was playing stops.
- **When the shell is playing an app's queue:** the presses act on that queue
  directly, and nothing is sent.
- **Art serials** must only grow during a boot, from one run of the app to the
  next, and alternate between the two files. `scCoverNextSerial` makes them.

## 4. Background music

### Handing over (`SC_CMD_MUSIC_PLAY`)

When the app closes, it writes `<folder>/sc-queue.txt`, gives the row back,
then sends `SC_CMD_MUSIC_PLAY`:

```
written=<sceKernelGetSystemTimeWide()/1e6, %.6f>
index=<track to start with, from 0>
positionMs=<where in it>
repeat=<0 off, 1 one, 2 all>
offlineSkip=<0/1: when a track cannot be fetched, skip to the next one on the card>
buttons=<tracks (default) or seek: what the row's ⏮ ⏭ do, see "Buttons" below>
seekBackMs=<with buttons=seek: how far ⏮ goes back, default 10000>
seekForwardMs=<with buttons=seek: how far ⏭ goes forward, default 30000>
headers=<encoded: header lines sent with every request below, "Name: value", \r\n between>
report.state=<encoded, optional: URL template, see §4.3>
report.state.method=<GET (default), POST, PUT, DELETE or PATCH>
report.state.body=<encoded, optional body template>
report.state.type=<encoded, the body's Content-Type; application/json by default>
report.state.every=<seconds between repeats while playing, default 10, at least 2>
report.played=<encoded, optional: URL template when a track plays to its end>
report.played.method=..., report.played.body=..., report.played.type=...
control.*=<encoded, optional: the remote control, see §4.4>
track=<fields, '|'-separated, each encoded, below>
...                                         (up to 1000 tracks)
end
```

A `track=` line has these fields:

| # | Field | Meaning |
|---|---|---|
| 0 | `id` | the app's own ID for the track: `{id}` in reports, and its files' names (with anything but letters, digits, `-`, `_` and `.` made `_`) |
| 1 | `durationMs` | its length, or 0 |
| 2 | `local` | a file on the card to play as it is, or empty |
| 3 | `url` | where to fetch it in its own format, or empty |
| 4 | `ext` | that format's extension, or empty to take it from the URL |
| 5 | `altUrl` | a URL that answers with the track as MP3, used when `ext` is a format the shell doesn't play (anything but `mp3`, `m4a`/`mp4`, `aac`, `wav`), or when there is no `url`. Empty: such tracks are skipped |
| 6 | `altPrep` | an optional URL fetched (and its answer discarded) just before `altUrl`, for example Plex's transcode decision |
| 7 | `title` | for the Music row |
| 8 | `artist` | for the Music row |
| 9 | `album` | for the Music row |
| 10 | `coverUrl` | the cover for the row: must answer with a PNG, ideally 64×64 (the plugin can't decode or scale anything else), or empty |
| 11 | `coverPng` | the same, on the card already, or empty |

`{session}` in `url`, `altUrl` and `altPrep` is replaced by a number unique
to the fetch, the same in `altPrep` and `altUrl`.

### Playback

- **Order:** the plugin plays the queue in order, starting from `index` at
  `positionMs`.
- **Sources:** for each track it uses `local` if the shell plays its format,
  otherwise a copy it fetched into `<folder>/sc-songs/<id>.<ext>` (from
  `url`, or from `altUrl` as `.mp3`, framed with a length header as it
  comes). A file already there from an earlier queue is used as it is.
- **Fetching ahead:** it fetches the next track while the current one plays,
  and the covers of the current and next tracks into `sc-songs/<id>.png`.
  A track with the same `coverUrl` as the one before reuses its cover.
- **On the card:** it keeps the previous, current and next tracks' files, and
  all of them when the app takes the music back, so the app can play on from
  them.
- **Missing tracks:** a track it can't get is skipped; three in a row stop the
  queue. When the server doesn't answer at all and `offlineSkip=1`, it skips
  to the next track on the card; with none, it holds paused.

### Buttons

While the shell plays the queue, the row's ⏮ and ⏭ either move between
tracks (`buttons=tracks`, the default: ⏮ restarts a track past its first
3 s) or, with `buttons=seek`, move within the track by `seekBackMs` and
`seekForwardMs`, as an audiobook player does. Seeking forward past a track's
end goes on to the next track; seeking back stops at its start. While the
app itself plays, the presses go to the app, which does what it likes with
them.

### 4.3 Report templates

Optional. They're how the app's server learns the playback position while
the app is closed. Placeholders in the URL and body are filled in when the
report falls due:

| Placeholder | Value |
|---|---|
| `{id}` | the track's `id`, as the queue gave it |
| `{state}` | `playing`, `paused`, `buffering` or `stopped` |
| `{positionMs}` / `{positionS}` | position in ms / seconds (with 3 decimals) |
| `{durationMs}` / `{durationS}` | duration in ms / seconds (with 3 decimals) |
| `{index}` | the track's place in the queue, from 0 |

- **When:** at every state change, and every `report.state.every` seconds while
  playing. `report.played` when a track ends, with `{state}` `stopped`.
- **Order:** reports are sent one at a time, in order, with the queue's
  `headers`. Only the latest state waits to be sent, and up to 4 played
  reports.
- **Failures:** anything but a 2xx answer is logged once, until one succeeds,
  and never holds up playback.
- **Limits:** a filled-in URL up to 1023 bytes, a body up to 511.
- **PATCH** uses `sceHttpCreateRequestWithURL2`; on a firmware without it,
  PATCH reports fail and the plugin's log says so.

Examples:

- **Plex:**
  `report.state=<server>/:/timeline?ratingKey={id}&key=/library/metadata/{id}&state={state}&time={positionMs}&duration={durationMs}`
  and `report.played=<server>/:/scrobble?key={id}&identifier=com.plexapp.plugins.library`,
  with `headers` carrying `X-Plex-Token` and `X-Plex-Client-Identifier`.
- **Audiobookshelf:** `report.state=<server>/api/me/progress/{id}`, with
  `report.state.method=PATCH` and
  `report.state.body={"currentTime":{positionS},"duration":{durationS}}`.

### Status and taking back

- **Status:** `<folder>/sc-queue-status.txt` is written every 2 s and at every
  change:

  ```
  written=<as above>
  queue=<the queue's written=>
  index=<the track's place>
  id=<its id>
  positionMs=
  durationMs=
  state=playing|over|stopped
  fetching=<1: waiting for that track; it plays from positionMs once fetched>
  paused=<1: paused from the row>
  end
  ```

- **`SC_CMD_MUSIC_TAKE_BACK`:** the app sends it once its own player is ready.
  The plugin stops the shell's music, writes the status one last time with
  the exact position, and the app continues from there. A queue handed over
  and not yet begun is not begun. Only the app whose queue it is can take it
  back.

### 4.4 Remote control

A queue can have the plugin listen for a remote while the shell plays it:
the app's server, or another app on the network, then skips, pauses, seeks
or starts a track of the queue with the app closed. The plugin knows no
remote protocol: the app writes the paths to answer, what each means, and
the replies, as templates like the reports'. The listener runs only while
the shell plays the queue, and stops with it.

```
control.port=<TCP port to answer HTTP on>
control.headers=<header lines on every reply, and on the pushes>
control.reply.<name>=<a reply body template; up to 4, up to 1535 bytes>
control.route=<path>|<commands>|<reply name>      (up to 20 routes)
control.notFound=<reply name for any other path; a plain 404 without>
control.discovery.port=<UDP port to answer discovery searches on>
control.discovery.group=<a multicast group to join for them, or none>
control.discovery.match=<what a search begins with>
control.discovery.reply=<the answer, a template>
control.discovery.hello=<a.b.c.d:port>|<sent there as the listener starts>
control.discovery.bye=<sent there as it stops>
control.push=<URL template>|<method, POST by default>|<reply name for the body>|<Content-Type>
control.push.every=<seconds between pushes to subscribers, default 1>
```

- **Paths** match exactly, without the query. `OPTIONS` requests (browser
  preflights) are answered with permissive CORS headers.
- **Commands**, `,` between, each `name` or `name:param` where `param` names
  the query parameter that carries its value:

  | Command | Does |
  |---|---|
  | `play`, `pause`, `toggle` | play, pause, or switch |
  | `next`, `previous` | the next track; the previous one, or this one's start past 3 s (whatever `buttons` says) |
  | `stop` | stop the queue, as at its end; the listener stops too |
  | `seekTo:p` | to `p` ms in the track |
  | `forward[:p]`, `back[:p]` | by `p` ms, or the queue's `seekForwardMs`/`seekBackMs` |
  | `skipTo:p` | to the track whose `id` is `p`, or whose `id` `p` ends with after a `/` (`/library/metadata/123`). With `seekTo` in the same route, it begins at that place |
  | `subscribe:p` | the asker's address, with port `p`, gets `control.push` every `control.push.every` seconds and at every change; its `commandID` parameter, on this and its later requests, is `{commandID}` |
  | `unsubscribe` | stops that |
  | `wait` | the reply waits up to 1 s for the state to change (a long poll) |

- **Replies** are sent with status 200 and the template filled in: the
  report placeholders (§4.3) for the track playing, plus `{param:NAME}` (a
  query parameter of the request) and `{commandID}`. `{state}` is `playing`,
  `paused`, `buffering` or `stopped`.
- **Pushes** go to each subscriber, with `{subscriberIp}`,
  `{subscriberPort}` and `{commandID}` filled in too. A subscriber is
  dropped after 90 s without a request from it, or three pushes in a row it
  didn't take; it is sent one last push, `stopped`, as the listener stops.
- **Discovery:** a datagram on `control.discovery.port` that begins with
  `control.discovery.match` is answered with `control.discovery.reply`.

**Plex** (VitaPlex), as a Plex player that Plex's apps can control
(the companion protocol: HTTP on 32500, GDM discovery on 32412):

```
control.port=32500
control.headers=Content-Type: text/xml\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Plex-Client-Identifier\r\nX-Plex-Client-Identifier: <the app's id>
control.reply.ok=<Response code="200" status="OK"/>
control.reply.resources=<MediaContainer><Player title="<name>" protocol="plex" protocolVersion="1" protocolCapabilities="timeline,playback" machineIdentifier="<the app's id>" product="VitaPlex" platform="PlayStation Vita" deviceClass="handheld"/></MediaContainer>
control.reply.timeline=<MediaContainer commandID="{param:commandID}" location="fullScreenMusic"><Timeline type="music" state="{state}" time="{positionMs}" duration="{durationMs}" ratingKey="{id}" key="/library/metadata/{id}" machineIdentifier="<server id>" address="<server host>" port="<server port>" protocol="<http or https>"/><Timeline type="video" state="stopped"/><Timeline type="photo" state="stopped"/></MediaContainer>
control.route=/resources||resources
control.route=/player/playback/play|play|ok
control.route=/player/playback/pause|pause|ok
control.route=/player/playback/stop|stop|ok
control.route=/player/playback/skipNext|next|ok
control.route=/player/playback/skipPrevious|previous|ok
control.route=/player/playback/stepForward|forward|ok
control.route=/player/playback/stepBack|back|ok
control.route=/player/playback/seekTo|seekTo:offset|ok
control.route=/player/playback/skipTo|skipTo:key|ok
control.route=/player/playback/playMedia|skipTo:key,seekTo:offset|ok
control.route=/player/timeline/subscribe|subscribe:port|ok
control.route=/player/timeline/unsubscribe|unsubscribe|ok
control.route=/player/timeline/poll|wait|timeline
control.notFound=ok
control.discovery.port=32412
control.discovery.group=239.0.0.250
control.discovery.match=M-SEARCH
control.discovery.reply=HTTP/1.0 200 OK\r\nContent-Type: plex/media-player\r\nResource-Identifier: <the app's id>\r\nName: <name>\r\nPort: 32500\r\nProduct: VitaPlex\r\nProtocol: plex\r\nProtocol-Version: 1\r\nProtocol-Capabilities: timeline,playback\r\nDevice-Class: handheld\r\n\r\n
control.discovery.hello=239.0.0.250:32413|HELLO * HTTP/1.0\r\n<the same lines as the reply>\r\n
control.discovery.bye=BYE * HTTP/1.0\r\nResource-Identifier: <the app's id>\r\n\r\n
control.push=http://{subscriberIp}:{subscriberPort}/:/timeline|POST|timeline|text/xml
```

(`\r\n` stands for the line breaks the encoding carries.) Only tracks in the
queue can be played: a `playMedia` for anything else is answered, and does
nothing. This follows the companion protocol as open players implement it
(plex-mpv-shim); it is untested against Plex's apps.

**Sendspin** (Music Assistant) can't be served this way: a Sendspin player
keeps a WebSocket open to the server, the server streams the audio over it
and controls playback by changing the stream, and current Sendspin encrypts
the connection end to end (Noise). Remote control of a queue the shell plays
from files isn't what Music Assistant's server does.

## 5. Background downloads

### Handing over (`SC_CMD_DL_START`)

When the app closes, it writes `<folder>/sc-downloads.txt`, then sends
`SC_CMD_DL_START`:

```
written=...
notify=<1: show the progress notification>
headers=<encoded: header lines sent with every request, as in §4>
item=<fields, tab-separated, each encoded, below>     (up to 200 items, in order)
end
```

Each item has these fields:

| # | Field | Meaning |
|---|---|---|
| 0 | `id` | the app's ID for the download |
| 1 | `file` | the file to write; a partial file is continued with HTTP Range from its size |
| 2 | `size` | the expected full size, or 0 if unknown |
| 3 | `title` | for the notification |
| 4 | `kind` | `url` (§5.1), or `plexqueue` / `plexadd` (§5.2) |
| 5… | kind's own fields | see below |

The plugin waits up to 30 s for the app to close before it begins, so it
never writes a file the app is still writing.

#### 5.1 `url`

Field 5 is the URL. Field 6 is optional extra header lines for this item,
sent after the list's.

- **Resuming:** a server that answers a Range request with the whole file
  (200) has it rewritten from the start; 416 counts as complete.
- **Waiting:** 503 is waited for, every 10 s, for up to an hour.
- **Retries:** after 5, 15 and 60 s; getting any bytes resets the count.
- **Gone:** 404 and 410 mark the item `gone`.

#### 5.2 `plexqueue` / `plexadd`

These keep VitaPlex's existing behaviour: Plex's server-side download queue,
which prepares a transcoded file before it can be fetched. The list's
`headers` carry the token and client identity (`X-Plex-Token`,
`X-Plex-Client-Identifier`, `X-Plex-Product`, `X-Plex-Platform`,
`X-Plex-Device`), as the download queue belongs to the client.

- **`plexqueue`:** server, queue ID, item ID.
- **`plexadd`:** server, the add's query (from `?`), profile extra. The item is
  added to the client's download queue first.

### Status and taking back

- **Status:** `<folder>/sc-downloads-status.txt` is written every 2 s:

  ```
  written=
  running=0|1
  paused=0|1
  item=<id>\t<going|waiting|done|gone|failed>\t<bytes>\t<size>\t<queue id>\t<item id>
  ...
  end
  ```

  The last two fields are the Plex download queue's IDs the item used, and
  empty for `url` items.

- **`SC_CMD_DL_TAKE_BACK`:** the app sends it when it opens. The plugin stops
  after the piece it is writing and writes `running=0`. The app waits for
  that before touching the files.
- **Several apps:** downloads from different apps run one app's list at a
  time, in the order they were handed over. A list handed over again by the
  app whose list runs replaces it. A list still waiting is dropped when its
  app takes it back, and its status is written with `running=0` at once.

### 5.3 The notification

| Command | Fields |
|---|---|
| `SC_CMD_DL_PROGRESS` | while the app itself downloads: `text1` first line, `text2` second line, `arg` progress 0–1000 or `SC_PROGRESS_UNKNOWN` |
| `SC_CMD_DL_PROGRESS_END` | `text1`, `text2`: the notification's last two lines |

- **One notification:** the plugin owns it, so the same notification carries
  on after the app closes. Lines are cut to 63 UTF-16 units.
- **Icon:** it shows the icon of the app it was last shown for.
- **"Pause downloads?":** answering Yes pauses the plugin's own downloads
  (`paused=1`). While the app is downloading itself, the plugin creates
  `<folder>/sc-downloads-pause` instead, which the app checks for
  (`scDownloadPauseAsked`).
- **Left over:** a notification still showing 30 s after the downloads were
  taken back, with no progress from the app, is ended.

## 6. Self-update: `SC_CMD_UPDATE_INSTALL`

1. The app unpacks its update to `<folder>/sc-update/` and forges
   `sce_sys/package/head.bin` there. The client library doesn't do this: the
   usual template is VitaShell's `head.bin` (GPL-3.0); VitaPlex's
   `src/utils/vita_install.cpp` shows how.
2. It sends `SC_CMD_UPDATE_INSTALL`, then quits.
3. The plugin waits up to 30 s for the app to exit, installs the update
   (retrying while the title still counts as running), and writes
   `<folder>/sc-update-result.txt`: `result=0x…`, `why=…`, `end`.
4. It relaunches the app either way. One update runs at a time.

## 7. Client library (`client/`)

Plain C, Apache-2.0, for any app. Build `client/shell_companion.c`,
`client/sc_files.c` and `client/sc_link_stub.S` with the app, plus
`client/sc_cover.c` for covers (it needs the app's `stb_image.h`), and ship
`shell_companion.suprx` in the app's package.

```c
// setting up
int scInit(const char* titleId, const char* dataDir, const char* name, uint32_t features,
           const char* bundledPluginPath);           // who the app is; no I/O
int scOpen(char* what, size_t size);                  // finds or loads the plugin, sends HELLO
int scIsOpen(void);
int scPath(const char* name, char* out, size_t size); // a file in the data folder
// row
int scRowActivate(void);  int scRowRelease(void);
int scRowSong(const char* title, const char* artist, const char* album);
int scRowPlaying(int playing);  int scRowArt(uint32_t serial);  int scPoll(void);
// music
int scMusicPlay(void);  int scMusicTakeBack(void);
// downloads
int scDownloadsStart(void);  int scDownloadsTakeBack(void);
int scDownloadProgress(const char* l1, const char* l2, uint32_t permille);
int scDownloadProgressEnd(const char* l1, const char* l2);
int scDownloadPauseAsked(void);
// update
int scUpdateInstall(void);
// debug (§9)
int scProbeStream(int mode, const char* sourceWav);
// the files
int scWriteQueue(const ScQueue* queue, const ScTrack* tracks, uint32_t count);
int scWriteDownloads(int notify, const char* headers, const ScDownload* items, uint32_t count);
int scReadQueueStatus(ScQueueStatus* out);
int scReadDownloadsStatus(int* running, int* paused, ScDownloadStatus* items, uint32_t cap,
                          uint32_t* count);
int scReadUpdateResult(int* result, char* why, size_t whySize);
// covers (sc_cover.h)
int scCoverWrite(const void* image, size_t len, uint32_t serial, char* why, size_t whySize);
int scCoverWriteTo(const void* image, size_t len, const char* path, char* why, size_t whySize);
uint32_t scCoverNextSerial(uint32_t last, uint64_t nowUs);
```

`scMusicTakeBack` and `scDownloadsTakeBack` only reach a plugin that is in
SceShell already, and never load one: with none there, nothing was carried
on.

## 8. Logs

`ux0:data/ShellCompanion/plugin.log` holds the current boot, and
`plugin.prev.log` the boot before. Lines about an app's work name the app.
URLs and header lines are never logged, as they may carry tokens.

## 9. Debug: the streaming probe (`SC_CMD_PROBE_STREAM`)

A test for Music Assistant in the background: can the shell's music player
play a WAV that is still being written? The plugin writes 44.1 kHz 16-bit
stereo PCM into `<folder>/sc-probe-a.wav` at real-time pace, starting 3 s
ahead, gives the file to the player the way it gives it a queue's songs, and
logs what the player makes of it. It may change or go once that is answered.

```c
scProbeStream(SC_PROBE_ALL, NULL);   // or a WAV: "ux0:music/test.wav"
scProbeStream(SC_PROBE_STOP, NULL);  // stop it early
```

- **Feature:** needs `SC_FEAT_MUSIC`. It is refused while a queue is carried
  on; a queue handed over stops it and waits for it to finish.
- **`arg`, the mode:**

  | Mode | Value | The WAV's header |
  |---|---|---|
  | `SC_PROBE_STOP` | 0 | stops the run |
  | `SC_PROBE_HUGE` | 1 | a data size of `0x7FFFF000` (3 h 22 min) |
  | `SC_PROBE_GROWING` | 2 | the size written so far, rewritten after each write |
  | `SC_PROBE_REOPEN` | 3 | as 2; the file is reopened where it was at each stall and every 10 s |
  | `SC_PROBE_ALL` | 4 | 1, 2 and 3 in turn, about 4 min 20 s |

- **`text1`:** a WAV on the card to stream (PCM, 44.1 kHz, 16-bit, stereo),
  looped. Empty for a tone: 440 Hz, 880 Hz every tenth second, and the last
  100 ms of each second silent, so a jump can be heard.
- **When:** the first file is played 10 s after the command, so the app can be
  closed first. An app holding the audio may keep the shell's music silent.
- **Script:** each run lasts 80 s from its first play. Modes 1 and 2 pause at
  15 s, play again at 20 s, seek back to 5 s at 30 s, seek to 2 s short of
  what is written at 40 s, and skip to a second file (`sc-probe-b.wav`) at
  55 s. Both files are removed at the end.

What the log (`plugin.log`, lines beginning `probe:`) shows:

| Line | Meaning |
|---|---|
| `N s: X ms written …, shell state S at T ms, length L ms` | every second. With state 1 the player plays; T should keep climbing past 3000, the size the file had as it was opened. L is the length the player gives the song |
| `STALL: …` | the player's clock stopped for 1.5 s while nothing was asked of it: the last line before it says how much was written, and `as the shell opened it` how much it saw |
| `state A to B …, not asked for` | the player changed state by itself |
| `<step>: playing at T ms, N ms after it was asked` | how long a play, seek, skip or reopen took to be heard |
| `<step>: not playing from … after it was asked` | that step never took effect within 10 s |
| `open 0x8…` on `first play` | the player refused the file, probably its header |

