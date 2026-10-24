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

Each app turns on only the features it wants, and describes its work with
plain URLs, headers and report templates. The plugin knows nothing about any
particular server.

> **Status:** this is VitaPlex's quick menu plugin (protocol 17), moved here
> unchanged as the starting point. It is being generalised to the shared
> protocol in [`docs/PROTOCOL.md`](docs/PROTOCOL.md), which is still a draft.
> Until that lands, only VitaPlex can use it.

## Layout

| Path | What |
|---|---|
| `plugin/` | the plugin itself (GPL-3.0, based on [ElevenMPV-A](https://github.com/GrapheneCt/ElevenMPV-A)'s by GrapheneCt) |
| `include/` | the protocol and file formats the plugin and apps share |
| `client/` | the C library an app links to load the plugin and talk to it (Apache-2.0) |
| `docs/PROTOCOL.md` | the shared protocol (draft) |
| `scripts/` | PAF header fetch and PAF stub generator |

## Building

You need [vitasdk](https://vitasdk.org), or the `vitasdk/vitasdk:2026.08`
Docker image.

```sh
sh scripts/fetch_paf_headers.sh build-paf
cmake -B build -G Ninja -DSC_PAF_INCLUDE_DIR=$PWD/build-paf/include
cmake --build build        # -> build/shell_companion.suprx
```

The CI does the same and uploads the `.suprx` as an artifact.

## Requirements on the console

- **taiHEN:** HENkaku or Ensō.
- **Unsafe homebrew:** the app that loads the plugin must be unsafe homebrew,
  since loading uses `taiLoadStartModuleForPidForUser`.
- **No `config.txt` entry:** apps load the plugin themselves, once per boot.

## Licence

| Part | Licence |
|---|---|
| The plugin | GPL-3.0 (`LICENSE`, `NOTICE.txt`) |
| `client/` | Apache-2.0 (`client/LICENSE`), so apps under any licence can link it |

Apps only talk to the plugin through message pipes and files.
