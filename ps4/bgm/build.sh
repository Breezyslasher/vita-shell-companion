#!/bin/sh
# Builds Shell Companion's PS4 module, sc_companion.prx: the player (ps4/bgm)
# and the music watch (ps4/mcw) in one module, loaded into SceShellUI once a
# boot by the client library (ps4/client). With the OpenOrbis toolchain
# (OO_PS4_TOOLCHAIN) and create-fself (ps4/probe/get_create_fself.sh), from
# the repository's root:
#
#   docker run --rm --user 0 -v "$PWD":/src -w /src openorbisofficial/toolchain:latest sh ps4/bgm/build.sh
#
# A .prx, which the system's loader relocates, inside the shell. With unwind
# tables, as OpenOrbis builds everything: without them the module had no
# PT_GNU_EH_FRAME, and the PS4 refused it as not executable (ENOEXEC). Its
# LOAD segments on 16 KB pages, as GoldHEN's own plugins.
#
# SC_DEBUG=1 builds the debugging aids in too (ps4/README.md, "Debugging").
set -eu
OO=${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN is not set}
SRC=$(dirname "$0")
RT=$SRC/../probe
OUT=${OUT:-build-ps4}
mkdir -p "$OUT"
DEBUG=""
[ "${SC_DEBUG:-0}" = 1 ] && DEBUG="-DSC_DEBUG"
CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -fvisibility=hidden -ffreestanding -fno-builtin
        -fno-stack-protector -fno-jump-tables -O2 -isystem $OO/include -DSC_PRX -DSC_COMPANION $DEBUG"
WARN="-Wall -Wextra -Werror"
MODFLAGS="-funwind-tables -fasynchronous-unwind-tables"
clang $CFLAGS $MODFLAGS $WARN -c "$RT/prx_crt.c" -o "$OUT/sc_crt.o"
clang $CFLAGS $MODFLAGS $WARN -c "$RT/sys.c" -o "$OUT/sc_sys.o"
clang $CFLAGS $MODFLAGS $WARN -c "$SRC/../companion.c" -o "$OUT/sc_companion.o"
clang $CFLAGS $MODFLAGS $WARN -c "$SRC/bgm.c" -o "$OUT/bgm.o"
clang $CFLAGS $MODFLAGS -w -c "$SRC/mp3.c" -o "$OUT/bgm_mp3.o"  # minimp3, as it is
clang $CFLAGS $MODFLAGS $WARN -c "$SRC/../mcw/mcw.c" -o "$OUT/mcw.o"
clang $CFLAGS $MODFLAGS $WARN -c "$SRC/../mcw/hook.c" -o "$OUT/mcw_hook.o"
clang $CFLAGS $MODFLAGS -w -c "$SRC/../mcw/hde64.c" -o "$OUT/mcw_hde64.o"
PAGES="-z max-page-size=0x4000 -z common-page-size=0x4000"
ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -e _init $PAGES \
    -o "$OUT/sc_companion.elf" "$OUT/sc_crt.o" "$OUT/sc_sys.o" "$OUT/sc_companion.o" "$OUT/bgm.o" \
    "$OUT/bgm_mp3.o" "$OUT/mcw.o" "$OUT/mcw_hook.o" "$OUT/mcw_hde64.o" -L"$OO/lib" -lkernel -lSceAudioOut
CREATE_FSELF=${CREATE_FSELF:-$OUT/create-fself}
"$CREATE_FSELF" -in="$OUT/sc_companion.elf" -out="$OUT/sc_companion.oelf" \
    --lib="$OUT/sc_companion.prx" --paid 0x3800000000000011
if ! objdump -p "$OUT/sc_companion.elf" | grep -q EH_FRAME; then
    echo "sc_companion.elf has no PT_GNU_EH_FRAME" >&2
    exit 1
fi
if objdump -p "$OUT/sc_companion.elf" | grep LOAD | grep -qv 'align 2\*\*14'; then
    echo "sc_companion.elf has a LOAD segment not aligned to 16 KB" >&2
    exit 1
fi
ls -l "$OUT/sc_companion.prx"
