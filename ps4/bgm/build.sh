#!/bin/sh
# Builds the PS4 background music: sc_bgm.prx, the module that plays inside
# SceShellUI, and sc_bgm.bin, the raw payload for GoldHEN's BinLoader that
# has it loaded. With the OpenOrbis toolchain (OO_PS4_TOOLCHAIN) and
# create-fself (ps4/probe/get_create_fself.sh), from the repository's root:
#
#   docker run --rm --user 0 -v "$PWD":/src -w /src openorbisofficial/toolchain:latest sh ps4/bgm/build.sh
set -eu
OO=${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN is not set}
SRC=$(dirname "$0")
RT=$SRC/../probe
OUT=${OUT:-build-ps4}
mkdir -p "$OUT"
CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -fvisibility=hidden -ffreestanding -fno-builtin
        -fno-stack-protector -fno-jump-tables -O2 -isystem $OO/include"
WARN="-Wall -Wextra -Werror"
# The module: a .prx, which the system's loader relocates, inside the shell.
# With unwind tables, as OpenOrbis builds everything: without them the
# module had no PT_GNU_EH_FRAME, and the PS4 refused it as not executable
# (ENOEXEC) where it loaded OpenOrbis's own modules.
MODFLAGS="-funwind-tables -fasynchronous-unwind-tables"
clang $CFLAGS $MODFLAGS $WARN -DSC_PRX -c "$RT/prx_crt.c" -o "$OUT/bgm_crt.o"
clang $CFLAGS $MODFLAGS $WARN -DSC_PRX -c "$RT/sys.c" -o "$OUT/bgm_sys.o"
clang $CFLAGS $MODFLAGS $WARN -DSC_PRX -c "$SRC/bgm.c" -o "$OUT/bgm.o"
clang $CFLAGS $MODFLAGS -w -DSC_PRX -c "$SRC/mp3.c" -o "$OUT/bgm_mp3.o"  # minimp3, as it is
ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -e _init \
    -o "$OUT/sc_bgm.elf" "$OUT/bgm_crt.o" "$OUT/bgm_sys.o" "$OUT/bgm.o" "$OUT/bgm_mp3.o" \
    -L"$OO/lib" -lkernel
CREATE_FSELF=${CREATE_FSELF:-$OUT/create-fself}
"$CREATE_FSELF" -in="$OUT/sc_bgm.elf" -out="$OUT/sc_bgm.oelf" \
    --lib="$OUT/sc_bgm.prx" --paid 0x3800000000000011
# Two other builds, for the test app to try, as a PS4 (GoldHEN 2.4b18.6,
# 11.50) refused sc_bgm.prx as not executable (ENOEXEC) where it loaded
# OpenOrbis's own libSceFios2.prx:
# b: the same ELF, made a .prx with the toolchain's own create-fself, which
#    OpenOrbis's modules are made with, if it has one;
# c: built without -fvisibility=hidden, every global exported, as GoldHEN's
#    plugins are, with that create-fself too.
SDK_FSELF=$OO/bin/linux/create-fself
[ -x "$SDK_FSELF" ] || SDK_FSELF=$CREATE_FSELF
"$SDK_FSELF" -in="$OUT/sc_bgm.elf" -out="$OUT/sc_bgm_b.oelf" \
    --lib="$OUT/sc_bgm_b.prx" --paid 0x3800000000000011
CFLAGS_C=$(echo "$CFLAGS" | sed 's/-fvisibility=hidden//')
for f in "$RT/prx_crt.c" "$RT/sys.c" "$SRC/bgm.c"; do
    clang $CFLAGS_C $MODFLAGS $WARN -DSC_PRX -c "$f" -o "$OUT/c_$(basename "$f" .c).o"
done
clang $CFLAGS_C $MODFLAGS -w -DSC_PRX -c "$SRC/mp3.c" -o "$OUT/c_mp3.o"
ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -e _init \
    -o "$OUT/sc_bgm_c.elf" "$OUT/c_prx_crt.o" "$OUT/c_sys.o" "$OUT/c_bgm.o" "$OUT/c_mp3.o" \
    -L"$OO/lib" -lkernel
"$SDK_FSELF" -in="$OUT/sc_bgm_c.elf" -out="$OUT/sc_bgm_c.oelf" \
    --lib="$OUT/sc_bgm_c.prx" --paid 0x3800000000000011
# The loader: a raw payload, the module inside it.
clang $CFLAGS $WARN -c "$RT/sys.c" -o "$OUT/bgm_load_sys.o"
clang $CFLAGS $WARN "-DSC_PRX_PATH=\"$OUT/sc_bgm.prx\"" -c "$SRC/load.c" -o "$OUT/bgm_load.o"
ld.lld -m elf_x86_64 -pie --script "$RT/raw.x" -e _start \
    -o "$OUT/sc_bgm_raw.elf" "$OUT/bgm_load_sys.o" "$OUT/bgm_load.o"
if objdump -h "$OUT/sc_bgm_raw.elf" | grep -q '\.rela\.dyn'; then
    echo "bgm: the raw payload has relocations: a pointer kept in data" >&2
    exit 1
fi
objcopy --set-section-flags .bss=alloc,load,contents -O binary -R .rela.dyn \
    "$OUT/sc_bgm_raw.elf" "$OUT/sc_bgm.bin"
for e in sc_bgm sc_bgm_c; do
    if ! readelf -lW "$OUT/$e.elf" | grep -q GNU_EH_FRAME; then
        echo "bgm: $e.elf has no PT_GNU_EH_FRAME" >&2
        exit 1
    fi
done
ls -l "$OUT"/sc_bgm*.prx "$OUT/sc_bgm.bin"
