# vita-shell-companion

A PS Vita SceShell plugin that any homebrew app can load so its work keeps
going after it closes:

- **Music row:** the quick menu's Music row shows the app's song and cover,
  with working ⏮ ⏯ ⏭.
- **Background music:** the app's queue plays on through the shell's own music
  player, beside games.
- **Background downloads:** downloads carry on, with one progress notification
  and a "Pause downloads?" question.
- **Self-update:** the plugin installs the app's update once it has closed,
  then launches it again.
- **Remote control:** while the shell plays an app's queue, the app's server
  or remote apps on the network can play, pause, skip, seek and start tracks,
  through an HTTP listener the app describes (a Plex player's companion
  protocol, for one).
- **Skip or seek:** an app chooses whether the row's ⏮ ⏭ skip tracks or seek
  within them, as an audiobook app wants.

Each app turns on only the features it wants, and describes its work with
plain URLs, headers and report templates. The plugin knows nothing about any
particular server.

> **Status:** protocol 1 is implemented ([`docs/PROTOCOL.md`](docs/PROTOCOL.md))
> but not yet tested on a console. It is VitaPlex's quick menu plugin made
> app-agnostic; the parts that talk to SceShell are unchanged from it.

## Using it in an app

1. Add `include/` and `client/` to the app's build: `client/shell_companion.c`,
   `client/sc_files.c` and `client/sc_link_stub.S`, plus `client/sc_cover.c`
   for covers (it needs `stb_image.h`).
2. Ship `shell_companion.suprx` in the app's package, for example in
   `module/`.
3. At start-up:

   ```c
   #include "shell_companion.h"

   scInit("ABCD00001", "ux0:data/MyApp", "My App",
          SC_FEAT_ROW | SC_FEAT_MUSIC | SC_FEAT_DOWNLOADS,
          "app0:module/shell_companion.suprx");
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
| `plugin/` | the plugin itself (GPL-3.0, based on [ElevenMPV-A](https://github.com/GrapheneCt/ElevenMPV-A)'s by GrapheneCt) |
| `include/` | the protocol and file formats the plugin and apps share |
| `client/` | the C library an app links to load the plugin and talk to it (Apache-2.0) |
| `docs/PROTOCOL.md` | the protocol: commands, files, and the client's calls |
| `scripts/` | PAF header fetch and PAF stub generator |

## Building

You need [vitasdk](https://vitasdk.org), or the `vitasdk/vitasdk:2026.08`
Docker image.

```sh
sh scripts/fetch_paf_headers.sh build-paf
cmake -B build -G Ninja -DSC_PAF_INCLUDE_DIR=$PWD/build-paf/include
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
| `client/` | Apache-2.0 (`client/LICENSE`), so apps under any licence can link it |

Apps only talk to the plugin through message pipes and files.
