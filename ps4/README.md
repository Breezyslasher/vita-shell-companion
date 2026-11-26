# PS4: background music and quick menu controls

## Background music (first version)

**Working:** on a PS4 on firmware 11.50 with GoldHEN 2.4b18.12, the test app
loads the player (`sc_bgm.prx`) into the shell (`SceShellUI`), and it plays
the queue there: MP3 or 16-bit WAV, on audio port type 0 (MAIN), 1 or 2.
It does not show in the quick menu yet; that comes later.

What it took, each found on the console:

| | Why |
|---|---|
| **GoldHEN's toolchain** (illusion0001's fork of OpenOrbis, as GoldHEN's own plugins are built; CI downloads it) | the PS4 refused every module the official OpenOrbis toolchain made as not executable (ENOEXEC), where it loaded GoldHEN's own plugin |
| **16 KB pages** (`-z max-page-size=0x4000 -z common-page-size=0x4000`) | as ps4-hen-rest and ps4-thumbgrid-ime link their shell modules |
| **The module file 0777** | the shell answered EACCES for a module written 0666 |
| **`/user/data/...`**, for the module and all the player's files | the shell's sandbox has `/user`, not `/data` |
| **libkernel for files, memory and the clock** (`sceKernelOpen`, `sceKernelMkdir`, ...), and its functions linked, not looked up | with system calls of its own, the player never got to its log inside the shell |
| **`module_start` only starts the player's thread** | the shell waits for `module_start` |

Its files are in `/user/data/ShellCompanion/ps4/` (the same folder as
`/data/ShellCompanion/ps4/` over FTP); the module is copied to
`/user/data/GoldHEN/plugins/sc_bgm.prx` and loaded with GoldHEN's
`PROCESS_PRX_LOAD` (system call 500, command 6).

### A list's files in Notifications > Downloads (0.96, an experiment)

The download service takes packages only (0.92-0.94: a media file is
listed, then fails with `0x80990033` as soon as it is started, without an
HTTP request). `ps4/bgm/dllist.c` registers each `url` and `pages` item of
a download list as a placeholder task and **never starts it**, so the
service leaves it alone. Our downloader fetches the file. The service's
native progress functions (`sceBgftServiceDownloadGetProgress` and the
`Int` one, in libSceBgft) are hooked inside the shell (hook kind 18,
answered by `dlListHook`): for a placeholder they answer with the item's
bytes, size, time left and no error. These are native hooks, as the music
service's are, not the managed wrappers 0.85 hooked.

- The placeholders are removed 10 s after the list ends, when the app
  takes the list back, and at the next list for any a crash left
  (`dl-list-tasks.txt`), with `sceBgftServiceIntDownloadUnregisterTask`
  (else `sceBgftServiceDownloadStopTask`).
- **Not known yet:** whether a task never started is listed; which
  native call the list makes; what the state bits mean. While going, the
  answer is `0x30a`, the probe's task just after starting. The first
  answers for every other task, the PS4's own downloads included, are
  logged as `dllist:` lines in bgm.log, to learn a real download's bits.
  `dl-list-bits.txt` (`going=`, `waiting=`, `done=`) sets them without a
  rebuild; `dl-nolist` turns the placeholders off.
