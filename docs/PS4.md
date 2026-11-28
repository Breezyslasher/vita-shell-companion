# Shell Companion on PS4

Shell Companion's PS4 side lets a homebrew app hand its music to the PS4's home
screen (SceShellUI), so it carries on after the app has
closed. The quick menu's **Music** tab shows the app's queue as if it were a
music app's: sections, playlists, albums, covers, the player's controls, and
editing the queue. It's the PS4 counterpart of the Vita plugin; the
[protocol document](PROTOCOL.md) covers the Vita.

> **Status:** works on a PS4 on firmware 11.50 with GoldHEN, in testing.
> The engineering notes and every finding behind it are in
> [`ps4/README.md`](../ps4/README.md).

## What it does

- **Background music:** the app's queue plays in the shell, on the system
  audio port: local MP3 and 16-bit WAV files, and **streams** (MP3/WAV over
  `http://` or `https://`, played as they download, nothing written to disk).
- **The quick menu's Music tab:** the player appears as the music app, its
  track, cover and controls (play/pause, previous, next, shuffle, repeat,
  volume, "+"). The tab lists:
  - *Shell Companion:* **Queue**, **All songs**, **Liked**
  - *Albums:* one tile an album, its cover
- **Covers:** a track's embedded JPEG or PNG cover, or a cover URL the app
  gives (the shell fetches it), on tiles, beside each track and on the player.
- **Editing the queue in the Music tab**, on a focused track:

  | List | □ | △ |
  |---|---|---|
  | Queue | pick it up: ↑/↓ moves it, X puts it down, ○ cancels | take it out |
  | any other | play it next | add it to the end |

  Picking a track in a list other than Queue makes that list the queue, from
  that track.
- **"+" (R3):** adds the track to Liked by default. An app can also have it
  call its own server at once (a Plex favourite, say), and read every press
  back when it reopens.
- **Self-update:** an app can install its own update (a `.pkg`) once it has
  closed, through the PS4's own download service, and be started again.
- **Large libraries:** up to 10,000 tracks in 1,000 albums. A local file's
  tags are read once and cached.

## Requirements

- A PS4 with **GoldHEN** (tested: 2.4b18 on 11.50).
- The app is an OpenOrbis homebrew. It must be able to use GoldHEN's
  jailbreak (`scOpen` calls it if `/data` can't be written).
- The app's package carries **`sc_companion.prx`** (the player and the
  music watch, one module) at its root. `ps4/bgm/build.sh` builds it, and
  `ps4/app/build.sh` shows how it goes in a package.

## Using it in an app

Build `ps4/client/shell_companion.c` with the app and include
`ps4/client/shell_companion.h` (Apache-2.0, libc only).

```c
#include "shell_companion.h"

scInit("ABCD00001", "My App");          // the app's title id, its name

// As the app starts: whatever the shell was doing for us, back.
scMusicTakeBack();
ScQueueStatus st;
if (scReadQueueStatus(&st) == 0) {
    // go on from st.index / st.track at st.positionMs
}

// Load the player and the watch into the shell (once a boot).
char what[256];
if (scOpen(what, sizeof(what)) != 0) log("%s", what);

// As the app closes: its queue to the shell.
ScTrack tracks[] = {
    {.url = "https://server/a.mp3", .title = "A", .artist = "Me", .album = "X",
     .coverUrl = "https://server/a.jpg", .id = "1234"},
    {.local = "/data/MyApp/b.mp3"},     // tags read from the file
};
ScQueue q = {.index = 0, .positionMs = 12000};
scWriteQueue(&q, tracks, 2);
scMusicPlay();
```

### The calls

| Call | What it does |
|---|---|
| `scInit(titleId, name)` | Once, first. |
| `scOpen(what, size)` | Loads `sc_companion.prx` into SceShellUI unless the player and the watch answer already (a module loads once a boot; a second copy would play and hook the shell twice). It is copied from the package to `/data/GoldHEN/plugins/` and loaded with GoldHEN's `PROCESS_PRX_LOAD`. 0 when both run. |
| `scPlayerRunning()`, `scWatchRunning()` | 1 if that module answers. |
| `scWriteQueue(&queue, tracks, count)` | The queue, up to 10,000 tracks, and where to begin. |
| `scMusicPlay()` | The shell loads the queue and plays it. |
| `scMusicTakeBack()` | The player stops and keeps its place (`scReadQueueStatus`). |
| `scCommand("next")` | `play`, `pause`, `toggle`, `next`, `prev`, `stop`, `volume=<0-32768>`, `repeat=<0-2>`, `shuffle=<0/1>`, `index=<n>`. |
| `scReadQueueStatus(&st)` | State, place in the queue, the track's number in the queue written, position, length, tags, volume, repeat, shuffle. |
| `scReadPlusEvents(events, cap, &n)` | "+" presses made while the app was away, then forgotten. |
| `scUpdateInstallPkg(pkg, relaunch)` | The app's own update (a `/data/...` path or URL), installed once the app has gone; the app exits right after. |
| `scReadUpdateResult(&result, why, size)` | What came of it, read as the app starts again. |

