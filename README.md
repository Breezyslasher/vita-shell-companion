# vita-shell-companion

A PS Vita SceShell plugin that any homebrew app can load so its work keeps
going after it closes:

- **Music row:** the quick menu's Music row shows the app's song and cover,
  with working ⏮ ⏯ ⏭.
- **Background music:** the app's queue plays on through the shell's own music
  player, beside games.
- **Background downloads:** downloads carry on, with one progress notification
  and a "Pause downloads?" question. They wait while the battery is low and
  the Vita is off power, and say so in the notification.
- **Several apps, one plugin:** the plugin answers each app's hello with its
  version and what it has, so an app can tell when the copy in SceShell is
  older than its own.
- **Self-update:** the app downloads its `.vpk`; once the app has closed, the
  plugin unpacks it, makes its `head.bin`, installs it with the Vita's own
  installer, and launches the app again.
- **Remote control:** while the shell plays an app's queue, the app's server
  or remote apps on the network can play, pause, skip, seek and start tracks,
  through an HTTP listener the app describes (a Plex player's companion
  protocol, for one).
- **Music Assistant in the background:** a Sendspin player app (Vita Music
  Assistant) hands its player over as it closes, and the Vita stays the same
  player, which Music Assistant keeps streaming to and controlling.
- **Skip or seek:** an app chooses whether the row's ⏮ ⏭ skip tracks or seek
  within them, as an audiobook app wants.

Each app turns on only the features it wants, and describes its work with
plain URLs, headers and report templates. The plugin knows nothing about any
particular server.

> **Status:** the Vita plugin runs on a real Vita with VitaPlex: background
> music, the quick menu's controls, background downloads and updates.
>
> **PS4 (GoldHEN, tested on 11.50):** background music, local and streamed,
> in the quick menu's Music tab, with covers, queue editing and a per-app
> "+"; app self-update; a client library. See [docs/PS4.md](docs/PS4.md).

> **Moving an app to this layout:** `client/` is now `vita/client/` and
> `include/` is now `common/`. Build `vita/client/*.c` as before, and put
> `common/` and `vita/client/` on the include path. Nothing in the files or
> the protocol changed.

## Using it in an app

1. Add `common/` and `vita/client/` to the app's include path, and build
   `vita/client/shell_companion.c`, `vita/client/sc_files.c` and
   `vita/client/sc_link_stub.S`, plus `vita/client/sc_cover.c` for covers (it
   needs `stb_image.h`).
2. Ship `shell_companion.suprx` in the app's package, for example in
   `module/`.
3. At start-up:

   ```c
   #include "shell_companion.h"

   scInit("ABCD00001", "ux0:data/MyApp", "My App",
          SC_FEAT_ROW | SC_FEAT_MUSIC | SC_FEAT_DOWNLOADS,
          "app0:module/shell_companion.suprx");  // loaded from ux0:app/<title id>/...
   scMusicTakeBack();        // if the shell was playing our queue, stop it
   scDownloadsTakeBack();    // and our downloads
   char what[256];
   scOpen(what, sizeof(what));
   ```

4. While playing, send `scRowActivate()` every second with `scRowSong`,
   `scRowPlaying` and `scRowArt`, and act on `scPoll()`.
5. On exit, `scWriteQueue(...)` then `scMusicPlay()`, and
   `scWriteDownloads(...)` then `scDownloadsStart()`.

## Layout

| Path | What |
|---|---|
| `vita/plugin/` | the Vita plugin itself (GPL-3.0, based on [ElevenMPV-A](https://github.com/GrapheneCt/ElevenMPV-A)'s by GrapheneCt) |
| `vita/client/` | the C library a Vita app links to load the plugin and talk to it (Apache-2.0) |
| `vita/scripts/` | PAF header fetch and PAF stub generator |
| `ps4/` | the PS4 counterpart: the player and the music watch loaded into SceShellUI, the client library (`ps4/client/`), a test app ([docs/PS4.md](docs/PS4.md); notes in `ps4/README.md`) |
| `common/` | what both share: the protocol and file formats, and the plugin's own code for Sendspin (`sendspin_wire.h`), DEFLATE (`sc_inflate.h`), SHA-1 and MP3 framing |
| `docs/PROTOCOL.md` | the Vita protocol: commands, files, and the client's calls |
| `docs/PS4.md` | the PS4 side: what it does, the client library, files and limits |

## Building

You need [vitasdk](https://vitasdk.org), or the `vitasdk/vitasdk:2026.08`
Docker image.

```sh
sh vita/scripts/fetch_paf_headers.sh build-paf
cmake -S vita -B build -G Ninja -DSC_PAF_INCLUDE_DIR=$PWD/build-paf/include
cmake --build build        # -> build/shell_companion.suprx, libshell_companion_client.a
```

The CI does the same and uploads the `.suprx` as an artifact.

## Requirements on the console

- **taiHEN:** HENkaku or Ensō.
- **Unsafe homebrew:** the app that loads the plugin must be unsafe homebrew,
  since loading uses `taiLoadStartModuleForPidForUser`.
- **Log:** `ux0:data/ShellCompanion/plugin.log`, one file a boot.
- **No `config.txt` entry:** apps load the plugin themselves, once per boot.

## Licence

| Part | Licence |
|---|---|
| The plugin | GPL-3.0 (`LICENSE`, `NOTICE.txt`) |
| `vita/client/`, `ps4/client/` | Apache-2.0 (their `LICENSE`), so apps under any licence can link them |

Apps only talk to the plugin through message pipes and files.
