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

1. Install the 0.27 test app (0.26 hooked nothing: its module list hid the
   system libraries). Restart the PS4 if an older watch is loaded
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
