#!/bin/sh
# Builds the PS4 test app, Shell Companion BGM Test, as a .pkg to install
# with GoldHEN's Package Installer (or Debug Settings). It carries
# sc_bgm.prx, so ps4/bgm/build.sh runs first. With the OpenOrbis toolchain
# (OO_PS4_TOOLCHAIN) and create-fself (ps4/probe/get_create_fself.sh), from
# the repository's root:
#
#   docker run --rm --user 0 -v "$PWD":/src -w /src openorbisofficial/toolchain:latest sh ps4/app/build.sh
#
# -> build-ps4/IV0000-SCMP00001_00-SHELLCOMPBGM0001.pkg
set -eu
OO=${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN is not set}
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-build-ps4}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
TITLE="Shell Companion BGM Test"
TITLE_ID=SCMP00001
CONTENT_ID=IV0000-SCMP00001_00-SHELLCOMPBGM0001
VERSION=01.00
BIN=$OO/bin/linux
# PkgTool is .NET; without ICU (outside the image) it wants this.
[ -e /usr/lib/x86_64-linux-gnu/libicuuc.so.66 ] || export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

OUT="$OUT" sh "$SRC/../bgm/build.sh"

clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -Werror \
    -isysroot "$OO" -isystem "$OO/include" "-DSC_PRX_PATH=\"$OUT/sc_bgm.prx\"" \
    -c "$SRC/main.c" -o "$OUT/app.o"
ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -o "$OUT/app.elf" "$OUT/app.o" \
    -L"$OO/lib" -lc -lkernel -lSceVideoOut -lScePad -lSceUserService "$OO/lib/crt1.o"

PKG=$OUT/pkg
rm -rf "$PKG"
mkdir -p "$PKG/sce_sys/about" "$PKG/sce_module"
CREATE_FSELF=${CREATE_FSELF:-$OUT/create-fself}
"$CREATE_FSELF" -in="$OUT/app.elf" -out="$OUT/app.oelf" --eboot "$PKG/eboot.bin" \
    --paid 0x3800000000000011
cp "$SRC/sce_sys/icon0.png" "$PKG/sce_sys/"
cp "$SRC/sce_sys/about/right.sprx" "$PKG/sce_sys/about/"
cp "$SRC/sce_module/libc.prx" "$SRC/sce_module/libSceFios2.prx" "$PKG/sce_module/"

cd "$PKG"
SFO=sce_sys/param.sfo
"$BIN/PkgTool.Core" sfo_new $SFO
"$BIN/PkgTool.Core" sfo_setentry $SFO APP_TYPE --type Integer --maxsize 4 --value 1
"$BIN/PkgTool.Core" sfo_setentry $SFO APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
"$BIN/PkgTool.Core" sfo_setentry $SFO ATTRIBUTE --type Integer --maxsize 4 --value 0
"$BIN/PkgTool.Core" sfo_setentry $SFO CATEGORY --type Utf8 --maxsize 4 --value gd
"$BIN/PkgTool.Core" sfo_setentry $SFO CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$BIN/PkgTool.Core" sfo_setentry $SFO DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$BIN/PkgTool.Core" sfo_setentry $SFO SYSTEM_VER --type Integer --maxsize 4 --value 0
"$BIN/PkgTool.Core" sfo_setentry $SFO TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$BIN/PkgTool.Core" sfo_setentry $SFO TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$BIN/PkgTool.Core" sfo_setentry $SFO VERSION --type Utf8 --maxsize 8 --value "$VERSION"
"$BIN/create-gp4" -out pkg.gp4 --content-id="$CONTENT_ID" \
    --files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx"
"$BIN/PkgTool.Core" pkg_build pkg.gp4 "$OUT"
ls -l "$OUT/$CONTENT_ID.pkg"
