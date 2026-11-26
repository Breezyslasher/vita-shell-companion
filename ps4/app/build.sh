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
# Another title, as CI builds the second package (TITLE_ID=CUSA01780, the
# title the Music section knows as Spotify's: the shell then finds "Spotify"
# installed, ps4/README.md).
TITLE=${TITLE:-"Shell Companion BGM Test"}
TITLE_ID=${TITLE_ID:-SCMP00001}
CONTENT_ID=${CONTENT_ID:-IV0000-SCMP00001_00-SHELLCOMPBGM0001}
VERSION=01.00
BIN=$OO/bin/linux
# PkgTool is .NET; without ICU (outside the image) it wants this.
[ -e /usr/lib/x86_64-linux-gnu/libicuuc.so.66 ] || export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

OUT="$OUT" sh "$SRC/../bgm/build.sh"

clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -Werror \
    -isysroot "$OO" -isystem "$OO/include" "-DSC_PRX_PATH=\"$OUT/sc_bgm.prx\"" "-DSC_TITLE_ID=\"$TITLE_ID\"" \
    -c "$SRC/main.c" -o "$OUT/app.o"
ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -o "$OUT/app.elf" "$OUT/app.o" \
    -L"$OO/lib" -lc -lkernel -lSceVideoOut -lScePad -lSceUserService -lSceSysmodule -lSceSystemService "$OO/lib/crt1.o"

PKG=$OUT/pkg
rm -rf "$PKG"
mkdir -p "$PKG/sce_sys/about" "$PKG/sce_module"
CREATE_FSELF=${CREATE_FSELF:-$OUT/create-fself}
"$CREATE_FSELF" -in="$OUT/app.elf" -out="$OUT/app.oelf" --eboot "$PKG/eboot.bin" \
    --paid 0x3800000000000011
cp "$SRC/sce_sys/icon0.png" "$PKG/sce_sys/"
cp "$SRC/sce_sys/about/right.sprx" "$PKG/sce_sys/about/"
cp "$SRC/sce_module/libc.prx" "$SRC/sce_module/libSceFios2.prx" "$PKG/sce_module/"
# The module, in the package too: a place the system may let it load from.
cp "$OUT/sc_bgm.prx" "$OUT/sc_mps.prx" "$OUT/sc_mcw.prx" "$PKG/"
# A test module built as ps4-hen-rest builds its shell module (recipe/).
CREATE_FSELF="$CREATE_FSELF" sh "$SRC/recipe/build.sh" "$OUT/recipe"
cp "$OUT/recipe/hello.prx" "$PKG/"
# GoldHEN's own plugin, as a control (control/README.md).
cp "$SRC/control/no_share_watermark.prx" "$PKG/"

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
# The package's project (.gp4), as create-gp4 makes it; written here, as
# the CI's image has no create-gp4.
FILES="eboot.bin sc_bgm.prx sc_mps.prx sc_mcw.prx hello.prx no_share_watermark.prx sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx"
{
    echo '<?xml version="1.0"?>'
    echo '<psproject xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" fmt="gp4" version="1000">'
    echo '	<volume>'
    echo '		<volume_type>pkg_ps4_app</volume_type>'
    echo '		<volume_id>PS4VOLUME</volume_id>'
    echo "		<volume_ts>$(date -u '+%Y-%m-%d %H:%M:%S')</volume_ts>"
    echo "		<package content_id=\"$CONTENT_ID\" passcode=\"00000000000000000000000000000000\" storage_type=\"digital50\" app_type=\"full\" />"
    echo '		<chunk_info chunk_count="1" scenario_count="1">'
    echo '			<chunks>'
    echo '				<chunk id="0" layer_no="0" label="Chunk #0" />'
    echo '			</chunks>'
    echo '			<scenarios default_id="0">'
    echo '				<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>'
    echo '			</scenarios>'
    echo '		</chunk_info>'
    echo '	</volume>'
    echo '	<files img_no="0">'
    for f in $FILES; do echo "		<file targ_path=\"$f\" orig_path=\"$f\" />"; done
    echo '	</files>'
    echo '	<rootdir>'
    echo '		<dir targ_name="sce_sys">'
    echo '			<dir targ_name="about" />'
    echo '		</dir>'
    echo '		<dir targ_name="sce_module" />'
    echo '	</rootdir>'
    echo '</psproject>'
} > pkg.gp4
"$BIN/PkgTool.Core" pkg_build pkg.gp4 "$OUT"
ls -l "$OUT/$CONTENT_ID.pkg"