### Tracks

| `ScTrack` field | |
|---|---|
| `local` | a file the shell can read (`/data/...`) |
| `url` | else a stream (`http://`, `https://`) |
| `title`, `artist`, `album` | over a local file's tags; a stream's, or its file name |
| `coverUrl` | a JPEG or PNG by URL, fetched by the shell; used where a file has no cover of its own |
| `id` | the app's own id, for "+" (`{id}`) and its events |

Tracks with the same album are one album in the Music tab. A title taken
from a file name loses a YouTube video id at its end (`Song-QZWRu3kqkG8`).

### "+"

```c
ScPlus plus = {
    .mode = SC_PLUS_HTTP,
    .saveUrl = "http://plex:32400/:/rate?key={id}&rating=10&identifier=com.plexapp.plugins.library",
    .deleteUrl = "http://plex:32400/:/rate?key={id}&rating=0&identifier=com.plexapp.plugins.library",
    .method = "PUT",
    .headers = "X-Plex-Token: …",
    .containsUrl = "http://plex:32400/library/metadata/{id}",
    .containsMatch = "userRating=\"10",
};
ScQueue q = {.index = 0, .plus = &plus};
```

- Every press adds the track to Liked or takes it out, which lights the "+",
  and is recorded for `scReadPlusEvents`.
- With `SC_PLUS_HTTP` the request is sent at once, while the app is away.
  Placeholders `{id}`, `{title}`, `{artist}`, `{album}`, `{file}` are
  URL-encoded in a URL and JSON-escaped in a body. A failed request shows a
  notification.
- `containsUrl`, asked as each track begins, sets whether the "+" is lit: a
  2xx answer (holding `containsMatch`, if given) is "saved".

### Self-update

```c
// The new package downloaded (or its URL), as the app is about to close:
if (scUpdateInstallPkg("/data/MyApp/update.pkg", 1) == 0) exit(0);
...
// As it starts again:
int result; char why[200];
if (scReadUpdateResult(&result, why, sizeof(why)) == 0) log("update: %d %s", result, why);
```

The shell waits 3 s for the app to close, registers the package with the
PS4's download service (over what is installed), follows it, writes
`update-result.txt`, and with `relaunch` starts the app again
(`sceSystemServiceLaunchApp`).

## On the console

Everything is in `/data/ShellCompanion/ps4/`:

| File | |
|---|---|
| `bgm.log`, `mcw.log` | the player's and the watch's logs |
| `queue.txt` | the queue as written (`#start`, `#position`, `#plus.*`, then a line a track: path or URL, title, artist, album, cover URL, id, tab-separated) |
| `status.txt` | the player's status, every second |
| `liked.txt` | Liked: a path or URL a line |
| `plus-events.txt` | "+" presses not yet read by the app |
| `update.txt`, `update-result.txt` | a self-update asked for, and what came of it |
| `tags.txt` | the tag cache |
| `ebgm/` | what the Music tab is answered from (lists, covers in `ebgm/art/`) |

Switches (an empty file of that name in the folder):

| File | |
|---|---|
| `mcw-verbose` | log every call the music watch sees (mcw.log grows fast) |
| `mcw-norowart` | no covers beside tracks |
| `mcw-noqueuekeys` | no □/△ in track lists |
| `https-noverify` | streams accept any HTTPS certificate |

## Limits

- **Formats:** MP3 and 16-bit PCM WAV.
- **Audio port:** music plays on the main audio port (type 0); the others
  failed in the shell.
- **Covers:** tiles show JPEG and PNG; the shell appends `&w=192&h=192` to a
  tile's URL.
- **Background downloads:** not in this release. They were built and taken
  out as not reliable enough yet; the engineering notes in
  [`ps4/README.md`](../ps4/README.md) keep what was learned.
- **Starting position:** for a stream, the start position works only
  within what has been downloaded when it begins.

## Trying it

`ps4/app` is a test app (`IV0000-SCMP00001_00-SHELLCOMPBGM0001`), built by
CI as a package:

- **X** loads both modules and plays the queue: `yoursong.mp3` (from a USB
  drive's root or the folder), every MP3/WAV in `music/`, a test tone, and
  the lines of `streams.txt`
  (`URL | title | artist | album | cover URL`).
- **L3** installs the package named in `test-update.txt` as a self-update,
  then closes the app.
- **□ ⏯, △ stop, L1/R1 previous/next, R3** the quick menu's answers on or
  off, **OPTIONS** writes the test tone again.

`SC_DEBUG=1 sh ps4/bgm/build.sh` builds the debugging aids in: the shell
code dump and the assembly carver (`mcw.carve`).

Restart the PS4 after installing a new version: the modules load once a
boot.