- **0.96 on the PS4:** no crash. The list calls
  `sceBgftServiceIntDownloadGetProgress` (two threads, about once a second)
  for our placeholders and its own tasks. A placeholder never started still
  answers bits `0x308`, error `0x80990033`, as the probe's did once started.
  The PS4's finished downloads answer `0x188` or `0x213` (the bits' top 16
  are not always 0: `0x5ca1e308`, `0x3136398a`). The test's files were there
  already (416), so none was seen going. 0.97 answers `0x188` for a finished
  one, logs other tasks each time their bits or bytes change (a real
  download's states), and the test app deletes its files first.
- **0.97 on the PS4:** the three songs were listed under their titles, a
  "?" for an icon. The one going (bits `0x30a`) read "Preparing to
  download… (0%)": the list reads the bits we answer, and `0x30a` is
  preparing, not downloading. The others read "Cannot download", the
  finished one too, though answered `0x188` with no error: the failure
  comes from elsewhere, most likely the task query's param (`0x200`).
  0.98 hooks the task queries (`sceBgftServiceIntDownloadQueryTask`,
  `...Section`, and the public ones, whichever exist), clears `0x200` on
  our tasks' pairs, logs their first calls, and pauses each placeholder as
  it is registered (`dl-list-nopause` not to), logging its bits before and
  after.

### One module, and the cleanup (0.95)

- **One module, `sc_companion.prx`:** `ps4/companion.c` starts the player
  (`bgmStart`) and the music watch (`mcwStart`). `ps4/bgm/build.sh` builds
  it from `bgm.c`, `mcw.c`, `hook.c`, `hde64.c` and `mp3.c`. The client
  library loads it when neither half answers, and asks for a restart when
  only one does (an older pair of modules from earlier in the boot).
- **Debugging aids only with `SC_DEBUG=1`:** the code dump, the assembly
  carver, the download service's logging hooks and the media experiment.
- **The watch logs each call only with `mcw-verbose`** (before, always
  unless `mcw.quiet`).
- **Gone:** the music service probe (`ps4/mps`), the raw loader
  (`ps4/bgm/load.c`, `raw.x`), the early probes (`ps4/probe/*_probe.c` and
  their build), the test package's extra modules (`recipe/`, `control/`)
  and playlist picture.
- **The test app:** the old ways of loading, the shell code dump, the music
  service test, the audio port switch (`port.txt` is removed; the player
  falls back to type 0) and the experiment's button are gone.
- **CI's PS4 job** builds the module and the test app only.

The notes below record what each version found, and name files that have
since gone.

### Trying it with the test app (easiest)

The test app needs no BinLoader. Install
`IV0000-SCMP00001_00-SHELLCOMPBGM0001.pkg` once (copy it to a USB stick or
`/data/pkg` and use GoldHEN's Package Installer), then open **Shell
Companion BGM Test** from the home screen.

- It loads the player into the shell at once. A `yoursong.mp3` at the root
  of a USB drive (beside the `.pkg`) or in `/data/ShellCompanion/ps4/` is
  copied into `music/` and played first; with no music at all it writes
  `music/test-tone.wav` (a C-E-G-C tune) so there is something to hear.
- **X** load / play, **Square** pause, **Triangle** stop, **R1** / **L1**
  next / previous, **Left** / **Right** another audio port type (0, 1, 2),
  **Options** write the test tone again.
- The screen shows whether the player is running, the track, and the end
  of `bgm.log`.
- Press the PS button and start a game: does the music keep playing?
- Its own log is `/data/ShellCompanion/ps4/app.log`.

### Trying it with the BinLoader

On GoldHEN the BinLoader is off until you turn it on in GoldHEN's settings,
and is usually on port **9090**; "Connection refused" means it is not
listening.

1. By FTP (GoldHEN's, port 2121), put some MP3s (or 16-bit WAVs) in
   `/data/ShellCompanion/ps4/music/`. They play in name order.
2. Send `sc_bgm.bin` to the BinLoader: `nc -v <ps4-ip> 9020 < sc_bgm.bin`.
3. A notification says "Now playing: ..." and the first track plays.
4. Start a game. Does it keep playing?
5. Send `/data/ShellCompanion/ps4/bgm.log` and `bgm-load.log`, and
   `status.txt`.

Sending `sc_bgm.bin` again while it runs does not load a second player: it
starts the queue again.

### Controlling it

Everything is in `/data/ShellCompanion/ps4/`:

| File | What |
|---|---|
| `music/` | the tracks played when there is no `queue.txt` |
| `queue.txt` | the queue instead: one absolute path a line (`/data/...mp3`); `#start 3` starts at the fourth |
| `cmd` | upload a file with one word in it: `play`, `pause`, `toggle`, `next`, `prev`, `stop`, `reload` (read the queue again), `quit`. The player reads it and deletes it. |
| `port.txt` | the audio port type, if MAIN (0) is not heard: `1` is BGM, or `14` |
| `status.txt` | written every second: state, track, position |

An app will later drive these the way the Vita client does (`scWriteQueue`
then `scMusicPlay`).

### If it is not heard

- If nothing plays and `bgm-load.log` shows `PROCESS_PRX_LOAD` failing, the
  shell probe below will say more.
- If it plays on the home screen but not in a game, try `port.txt` with `1`,
  then `14`, and send `reload` then `play` as `cmd`.

### Building

```sh
docker run --rm -v "$PWD":/src -w /src golang:1.22 sh ps4/probe/get_create_fself.sh
docker run --rm --user 0 -v "$PWD":/src -w /src openorbisofficial/toolchain:latest sh ps4/bgm/build.sh
```

`build-ps4/sc_bgm.bin` is the payload (the module, `sc_bgm.prx`, is inside
it). The CI builds it with the probes.

## Music watch: what the quick menu sends and expects back

The quick menu's Music section is the shell's, and talks to the system's
music server through `libSceMusicCoreServerClient` and
`libSceMusicCoreServerClientJsEx` (`sceMusicCoreServerPlayControl`,
`GetPlayStatus`, `GetTrackInfo`, ...; names from shadPS4's aerolib). No
signatures for them are public. `sc_mcw.prx` (ps4/mcw) is loaded into the
shell and watches them, **changing nothing**: each function is hooked, and
every call is passed on as it came, then logged with

- its six argument registers and what it returned,
- for each argument that points to readable data, the first 128 bytes it
  points to before the call, and the 16-byte rows (of 512) the call
  changed: the answer it was given,
- for the rest, the first four stack words, in case it takes more than six
  arguments.

A getter (`Get...`, `Check...`) the quick menu may ask many times a second
is logged only when its answer changes, with how many calls gave the same
answer in between. Every 10 s the log shows how often each was called. A
callback given to `RegistEventCallback` is hooked too, so the events the
server sends the shell show up. The `sceCustomMusicCore*` functions a music
core calls are on the list as well; the shell doesn't have them, so they
show up only once the module is in a core's process.

### Trying it

1. Install the 0.28 test app (0.26 hooked nothing: its module list hid the
   system libraries; 0.27 logged every poll). Restart the PS4 if an older watch is loaded
   (a module loads into the shell only once each boot).
2. Open the test app and press **UP**. A notification says how many
   functions it watches.
3. Play something with the **USB Music Player**. Open the quick menu's
   Music section, press pause, play, next and previous, and change shuffle
   and repeat. Note the time of each press.
4. Send `/data/ShellCompanion/ps4/mcw.log` (and `mcw.1.log`, if it's there).

| File in `/data/ShellCompanion/ps4/` | What |
|---|---|
| `mcw.log` | the log; past 8 MB it becomes `mcw.1.log` and a new one starts |
| `mcw.off` | if it's there when the watch starts, nothing is hooked |
| `mcw-names.txt` | the functions to hook instead, one a line (`PlayControl` or a full name) |
| `mcw.quiet` | while it's there, nothing is logged (checked every 10 s) |

### What a PS4 showed (0.27, 11.50, no music core running)

With only our own player running, the Quick Menu's music widget polls:

| Call | How often | What it saw |
|---|---|---|
| `GetPlayStatus(status*)` | about 15 a second | returns 0, and fills a 0x70-byte struct the same way every time: `+04` 3, `+08` 100, `+10` 281, `+2C` 1, `+30` 0x8000, `+44` 1, `+48` -2, `+54` 1, `+58` 1, `+60` `"NPXS20117"` (a system title: the music core it knows?) |
| `GetEvent(...)` | 60 a second | always `0x80FD0014` (no event, it seems) |
| `GetStatusOfTracks` | about once a second | logged once, unchanged |

0.27 logged every poll anyway: arguments left over in registers pointed to
memory other threads change. From 0.28 a getter's answer is only the rows
the call itself wrote, and it's logged at most 5 times a second.

### The USB Music Player and the quick menu: the protocol (11.50)

Seen with the watch while the USB Music Player played `yoursong.mp3` from a
USB drive and its quick menu entry's buttons were pressed:

**`GetPlayStatus(status*)`**: returns 0 and fills 0x70 bytes:

| Offset | Seen | Meaning |
|---|---|---|
| `+04` u32 | 2 playing, 3 paused | state |
| `+08` u32 | 100 | volume |
| `+10` u64 | 281 | the track's length, s (the MP3 is 4:41) |
| `+18` u64 | 1, 2, 3 ... once a second | position, s |
| `+2C` u32 | 1 | (track count?) |
| `+30` u32 | 0x8000 | ? |
| `+44` u32 | 1 | ? |
| `+48` s32 | -2 | the core's id: PlayControl's first argument |
| `+54`, `+58` u32 | 1, 1 | ? |
| `+60` char[] | `NPXS20117` | the core's title (the USB Music Player) |

**`GetEvent(event*, 1, ...)`**: polled 60 times a second; `0x80FD0014`
when there is nothing, else 0 with `event+0` = **2**, about once a second
while playing and at once after each button. The shell then calls
GetPlayStatus.

**`PlayControl(core, command, 0, 100, 100)`**: `core` -2 (GetPlayStatus's
`+48`). Command **1 play** (state 3 to 2), **3 pause** (2 to 3, the position
kept). Next and previous weren't pressed.

**`GetTrackInfo(index, uri*, ...)`**: index 0 filled `uri` with
`file:///usb264499990_music/yoursong.mp3`, the track's file; the call took
13 ms. Its title and artist come from elsewhere (the file's tags?).

**`LaunchUsbMusicCore(3, 1)`**: called twice, returned 0, when the music
started. **`GetPlayStartCondition(titleId, out*)`**: called from the
Library's option menu with a game's title (`CUSA01780`): `out` `00 01 01 00`,
whether music may play on in that game, it seems.

### It works: our player in the quick menu (0.33, 11.50)

With the watch answering as a core (R3), the quick menu showed a music
entry for our player with the MP3's title, artist and cover, and its
buttons worked: pause and play (`PlayControl` 0xFFFFFFFE, commands 3 and 1),
next (0xFFFFFFFC) and previous (0xFFFFFFFD), each reaching our player.
While the test app is open its own entry shows as well; the music entry is
our player's, which plays on in the shell when the app is closed.

Volume: the entry's slider calls `SetAudioVolume(level)`, 0 to 0x8000
(0 dB), ten times a second; the server refused it (0x80FD0005) as there is no
real core. From 0.34 the watch sends it to our player (`volume=<level>` as
`cmd`, applied to its audio port) and reports it in GetPlayStatus: on a PS4
the slider took our player from 0 dB to silence and back.

The Music section still showed Spotify's "Sign in to PlayStation Network"
with `CUSA01780` in `mcw-fake.txt`, but 0.34 read the title only when our
player became the core, so the shell was never told: from 0.35 a new title
is announced at once (core ready, track, status).

### 0.36: on by default; shuffle and repeat

The watch answers the quick menu for our player whenever it has a track,
unless `mcw-fake.txt` says `off` (R3 toggles it). Shuffle and repeat are
ours: `SetShuffle` and `SetRepeatMode` go to our player (`shuffle=<0|1>`,
`repeat=<0|1|2>` as `cmd`), which at a track's end plays it again (repeat
one), another at random (shuffle; once through the queue unless repeat), the
next, or from the first (repeat all); GetPlayStatus reports both.

The Music section (Spotify's panel) asked for a PSN sign-in with our player
announced as `CUSA01780` (0.35), but only for a second: 0.35's R3 removed
`mcw-fake.txt`, so it fell back to `SCMP00001`. The panel called nothing of
the music server's while it showed "Sign in" (no GetSpTrackInfo, no
GetCoreInfo of its own), which suggests the sign-in check comes first.

### The Music section shows our player too (0.36, as CUSA01780)

With `CUSA01780` in `mcw-fake.txt` (Spotify's title, as it turns out: the
quick menu then listed our player as "Spotify", with its icon), the Music
section showed Spotify's player panel for our track: its title, artist,
time and buttons (play/pause, previous, next, volume, shuffle, repeat, and
R3 "+", Spotify's add to library), above its "Sign in to PlayStation
Network" message, which stays. The sign-in check guards Spotify's own part
(playlists); the player panel reads the music server, as the music entry
does. The cover did not show there.

### 0.37: Spotify mode by default, and its cover

The watch now names our player `CUSA01780` unless `mcw-fake.txt` gives
another title (`SCMP00001` for the test app's own name and icon) or `off`.
The Music section's panel loads its cover by address alone
(`CoverArtLoadAsync(string uri)`, no offset or size), so our player writes
each track's cover to a file of its own (`cover-<n>.jpg` or `.png`, a new name
each track, the last removed) and both panels are given that file.

### Where the Music section's lists come from

The shell's string table (its app.exe's `#US` heap: the strings the
compiled-away method bodies used) has the Music section's requests, all to
Sony's PSN web API through `sceNpWebApi*`, so all needing a signed-in PSN
account:

- `/v1/ebgm/me/partner/profile?partner=spotify` (`subscriptionType`,
  `legalCountry`, `PREMIUM`)
- `/v1/ebgm/recommended/views/{0}` (`views`, `section_title`, `image_url`
  with `&w={0}&h={0}`, `playlists`): the sections and their playlists
- `/v1/ebgm/{0}/tracks?uri={1}` and `?context_uri={1}&track_uri={2}`, with
  `&offset={0}&limit={1}` (`track_list`, `artists`, `explicit`,
  `is_playable`): a playlist's tracks
- `/v1/ebgm/nowplaying/tracks?context_uri={0}&track_uri={1}`
- `/v1/ebgm/users/me/library/contains|save|delete/{0}`: R3's "add to your
  music"
- `/socialnetwork/api/v1/sp/sessionlink` (`token`, `track_uri`,
  `context_uri`, `track_index`, `start_view`)

A choice is played through the music server: `PlaySpUri(loginInfo, playUri,
trackIndex, startView, shuffle)`, which the watch hooks. Our own lists
there would mean answering those requests, and first getting past the
sign-in check; 0.38 watches `sceNpGetState` and the rest, and the web API's
calls, to see what that check asks.

### The Music section's sign-in is one failed request (0.38)

Each time the Music section opened, the shell made one PSN web API request:
`sceNpWebApiCreateContext`, then `CreateRequest` with API group `music` and
path `/v1/ebgm/me/partner/profile?partner=spotify` (GET), a
`current_timestamp` header, then `SendRequest`, which failed at once with
0x80550003 (signed out), and it showed "Sign in to PlayStation Network". It
called no `sceNpGetState` or other sign-in check.

### Answering it (0.39, an experiment)

A request of group `music` whose answer is a file in
`/data/ShellCompanion/ps4/ebgm/` is answered with it: `SendRequest` and
`SendRequest2` succeed, the status is 200, `ReadData` gives the file,
`Content-Length` and `Content-Type` are answered. The file is named after the
path after `/v1/ebgm/`, each character not a letter or digit made `_`, with its
query, then without (`me_partner_profile_partner_spotify.json`, then
`me_partner_profile.json`). Any other request, of another group or with no
file, is left as it was and logged with the file looked for, so each next
request shows in `mcw.log`. The test app writes a first guess for the profile
(`{"partner":"spotify","subscriptionType":"PREMIUM","legalCountry":"US"}`).

### The profile was taken (0.39)

With `me_partner_profile.json` answering, the Music section asked next for
`/v1/ebgm/recommended/views/SCMP00001_00`: the sections and their playlists
for the app in front (its title, `_00`), as Spotify's showed "Featured on
PlayStation: MLB The Show" while that game ran. From 0.40 a file may also be
named without the path's last part (`recommended_views.json`, for any app),
and the test app writes a first guess at the views each time it starts
(unless `ebgm/keep` is there): one section, "Shell Companion", with one
playlist under several key names (`items`, `playlists`, `content.items`).

### Past "Sign in" (0.40)

With the profile and a views answer, the Music section no longer asked to
sign in: it showed Spotify's player panel for our track, its cover now
shown (0.37's cover file), and below it "No playlists.": all 939 bytes of
the views were read and nothing else was asked. 0.41's guess makes each view
a playlist itself, with its section's title (the shell's strings have
`section_title` before `views`), the nested lists kept inside it too.

### Which keys the shell reads (0.42, an experiment)

0.41's views were read whole and the Music section still showed "No
playlists.", with nothing more asked. Rather than guess on, the watch asks
Mono itself, in the shell (`libmonosgen-2.0`: `mono_get_root_domain`,
`mono_thread_attach`, `mono_image_loaded("Sce.PlayStation.Json")`,
`mono_class_from_name(…, "JsonObject")`, `mono_class_get_method_from_name`,
`mono_compile_method`), for the native code of `JsonObject`'s `this[string]`,
`ValueOrDefault`, `ContainsKey` and `TryGetValue`, hooks them, and for 10 s
after it answers one of the Music section's requests logs each key asked for
(`json: Item("views") = …`, `json: ContainsKey("…") = yes|no`). An empty
`mcw-nojson` file turns this off.

### The keys, from the shell (0.42)

Mono's API worked in the shell: the root domain, the thread attached,
`Sce.PlayStation.Json`'s `JsonObject` and its four methods' code, all hooked.
Reading our answers, the shell asked for:

- the profile: `RootObject` (the parser's own wrapper), then `spotify`
  (missing: its fields belong under it)
- the views: `RootObject`, `views` (there), each view's `name` (there) and
  `lists` (missing: where its playlists go), then `upgrade_scopes_required`

0.43 answers `{"spotify": {"subscriptionType": "PREMIUM", "legalCountry":
"US"}}` and `{"views": [{"name": "Shell Companion", "lists": [ ... ]}],
"upgrade_scopes_required": false}`; a list's own keys come next.

### Our playlist in the Music section (0.43)

With the answers in the shape the shell reads, the Music section showed a
"Shell Companion" section with "Shell Companion queue, 2 Songs", beside
Spotify's player panel for our track. A list's keys, as the shell read them:
`image_url`, `name`, `total`. To its right: "Enhance your gaming experience
with music. Download Spotify": selecting the playlist made no request, no
`PlaySpUri`.

0.44: the playlist's picture is the test app's icon
(`file:///user/data/ShellCompanion/ps4/ebgm/playlist.png?v=1`; the shell adds
`&w=..&h=..` to `image_url`), and the Music section's own check that Spotify's
app is there and recent enough,
`PlayStationMusicContext.IsValidAppVer(bool)`, a managed method in the
shell's `app.exe`, is hooked through Mono like `JsonObject`'s and answered
true while `ebgm/` is there; `get_AppVer`, the version it found, is logged.

### Choosing the playlist (0.44)

`IsValidAppVer` was hooked through Mono (the shell's own answer 0, ours 1);
`get_AppVer` was too short to hook. Choosing our playlist opened
`psnsint:psmusicshare`, Spotify's linking page, as the views had no
`accounts_linked` (the shell asked for it on the views' root). 0.45's views
have `"accounts_linked": true`.

### "Download Spotify" (0.45, 0.46)

With `accounts_linked`, choosing the playlist no longer opened Spotify's
linking page, but "Download Spotify" stayed beside it and X moved there:
`IsValidAppVer` answered true is not all that is read. Its object's
`m_appVer` (the Spotify app's version found, 0 for none: `get_AppVer` is
`vmovss xmm0, [rdi+0xb8]`) is made 9.99 after each `IsValidAppVer`, and
`NotifyAppVerChanged(float)` is hooked and its argument made 9.99 before the
call, while `ebgm/` is there.

### "Download Spotify" is the installed-apps list (0.46, 0.47)

0.46 made the version 9.99 (`NotifyAppVerChanged(0.00) made 9.99`,
`IsValidAppVer … made 9.99`) and "Download Spotify" stayed. The Music
section's panel watches the installed apps for Spotify's entry
(`UserContext.SpotifyAppBrowseItemData`, `OnMusicAppBrowseItemChanged`):
none, so it offers the download, and X goes there.

0.47 built the test app as `CUSA01780` too; the PS4 refused to install it
(CE-35237-0), with no `CUSA01780` folder under `/user/app`, `/user/appmeta`
or `/system_data/priv/appmeta`. CI builds it no more (`build.sh` still takes
`TITLE`, `TITLE_ID`, `CONTENT_ID`).

From 0.48, in `IsValidAppVer`'s hook (a shell thread Mono knows), the watch
follows the Music section's context to its `m_userContext`, and when
Spotify's app entry (`SpotifyAppBrowseItemData`) holds no `AppBrowseItem`
while the app in front's (`BigAppBrowseItemData`) does, puts that item in
Spotify's (`mono_field_set_value`) and calls its
`NotifyAppBrowseItemChanged` (`mono_runtime_invoke`). Each step is logged
(`mono: spotify entry: …`).

### The Music section's own code (0.49)

Once per boot, unless `code/methods.map` is there, the watch writes the
machine code of the Music section's classes (`PlayStationMusicPanel`,
`PlayListPanel`, `PlayListPanelItem`, `PlayListMenuData`,
`PlayStationMusicContext`, `MusicPlayerPanel`, `ControlPanel`, the track list's
classes, `UserContext`, `AppBrowseItemData`, `DirectPlayback`) to
`code/code.bin`, and `code/methods.map`: each class's fields and their offsets,
each method's place in `code.bin`, its address, size and full name, and each
call in it (walked with HDE64, followed through `jmp [rip]` stubs) with the
method called, named by Mono. Methods with no AOT code (abstract, P/Invoke,
runtime) are skipped, as asking for them would abort the runtime; the one
being asked for is kept in `code/dumping`, and one the shell went down on is
skipped from then on (`code/skip.txt`).

### What the code says about "Download Spotify" (0.49's dump)

0.49 dumped 349 methods (77 KB, 1284 calls named) with no trouble, and
0.48's check found Spotify's app entry not empty (left as it was): that
guess was wrong. The code instead:

- `PlayStationMusicPanel.OnItemSelected(PlayListMenuData)`: type (`+0x28`) 0,
  a playlist: a call, then `FocusToChildMenu()`: focus to the panel on the
  right, the playlist's `TrackListMenuPanel`.
- `TrackListMenuPanel.OnTotalChanged(total, errorCode)` keeps the track
  query's error code (`+0x2e4`); `UpdatePanelErrorMessage` picks the message
  by it: 0x80BA0028, 0x80BA0027, 0x80BA0029, else another; its button
  (`<Create>b__17_0`) opens a link chosen by the same codes. "Enhance your
  gaming experience with music. Download Spotify" is that panel in error.
- `TrackListPanel.StartQuery` hands the query to a task
  (`PostCancellableTask`), in a nested class the dump did not have, and no
  tracks request was ever made: the error comes before any.

0.50 dumps the nested classes too (`code/methods2.map`, `code/code2.bin`),
and logs `TrackListPanel.OnQueryFinish(errorCode, list)` and
`TrackListMenuPanel.OnTotalChanged(total, errorCode)` as they are called.
On a PS4 it took the shell down twice at a generic nested class,
`ControlPanel/<>c__DisplayClass41_0`1` (each instance has its own code; the
class none), the second time though its constructor was in `skip.txt`. From
0.51 generic classes (a `` ` `` in the name) and generic methods are skipped.

### Why the track list fails: Spotify's app isn't installed (0.51's dump)

0.51's dump (381 methods, nested classes too; one generic class still took
the shell down once, then was skipped) and log: the track list's query ends
with 0x80BA0028 (`OnTotalChanged(0, 0x80BA0028)`). Its task,
`TrackListPanel/<>c__DisplayClass25_0:<StartQuery>b__0`, before any request:

1. `ApplicationMonitor.Util.AppInfo.IsInstalled(<a title>)`: false →
   0x80BA0028 ("Download Spotify")
2. `PlayStationMusicContext.IsValidAppVer(true)`: false → 0x80BA0029
3. `UT.Get<bool>(<the profile's JSON>, <a key>)`: false → 0x80BA0027

0.52 hooks `IsInstalled(string titleId)` (a static method of the nested class
`ApplicationMonitor/Util/AppInfo`) and answers true for `CUSA01780` while
`ebgm/` is there, logging each title it is asked about.

### Past "Download Spotify" (0.52)

`IsInstalled("CUSA01780")` answered true: the Music section then said
"Cannot obtain playlist." (Refresh), the query's error 0x80550003. Its first
request was not a `music` one: group `socialnetwork`, GET
`/socialnetwork/api/v1/sp/sessionlink` (Spotify's session link: the
shell's strings near it are `token`, `track`, `track_uri`, `context_uri`,
`track_index`, `start_view`). From 0.53 that group is answered too, from
`ebgm/sp_sessionlink.json` (the path after `/socialnetwork/api/v1/`), a first
guess the test app writes.

### The tracks request (0.53)

With `sp_sessionlink.json` answering (the shell read nothing from it but the
answer itself), the shell read the playlist's `uri` and asked for its
tracks: `/v1/ebgm/playlist/tracks?uri=spotify%3Auser%3Ashellcompanion%3Aplaylist%3Aqueue&nav_id=…&title_id=SCMP00001_00&section_title=Shell%20Companion&offset=0&limit=15`,
which `ebgm/playlist_tracks.json` answers for any playlist. 0.54 writes a
first guess: `total`, `offset`, `limit`, and `track_list`, each track with
`name`, `uri`, `artists[].name`, `album.name`, `explicit`, `is_playable`,
`duration_ms` and `image_url`.

### Our tracks in the Music section (0.54), and choosing one (0.55)

`playlist_tracks.json` was taken: the shell read `total`, `offset`,
`track_list`, and each track's `name`, `explicit`, `artists[].name` and
`is_playable` (and `partnerProfile`, which it adds itself), and the track
list ended with no error, 3 items (our two tracks and Play All).

Choosing one (`TrackListMenuPanel.OnItemSelected`, from 0.51's dump) posts a
task that ends in `MusicCoreServerController`, then the native
`sceMusicCoreServerPlaySpUri(loginInfo, playUri, trackIndex, startView,
shuffle)`, the C# side waiting up to 5 s (`SPOTIFY_PLAYURI_TIMEOUT`). From
0.55 the watch answers `LaunchSpCore` (we are the core) and `PlaySpUri`:
our player is sent `index=<trackIndex>` (a new command: that track of its
queue, playing) and `shuffle=1` if asked, and the shell is given track info
changed, a Spotify message (4, param 200, `MESSAGE_LOGIN_SUCCESS`) and play
status changed.

### Choosing a track: CE-41495-3 (0.55)

Choosing a track or Play All showed "An error has occurred. (CE-41495-3)",
with no `PlaySpUri` or `LaunchSpCore` reached. The press read the
playlist's `type` and `uri` and the session link's `id` (missing) and
`token`, and its task (`TrackListMenuPanel/<>c__DisplayClass21_0:<OnItemSelected>b__0`)
asks `MusicCoreServerController.PlaySpotifyUri(info)` and shows its error
when not 0. 0.56: `id` in `sp_sessionlink.json` (the login's user name, it
seems; `token` its OAuth), `PlaySpotifyUri`'s result logged, and the music
server's controller classes dumped too (`code/methods3.map`, `code3.bin`).

### It plays (0.56), and the playlist's picture (0.57)

With `id` in the session link, choosing a track or Play All worked on a PS4:
`PlaySpotifyUri` returned 0, `PlaySpUri` came with the track's index (0, 1,
0), and our player played it; the player panel showed its cover. The
playlist's tile showed no picture: it loads `image_url` +
`string.Format("&w={0}&h={0}", 192f)` with `ImageBox.LoadAsync` (0.51's dump,
`PlayListPanelItem.UpdatePlayList`; 192 read from the float in its code).
With a `file://` address and no `?` that is a file's name: from 0.57
`image_url` is `file:///user/data/ShellCompanion/ps4/ebgm/playlist` and the
picture is also written as `ebgm/playlist&w=192&h=192`.

### The tile's picture, five ways (0.58)

0.57's `file://…/ebgm/playlist` with `playlist&w=192&h=192` (a PNG) stayed
blank. The tile's `ImageOptions` set `ImageFormat` 4, `Dxt5`: the texture it
decodes into, not the file's type. 0.58 packs a 192-square JPEG
(`ps4/app/playlist.jpg`, the icon) and gives five playlists, each its
picture a way of its own: A `file://…/a.jpg?x=1` (the loader dropping the
query), B `file://…/b.jpg#` (the fragment), C `file://…/c` (the name
`c&w=192&h=192`), D `file://…/d.jpg` (the name `d.jpg&w=192&h=192`), E
`/user/data/…/e.jpg?` (a path, no scheme).

### The tile's picture: a JPEG by the literal name (0.58, 0.59)

Of 0.58's five, only C (`file://…/c`, the file `c&w=192&h=192`) and D
(`file://…/d.jpg`, the file `d.jpg&w=192&h=192`) were shown: the tile loads
`image_url` + `&w=192&h=192` as the file's own name, no query or fragment
dropped and no bare path taken; and it must be a JPEG (0.57's PNG by that
name was not shown). 0.59 has one playlist again, `image_url`
`file:///user/data/ShellCompanion/ps4/ebgm/playlist.jpg`, its picture written
as `ebgm/playlist.jpg&w=192&h=192`.

### The Music section from the real queue (0.60)

Our player now writes the Music section's library each time it reads its
queue (`writeLibrary` in `ps4/bgm/bgm.c`), from each track's ID3 tags:

- `recommended_views.json`: a "Shell Companion" section with **Queue** (the
  whole queue, the first album cover as its picture), and **Albums**, one
  playlist an album (up to 64), its cover as its picture
- `tracks_queue.json`, `tracks_album<n>.json`: their tracks (title, or the
  file's name; artist; album), JSON-escaped
- `album<n>.jpg&w=192&h=192`: each album's cover, from the first of its
  tracks with an APIC picture that is a JPEG (the tile shows no other)

The watch picks a playlist's tracks file by its uri's last part
(`…%3Aplaylist%3Aalbum3` → `tracks_album3.json`), and turns `PlaySpUri` into
`pick=<id>:<n>`, which our player maps to its queue (the n-th track of that
album, or of the queue) and plays. The test app no longer writes these, only
the profile and the session link. A track's row has no picture: the shell
reads only `name`, `explicit`, `artists[].name` and `is_playable` for it.

### R3, "Add to Your Music" (0.61), and pictures on tracks

The player panel's R3 "+" is Spotify's "Add to Your Music" (`ControlPanel`,
`AddMyLibraryFunction`): enabled once the shell has the playing track's
Spotify id (`OnSpotifyTrackIdChanged`, from `GetSpTrackInfo`), it asks
`/v1/ebgm/users/me/library/contains/<id>` (`contains_in_library`) and on R3
`…/save/<id>` or `…/delete/<id>`. From 0.61 the watch answers
`GetSpTrackInfo` (`trackId[128]` `sc<n>`, `trackUri[128]`, `contextUri[128]`,
`canShare`, `canEditPlaylist`), gives event 6 (Spotify track info changed)
when the track changes, and answers the library requests itself: the playing
track's file is put in, or taken out of, `/data/ShellCompanion/ps4/liked.txt`
(one path a line), for an app to read.

A track's row (`TrackListPanelItem`) is two labels, its title and its
artist, with no picture in it; tiles (playlists) and the player panel are
where pictures are shown.

### The shell's own declarations (0.32's copy from memory, decompiled)

DOWN copied 135 .NET assemblies out of the shell's memory; `app` (the
shell's app.exe, 9 MB) and `platform` decompile with ILSpy. They are
precompiled (full AOT), so method bodies are empty, but every type, field,
enum and native import is there:

- `Sce.Vsh.ShellUI.MusicCoreServer.MusicCoreServerWrapper` (platform): each
  `sceMusicCoreServer*` import with its arguments, and the native
  structures: `SceMusicCorePlayStatusInfo` (0x70), `SceMusicCoreTrackInfo`
  (srcUri[1048], trackName, albumName, artistName [341 each], a byte,
  coverartUri[1048], coverartOffset, coverartSize), `SceMusicCoreCoreInfo`
  (scheme[32]: `bgmplayer`, `mediaplayer`, `mediaplayer2`, `customcore`;
  titleId[10]; content type; six capabilities), `SceMusicCoreIpmiMessageData`
  (type, param: 1 core ready, 2 play status changed, 3 track info changed,
  4 Spotify message, 5 core killed, 6 Spotify track info changed).
- `PlayControl(trackIndex, cmd, startTime, playSpeed)`: track 0xFFFFFFFE the
  current one, 0xFFFFFFFD the previous, 0xFFFFFFFC the next; command 0 start
  by time, 1 start at the current position, 2 jump, 3 stop.
- The quick menu's entries (`MainMenuType`): **MusicApp**, the current music
  core's entry (`MusicPanel`: cover, title, album, artist, time, play/pause,
  previous/next, shuffle, repeat, volume), which is where the USB Music
  Player shows; **PSMusic**, the Music section, Spotify's
  `PlayStationMusicPanel` (playlists, sign-in, and a `MusicPlayerPanel` for
  what plays); SpotifyApp.

### Our player as a music core (0.33, an experiment)

With `mcw-fake.txt` there and our player playing, the watch answers as a
core: GetEvent gives core ready (1), track info changed (3) and play status
changed (2) as they are owed; GetCoreInfo `customcore` with the file's
title; GetPlayStatus our state, length, position, track and count;
GetTrackInfo the file, its ID3 title, artist and album, and its cover (the
APIC picture's offset and size in the MP3, which `MusicPanel` loads); play,
pause, next and previous go to our player. When it stops, core killed (5).
Our player's `status.txt` now has the tags (`title`, `artist`, `album`,
`art_off`, `art_len`).

### Answering the quick menu for our player (0.31, an experiment)

With `mcw-fake.txt` there (the test app's R3 makes and removes it), while our
player has a track, the watch changes three answers after the real call, as
the USB Music Player's were:

- **GetPlayStatus**: state (2 playing, 3 paused), length and position in
  seconds (our player's `status.txt` now has `dur_ms`), core id -2, and the
  file's first word as the title (`SCMP00001`, the test app, by default;
  `NPXS20117` would claim to be the USB Music Player).
- **GetEvent**: event 2 when that changes, and once a second, if the real
  call had none.
- **PlayControl**: 1 play and 3 pause, sent to our player's `cmd`; anything
  else is logged, not sent.

The questions: does the test app's quick menu entry show music controls?
Does the Music section change? The log has a `fake:` line for each change
and each button.

### The USB Music Player is NPXS20117

`/system_ex/app/NPXS20117/sce_sys/param.sfo` says TITLE "USB Music Player",
APP_VER 01.07: the title GetPlayStatus names. On that PS4 its controls show
in the quick menu under its own name, among the running apps (as "Shell
Companion B..." does), not in the Music section; with no core playing, the
Music section shows only Spotify's "Sign in to PlayStation Network".

### The shell's own code (test app, DOWN)

The quick menu is C#. On disk its assemblies are signed executables
(`*.dll.sprx`, all starting `4F 15 3D 1D`, encrypted), so 0.29's copies were
of no use. From 0.30, DOWN (with the watch loaded, UP) has the watch search
the shell's memory for them instead: every CPU-readable, committed region
that is not direct (GPU) memory, 16 bytes at a time, for "MZ" with a .NET
PE header after it. Each is written to `/data/ShellCompanion/ps4/shell/asm-<address>.dll`,
plain, for a decompiler, and `mcw.log` lists the regions and what was found.
The region being searched is kept in `mcw.carving`: if the shell goes down
reading one, the next search adds it to `mcw-carve-skip.txt` and never reads it
again.

### Pictures on track rows (0.62, an experiment)

A track row in the Music tab (`TrackListPanelItem`) is two labels and has no
picture. The watch hooks its `UpdateData(menuData)`. After that runs on the
shell's UI thread, the watch uses Mono to give the row its own `ImageBox`
(Sce.PlayStation.PUI.UI2): 64 px square at (10, 10), loaded with
`file:///data/ShellCompanion/ps4/ebgm/trackart<n>.jpg`. It moves both labels
74 px right, to their own X plus 74, set absolutely. `n` comes from the
track's `uri` (`spotify:track:sc<n>`). The player writes one JPEG per queue
track that has an embedded JPEG cover. Rows with no cover, and placeholder or
"play all" rows, have the picture hidden and the labels put back. The list
reuses rows as it scrolls, so each row is tracked by a weak GC handle. Look
for `rowart:` lines in mcw.log. `mcw-norowart` in the player's folder turns
it off.

0.62 loaded the picture with no options and showed nothing, though its labels
moved. 0.63 loads it the way the shell's own cover box does
(`MusicPlayerPanel.CoverArtLoadAsync`), with `ImageOptions`: Format Dxt5,
ConvertOption DXTCompressByGPU, Resize 64×64, and the box's ImageScaleType
set to AspectInside. Each `rowart:` line now also says whether the box's
`Image` has loaded.

0.63 still showed nothing. Each list opening makes new rows, so no row was
logged twice and the result of a load was never seen. 0.64 hooks
`ImageBox.HandleLoadCompleted`, where every image box's load ends. It logs
each of ours (`rowart: OUR box … loaded: ok|FAILED`, with the shell's own
error text) and the shell's own first 12, for comparison.

0.64 showed that the shell's own loads succeed, but every one of ours failed,
with an error text 1164 characters long. 0.65 logs that text in full
(`rowart:   error …`) and gives our loads the shell's global image cache
(`ImageObject.GlobalCache`, through `ImageOptions.set_Cache`). Our loads had
no cache; the shell's always have one.

0.65's error was `System.OutOfMemoryException: AssetManager(SceUIDefaultAssetManager)
is out of memory`, raised in `AssetManager.Register`. The picture was read
and decoded, but the default asset manager has no room. 0.66 hooks the
shell's own `ImageBox.LoadAsync(uri, action, options)`, keeps the
`AssetManager` of the latest of those (with a strong GC handle), and gives it
to our loads' options (`ImageOptions.set_AssetManager`). It logs each of the
shell's first 12 loads with its address and manager's name.

0.66 works: the track row shows its cover at its left, the title and artist
beside it (photo, 2026-11-26).

### Editing the queue from the Music section (0.67, an experiment)

The player now keeps a **library** and a **queue**. The library is every
track loaded (from queue.txt or music/), and a track's number in it is its
id (`sc<n>`) in the Music section. The queue is a list of library numbers in
play order. The Music section's front page lists Queue, All songs and Liked
(from liked.txt, R3 "+"), then the albums. Picking a track in any list but
Queue makes that list the queue, starting from the track picked (Play All
starts from its first track). Picking in Queue only jumps to the track.

Buttons on a track list's focused track:

| List | □ | △ |
|---|---|---|
| Queue | pick the track up: ↑/↓ move it, X or □ puts it down, ○ cancels | take it out of the queue |
| any other | play it next (after the track playing) | add it to the end of the queue |

The watch hooks the Music panel's global key handler
(`ControlPanel.OnGlobalKeyEventReceived`) and acts before it, then marks the
key handled. That way □ doesn't also open the volume and △ doesn't also
pause. The pick-up mode is the list control's own
(`ListPanelBase.BeginItemMoveMode`, the one the home screen uses to move
tiles). The watch notes the row where the track was picked up, follows each step
(`ListPanelBase.OnItemMoved(from, to)`, where `to` is the held track's row),
and when the move mode ends (`EndItemMoveMode`) sends one
`move=<from>:<to>` command, unless ○ put it back (`RollbackMoveTarget`).
0.67 sent every step, and 0.68 sent every `DecideMoveItem`. The list calls
both on each step, and after the first step they read (-1, row), because the
held track is out of the list while it moves. When the move mode ends (`EndItemMoveMode`), or after a removal,
the list asks for the queue again (`TrackListPanel.StartQuery`). The watch
holds that request until the player's `queue_rev` (status.txt) shows the
edits made, for up to 1.5 s.

The player's new commands:
- `move=<from>:<to>` and `remove=<place>`: places in the queue
- `next=<track>` and `add=<track>`: library numbers
- `liked`: write the Liked list again

`next`, `add` and `remove` each show a notification. `mcw-noqueuekeys` in
the player's folder turns the buttons off (they need the row pictures'
setup, so `mcw-norowart` turns them off too).

### Covers by name, and PNGs (0.71)

Since 0.75 each cover a row shows gets an ImageBox of its own, and the row's
earlier box is taken off it (`Widget.RemoveFromParent`). The list reuses rows
as it scrolls, and a load still coming into the earlier box put an earlier
track's cover on the row: the Power Ballad PNG on the Agatha song.


The shell keeps pictures by their address. When the library changed, 0.70
showed `trackart0.jpg`'s old picture on the new first track. The player now
names covers with a number that changes each time the library is written
(`album<a>-<rev>.jpg`, `trackart<n>-<rev>.jpg`, `art_rev` in status.txt) and
deletes the older ones. PNG covers are written too (`.png`), for rows and
tiles. An album's tile uses a JPEG if any of its tracks has one. bgm.log
lists each track's tags and cover kind (`library <n>: …; cover: JPEG|PNG|none`).

A queue move is sent when the track is put down. `OnItemMoved(from, to)`
with both rows real becomes `move=`; `(-1, row)` doesn't, because nothing
was held. In the list's move mode, X picks a track up and X puts it down;
more tracks can be moved, and ○ leaves the mode. Leaving calls
`RollbackMoveTarget` even when nothing is held, so 0.69 wrongly took every
move as put back. □ puts down the held track, or leaves the mode if none is
held.

### Streaming (0.72, an experiment)

A track can be a URL. The player fetches it with the shell's own HTTP library
(`libSceHttp`, through `libSceNet` and `libSceSsl`, loaded by name), into
memory on a thread of its own (`ScBgmFetch`). It starts playing once the ID3
tag and 192 KB of audio past it have come. If playback catches up with the
download, it plays silence (`stream: buffering` in bgm.log) until 64 KB more
has come. Nothing of the track is written to a file. The download's length
comes from Content-Length; without one, the first 16 MB are taken. MP3 and
16-bit WAV are played (WAV by its `RIFF` header).

Covers stream too: a cover URL is handed to the shell, which fetches it
itself. It goes in a tile's `image_url` (with a `?` added to a URL that has
no query, since the shell appends `&w=192&h=192`), in a row's ImageBox (the
URL in `ebgm/trackart<n>-<rev>.url`), and as the now-playing cover
(`coverartUri`, without `file://`).

A queue.txt line for a stream:

```
URL<tab>title<tab>artist<tab>album<tab>cover URL
```

Since 0.80, `|` also separates the fields of a line that starts with a URL,
with spaces around them allowed:
`https://…/song.mp3 | Title | Artist | Album | https://…/cover.jpg`.

0.72–0.74 failed on 11.50 with `http: … libSceSsl 0 …: functions missing`:
the shell's TLS library there is `libSceSsl2.sprx`. 0.75 tries
`libSceSsl.sprx`, then `libSceSsl2.sprx`, and looks for any function still
missing in every module the shell has loaded (`anySym`). It logs each
function it can't find.

Only the URL is needed. Put `streams.txt` in the player's folder
(`/data/ShellCompanion/ps4/`), in `/data/ShellCompanion/`, or on a USB
drive's root. The test app copies it into the player's folder and adds its
lines to the end of the queue it writes. The player also reads it there
whenever it loads the queue (0.74), and skips URLs already in the queue. A
UTF-8 byte-order mark and spaces before a URL are ignored. Logs show URLs up to
their `?`, so a token in the query isn't logged. HTTPS checks the server's
certificate unless `https-noverify` is in the player's folder.

### Streaming works (0.75), and tidier names (0.76)

0.75 played `https://www.soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3`
on 11.50. The server answered HTTP 200 with a length of 8,945,229 bytes in
577 ms. Playback began at 262,144 bytes, 1.2 s after the track was picked,
and the whole file had come in 9.6 s.

A title taken from a file's name (no TIT2 tag) loses a YouTube video id at
its end, as download tools add one: `Celebration-QZWRu3kqkG8` and
`Celebration [QZWRu3kqkG8]` both read `Celebration`. Since 0.76 a row's box
is replaced for any new cover once a load has been begun into it. 0.75 kept
the box when the row had held a track with no cover in between, and that
load could still land.

### A pick plays the row's own track (0.77)

PlaySpUri gives only the list and the row's place in it. The list the shell
shows can be older than the player's: the Queue it had loaded before a pick
in an album replaced the queue, or before an edit. Then the row's place
holds another track, and a different song played. The watch now hooks
`TrackListMenuPanel.OnItemSelected(menuData)` (both overloads, before) and
keeps the chosen row's own track (`sc<n>` from its uri). PlaySpUri then
sends `pick=<list>:<place>:<track>`. The player plays that track: at that
place if it is there, else where it is in the list (the nearest), else, for
the queue, put after the one playing. bgm.log says when the place held
another track.

### Track lists answered a page at a time (0.78)

The shell asks for a list's tracks a page at a time
(`playlist/tracks?...&offset=0&limit=15`). Up to 0.77 the watch answered every
request with the whole file. With 17 tracks the shell kept 15, and its rows
past them showed the list's first tracks again over their real places. A row
labelled "Celebration" (track 2) was really place 15, and played the test
tone. The watch now reads the whole list (up to 1 MB) and answers with only
the tracks asked for (`offset`, `limit`), the `total` counting all of them,
so the shell asks for the next page as the list scrolls. This also lifts the
64 KB limit an answer had, which a long list would have passed.

### Rows' titles: a track's `id` (0.79)

With the pages right (0.78), rows reused while scrolling still kept their old
titles: the stream's row read "Dublin to L.A.", and track 13's row read the
first track's. Before updating a row, the shell compares the old track and
the new one by `id` (`TryGetValue("id")` on both, `TrackListMenuData.IsSameData`).
Our tracks had none, so every pair matched and the title was not redrawn.
The pictures and the track played were right, since they come from our
hooks and the row's `uri`. Each track now has `"id":"sc<n>"`.

### Client library (`ps4/client/`, 0.81)

What an app links to hand its music to the shell, as `vita/client` does on
the Vita. It's plain C with libc only, Apache-2.0, in
`shell_companion.h`/`.c`. The app carries `sc_bgm.prx` and `sc_mcw.prx` at
its package's root.

```c
scInit("ABCD00001", "My App");
char what[256];
scOpen(what, sizeof(what));           // the player and the watch into SceShellUI
ScTrack t[] = {{.url = "https://…/a.mp3", .title = "A", .artist = "Me", .album = "X",
                .coverUrl = "https://…/a.jpg"},
               {.local = "/data/MyApp/b.mp3"}};
ScQueue q = {.index = 0, .positionMs = 12000};
scWriteQueue(&q, t, 2);
scMusicPlay();                        // as the app goes
…
scMusicTakeBack();                    // as it comes back: stopped, the place kept
ScQueueStatus st;
scReadQueueStatus(&st);               // st.index, st.track, st.positionMs, …
```

- **`scOpen`** jailbreaks the app (GoldHEN) if `/data` can't be written. It
  writes the Music section's account and session answers (`ebgm/`), then
  loads each module that isn't running: the player if `status.txt`'s
  `beat_ms` is over 3 s old, the watch if `mcw-beat.txt`'s is (the watch
  writes it every second). A second copy of a module would hook the shell
  twice, and a module loads only once a boot. Each module is copied from
  the package to `/data/GoldHEN/plugins/` (0777) and loaded with
  `PROCESS_PRX_LOAD`. The copy already there is used if the package has
  none.
- **`scWriteQueue`** writes `queue.txt`: `#start`, `#position`, then a line
  a track, `path or URL<tab>title<tab>artist<tab>album<tab>cover URL`.
  `/data/` paths become `/user/data/`, as the shell sees them. An app's
  title, artist and album override a local file's tags, and its cover URL
  is used where the file has no cover.
- **`scMusicPlay`** sends `reload` and `play`. **`scMusicTakeBack`** sends
  `takeback`: the player stops and keeps its place, so `status.txt` then
  reads `state=stopped` with `pos_ms` where it was. **`scCommand`** sends
  any of the player's commands.
- The player begins `#position` ms into the `#start` track, by decoding up
  to it.

The test app (0.81) loads both modules with `scOpen`, on X and on UP.

### The experiment's answer: packages only

0.94's run, served from the PS4 itself: the server answered (the probe's own
HEAD request was logged: 200, 6,830,270 bytes), and the download service took
the task (1061) and started it. Two seconds later it failed with `0x80990033`
(CE-32942-0 on screen, "not on the server") **without a single request to the
server**. The service doesn't fetch content that isn't a package: it gives up
before asking for it. Plain media stays with our own downloader
(`downloads.c`) and its notifications. Packages (`pkg` items, self-update) go
to the service.

### A web server on the PS4 for the experiment (0.94)

With no URL in `bgft-probe.txt`, the probe serves one of the console's MP3s
(`music/yoursong.mp3`, else the queue's first local file) from a small HTTP
server in the player module, `http://127.0.0.1:38521/probe.mp3` (libSceNet
sockets: GET, HEAD, Range, then closed), and gives that URL to the download
service. Every request the service makes is logged in full
(`probe server <` and `probe server >` in bgm.log), so what it asks for is
seen without a PC. turbodl feeds the service from a loopback server the same
way. Each run's content id is new (`IV0000-SCMP00001_00-PROBE<11 digits>`).

### Media through the download service: listed, not fetched (0.93)

0.92's run: the MP3 was taken (task 1059) and shown in Notifications >
Downloads as "Shell Companion test". It failed at once, without a byte
(progress error `0x80990033`, 0 of 0), and the PS4 said CE-32942-0: "Cannot
continue downloading the content. It is not on the server." The same URL
streams in the player. The service keeps what it downloads in
`/user/download/NPXS20113/download0.dat` (2 GB), not where an app chooses.
0.93 registers it with its size (a HEAD request's Content-Length), as
package installers give a package's. The probe's URL can be a PC's HTTP
server (`bgft-probe.txt`), whose log shows what the PS4 asks for.

### Media through the download service: taken (0.92)

With a user (0.91), the service took the MP3 at once: package type `""`,
entitlement type 5, `0x00000000`, task 1056, and `sceBgftServiceDownloadStartTask`
`0x00000000`. The shell then went down. The experiment's next call was
`sceBgftServiceIntDownloadGetTaskInfo`, given null pointers for the title and
icon: it writes into buffers it is given. It is not asked now. An earlier
run's task (`0x80990086`, taken already) is found and followed.

### The user the download service wants (0.91)

The first run of the experiment had every registration refused with
`0x80990004`. That shows only that the user was wrong: inside the shell
`sceUserServiceGetInitialUser` gave -1 (`sceBgftServiceIntInit` itself
answered `0x80990001`, the service being up already). The user is now the
foreground user, else the initial one, else the first of
`sceUserServiceGetLoginUserIdList`, else the app's: the client library's
`scOpen` writes the app's initial user to `user.txt`. It is asked again
while it is unknown. The same would have stopped `.pkg` items and
self-update.

The player also falls back to audio port type 0 when the type in
`port.txt` won't open. The test app's LEFT/RIGHT change that file, and only
type 0 works in the shell.

### An experiment: media through the download service (0.90)

The service has task subtypes for photos, music and video besides packages.
No documentation or homebrew shows a plain MP3 or MP4 registered with it.
`bgft_probe` (the test app's touch pad button) registers one by URL
(`bgft-probe.txt`'s first line, else SoundHelix's test MP3) with package
types `""`, `PS4MUSIC`, `MUSIC`, `PS4AUDIO`, `AUDIO`, `PS4VIDEO`, `VIDEO`,
`audio/mpeg`, then `NULL`, each with entitlement types 5 and 0, logging
each answer (`probe:` in bgm.log). For the first one taken, it starts it,
logs its task info and progress every 2 s for up to 3 minutes, then lists
the folders the file might have gone to.

### Packages and self-update (0.89)

`ps4/bgm/pkg.c`. A download list's `pkg` item, and an app's own update
(`update.txt` and `update_start`, the client library's `scUpdateInstallPkg`),
go to the PS4's own download service (BGFT), as GoldHEN's Package Installer,
flatz's remote package installer and cy33hc's ps4-webdav-client do:

1. `sceBgftServiceIntInit` with a 1 MB heap (the shell has the service up
   already, so its answer is only logged).
2. `sceBgftServiceIntDownloadRegisterTask` for a URL (option
   `DISABLE_CDN_QUERY_PARAM`), or `…RegisterTaskByStorageEx` for a file
   (option `FORCE_UPDATE`, slot 0), with `entitlementType` 5, the
   package's content id, type and size, `playgoScenarioId` "0", and the
   initial user.
3. `sceBgftServiceDownloadStartTask`.

0x80990086 (being downloaded already) takes that task
(`sceBgftServiceDownloadFindTaskByContentId`). `sceBgftServiceDownloadGetProgress`
is followed every 2 s until the bytes are all there or it reports an error.

A package's header, the first 4 KB, read from the file or by HTTP `Range`,
is big-endian: `\x7FCNT` at 0, the content id at 0x40, the content type at
0x74 (0x1A GD, 0x1B AC, 0x1C AL, 0x1E DP), the size at 0x430.

OpenOrbis's `Bgft.h` has the package size and the progress's lengths as 32
bits, while the shell's own wrapper has them as `ulong`. They're 64 bits
here, and the progress structure has room to spare.

Self-update waits 3 s for the app to have gone, installs, waits 15 s more
for the install, writes `update-result.txt` (`result=`, `why=`), and with
`relaunch=1` starts the app with `sceSystemServiceLaunchApp`. In the test
app, L3 installs `test-update.txt`'s package and closes the app, and the
next start logs the result. None of this has been run on the console yet.

### Set up again when loaded early (0.88)

When the shell started again (after a crash), the watch was loaded while
only 63 of its ~220 modules were in. The UI library
(`Sce.PlayStation.PUI`) wasn't there yet, so the rows' covers and the
queue's keys stayed off for the session (`rowart: … NOT ready`,
`queue: keys NOT ready`); 0.15 s later the library was in, and the other
hooks went in. The watch now tries those two setups again every 2 s, up
to a minute, until they are ready.

0.87, with the download service's hooks off, didn't crash with a download
going.

### "+" as each app wants it (0.86)

The Music section's "+" (R3, Spotify's "Add to Your Music") is the shell's,
and its label can't change. What it does is now the app's, through
`ScQueue.plus` (`ps4/bgm/plus.c`):

- **Always:** the track is added to or taken from Liked (`liked.txt`), which
  lights the "+". Every press is added to `plus-events.txt`
  (`<ms>\t<save|delete>\t<the app's id>\t<path or URL>`), which
  `scReadPlusEvents` reads and empties when the app comes back.
- **`SC_PLUS_HTTP`:** the app's server is told **at once**, while the app is
  away. `saveUrl` and `deleteUrl` are URL templates, sent with `method` (GET
  by default; `sceHttpCreateRequestWithURL2` for any method, PATCH
  included), `body`, `type` and `headers`. Placeholders: `{id}` (the track's
  `ScTrack.id`, a sixth field on its queue.txt line), `{title}`, `{artist}`,
  `{album}` and `{file}`, URL-encoded in a URL and escaped as in a JSON
  string in a body. A failed request shows a notification.
- **`containsUrl`** (optional) is asked as each track begins: a 2xx answer,
  whose body holds `containsMatch` if one is given, marks the track saved in
  `liked.txt`, and any other answer not. That's what the "+" shows.
- **The requests** run one at a time on their own thread (`ScBgmPlus`). In
  queue.txt the settings are `#plus.<key> <value>` lines, %-encoded. The
  watch tells the player of a press with `plus_save=<track>` or
  `plus_delete=<track>`.

The player's command buffer is now 4 KB (256 bytes before). Commands have
been added to `cmd` rather than replacing it since 0.67, so a burst could
have overflowed it.

### Download progress (0.85)

- **Notifications:** at each quarter of the whole list (25, 50, 75%), at
  most one every 20 s, then at the end. The progress counts bytes when every
  item's size is known, else items (the one being fetched by its own bytes).
  The PS4's notifications are pop-ups, not the Vita's single one that
  updates.
- **Notifications > Downloads, with a progress bar (to come):** the list is
  `Sce.Vsh.Orbis.BgftAccessor`'s `DownloadItemAccessor2`. Its tasks come
  from `BgftWrapper.DownloadQueryTask` (`QueryTaskResult {int taskId; uint
  param}`, `param` a bit field of type, subtype, state, …), its details from
  `DownloadGetTaskInfo` (`contentTitle`, `iconPath`, a tick) and
  `GetTaskInfoValue*`, and its progress from `DownloadGetProgress` (a
  `TaskProgress`: state, runstate, paused, length, transferred, totals,
  rest seconds, `percentTotal`). A task of ours there means adding it to
  `DownloadQueryTask`'s answer and answering the others for its id. 0.85
  only logs those calls (`bgft:` lines in mcw.log), to see a real task's
  values first. With a download going, opening Notifications > Downloads
  under those hooks crashed the shell. Since 0.87 they are off unless
  `mcw-bgftlog` (all but `DownloadGetProgress`) or `mcw-bgftprogress` (that
  one too, the suspect: it aligns its stack to 32 bytes for AVX) is in the
  player's folder.

### Large queues (0.84)

Up to 10,000 tracks in up to 1,000 albums (512 and 64 before).

- **Memory:** paths, URLs and tags are kept in a pool of 1 MB chunks, mapped
  as needed and freed when the queue is loaded again. The tables hold
  pointers. The fixed arrays of before would have taken ~15 MB in the shell
  at 4,000 tracks.
- **Files read whole into memory mapped for their size:** `queue.txt`,
  `streams.txt`, `liked.txt` (fixed buffers before: 48 KB, 64 KB, 64 KB).
- **Lookups by hash:** a path to its track (streams.txt's duplicates, Liked)
  and an album's name to its album, instead of one at a time.
- **Tag cache, `tags.txt`:**
  `path<tab>size<tab>title<tab>artist<tab>album<tab>cover file`, a line per
  local file. It holds the file's own tags (not an app's), and is rewritten
  with the library's files each time. A file is read again only when its
  size changed or its cover file is gone.
- **Covers by content, `ebgm/art/<hash>-<length>.jpg|png`:** written once
  however many tracks carry it, with a `&w=192&h=192` copy for tiles. A
  different picture always has a different name, so the shell's picture
  cache can't show an old one. The per-track and per-album files of
  0.71–0.83 are deleted.
- **Row covers, `ebgm/rowart-<rev>.txt`:** a line per track (a `file://`
  URL in `ebgm/art/`, a cover URL, or empty). The watch reads it whole once
  for each library (`art_rev` in status.txt), instead of a file per track.
- **Sizes:** the player's list files are sized for the library
  (1 MB + 900 bytes a track). In the watch, an answer can be 256 KB (64 KB
  before) and a track list read whole 4 MB (1 MB). The first 40 library
  lines are logged.

On Linux, 4,000 generated MP3s in 300 albums, carrying 5 distinct covers:
the first load read every tag in 0.2 s, the second took them from the cache
in 12 ms. The covers made 10 files in `art/`. `tracks_all.json` was 670 KB
and the front page 58 KB. The client library takes up to 10,000 tracks, its
buffer sized to them.

### Rows' covers after several lists (0.83)

The watch keeps an entry for each row it gave a cover. An entry was freed
only when the shell discarded the row, but the shell keeps the rows of lists
opened before. After a few lists the 64 entries were all taken, and every new
row got no cover: "Unknown album"'s rows went blank while its tile showed.
There are now 256 entries, and when they are all taken the one updated
longest ago is reused: its row's box is removed and its labels put back.

Background downloads worked on the first test: three SoundHelix MP3s
(8,945,229, 10,222,911 and 8,258,104 bytes), all `done`.

### Background downloads (0.82)

An app's files, fetched in the shell after the app has gone, as the Vita
plugin's (docs/PROTOCOL.md §5), by a thread of the player module
(`ScBgmDl`, `ps4/bgm/downloads.c`) with the same HTTP library as streaming.

- **Handing over:** `scWriteDownloads(notify, headers, items, count)` writes
  `downloads.txt`, then `scDownloadsStart()` sends `dl_start`. Each item has
  an id, a file (`/data/...`), a size (0 if unknown), a title, a kind (`url`
  or `pages`) and that kind's arguments. A field that holds a tab, a line
  break or a `%` is %-encoded.
- **Fetching:** in order, 2 s after the hand-over. A partial file is
  continued with `Range`. A 200 to a Range rewrites the file, 416 counts as
  complete, 503 is waited for every 10 s for up to an hour, 404 and 410 mark
  the item `gone`, and anything else is tried again after 5, 15 and 60 s
  (any bytes received reset the count). The list's headers and the item's
  own go with every request.
- **`pages`:** a URL and a file template with `{page}` or `{page:N}` (zero
  padded to N digits), and `count` or `count,first`. Each page goes to
  `<file>.part` and is moved in once whole; a page already there is
  skipped.
- **Notifications:** at the start, as each item of several ends, and at the
  end (with failures counted). The PS4 has no progress notification like
  the Vita's.
- **Status:** `downloads-status.txt`, every 2 s and at each change:
  `running=`, `paused=0`, then
  `item=<id>\t<going|waiting|done|gone|failed>\t<bytes>\t<size>\t\t[\t<done>/<pages>]`.
  `scReadDownloadsStatus` reads it.
- **Taking back:** `scDownloadsTakeBack()` sends `dl_takeback`. The thread
  stops after the piece it is writing (64 KB), and the call waits up to 5 s
  for `running=0`.

Not yet done: Plex's `plexqueue`/`plexadd`, and finding out whether
downloads go on in rest mode. In the test app, L2 hands over three
SoundHelix songs (or the URLs in `test-downloads.txt`) to
`/data/ShellCompanion/ps4/downloads/`, and R2 takes them back and logs
their status.

### How the hooks work

A hooked function starts with a 5-byte `jmp` to a small entry that our
module maps within 2 GB of it. The instructions that `jmp` replaced are
moved to a stub, with their relative addresses recomputed. A thunk keeps
every argument register (rdi to r9, rax, xmm0 to xmm7) and copies 8 stack
arguments. It calls the stub, which is the function, and returns rax, rdx
and xmm0 as the function left them. Memory for the stubs comes from the
first of these that works: a read/write/execute mapping, as GoldHEN's plugin
SDK maps its stubs; a read/write one that is then made read/execute; a
reserved part of the module's own code. The function's code is written after
`sceKernelMprotect`, or else through GoldHEN's `PROCESS_RW`. The log says
which was used and lists each function's first 16 bytes. A function whose
first bytes can't be moved is left alone, and the log says why.

The hook engine and the logger were tested on x86-64 Linux, which uses the
same calling convention, with shims for the libkernel calls. The tests
covered rip-relative loads, short and long jumps, calls, 9 integer and 2
float arguments, a float return, a returned struct, and a callback. The
console has not run it yet.

## The probes

The PS4 counterpart of the Vita plugin is being worked out. On the Vita, the
plugin plays an app's queue through SceShell's own music service, which
gives it the quick menu's Music row. The PS4 has nothing as direct, so two
probes come first, each an ELF payload for GoldHEN that answers one
question and logs what it finds.

| Probe | Question |
|---|---|
| `sc_ps4probe_audio` | Does a background process's audio keep playing while a game runs, on the main port and on the background-music (BGM) port? |
| `sc_ps4probe_music` | Will the system's own music service (`libSceMusicPlayerService`) play a track handed to it from homebrew, and does it then show in the quick menu with working buttons? |
| `sc_ps4probe_shell` | Is audio played from inside the shell (`SceShellUI`) heard, and which modules does the shell have, Mono's among them? |

Both use no user service, as GoldHEN before 2.4b18.10 hangs it in ELF
payloads, and neither needs anything resolved when it loads: system
libraries are loaded at run time (`probe/sys.c`).

## Building

```sh
docker run --rm --user 0 -v "$PWD":/src -w /src openorbisofficial/toolchain:latest sh ps4/probe/build.sh
```

The output is two forms of each probe in `build-ps4/`:

- **`.bin`, a raw payload:** the image as it runs, from its first byte, for
  GoldHEN's BinLoader. The loader jumps to the start of what it is sent.
- **`.elf`:** the same probe as an ELF, for an ELF loader.

The CI builds both. The first test sent the ELF to the BinLoader on 9020,
and the probe never ran: no log, and the port closed after it.

## Running them

Send each to GoldHEN's BinLoader, on port **9020** on GoldHEN 2.4b18.6
(check with `nmap -p 9020,9090 <ps4-ip>`; GoldHEN's FTP is on 2121), for
example `nc -v <ps4-ip> 9020 < sc_ps4probe_audio.bin`.

### 1. Audio probe

1. Send `sc_ps4probe_audio.bin` from the home screen. A notification says it
   has started.
2. Within the first 30 s, start a game and stay in it.
3. Listen for 4 minutes. Every 30 s the tone switches: a **440 Hz** tone is the
   main port, a **660 Hz** one the BGM port. Each second ends with a short gap.
4. Note when you heard each tone, and whether it was quieter in the game.
5. Send `/data/ShellCompanion/ps4probe-audio.log` (FTP).

### 2. Music probe

1. Optionally, copy an MP3 to `/data/ShellCompanion/test.mp3`. The probe
   also makes `probe-tone.wav`, 30 s of 440 Hz beeps.
2. Send `sc_ps4probe_music.bin` from the home screen.
3. If a notification says it is playing, open the quick menu (hold PS):
   - Is there a Music section, with the track?
   - Try pause, play, next and previous. Next and previous move between two
     copies of the same track.
4. It watches for 2 minutes, then stops.
5. Send `/data/ShellCompanion/ps4probe-music.log`.

### 3. Shell probe

What the first two found on GoldHEN 2.4b18.6 (firmware 11.50): a payload's
audio is heard on no port, though each takes its output at the hardware's
pace; and the music service's initialisation never returns in a payload,
nor does a payload's `scePthreadCreate`. So this probe runs inside the
shell, which is always heard and owns the quick menu.

`sc_ps4probe_shell.bin` carries a module, `sc_shellprobe.prx`. It writes the
module to `/data/ShellCompanion/`, then has GoldHEN load it into
`SceShellUI` (its SDK's `PROCESS_PRX_LOAD`, system call 500).

1. Send `sc_ps4probe_shell.bin` from the home screen.
2. Notifications should say "Inject probe: loaded into the shell?", then
   "Shell probe: running inside the shell".
3. Count the beeps, 20 s each: **1** main port, **2** BGM port, **3** type 14.
4. Send `ps4probe-inject.log` and `ps4probe-shell.log`.

A module that goes wrong inside the shell can take the home screen down with
it; restarting the PS4 brings it back. The `.prx` is made with OpenOrbis's
`create-fself --lib`, as GoldHEN's plugins are (`probe/get_create_fself.sh`
builds it).

`/data/ShellCompanion/ps4probe.stop` (any file) ends the first two probes early.
