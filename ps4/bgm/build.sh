#!/bin/sh
# Builds the PS4 background music: sc_bgm.prx, the module that plays inside
# SceShellUI, and sc_bgm.bin, the raw payload for GoldHEN's BinLoader that
# has it loaded. With the OpenOrbis toolchain (OO_PS4_TOOLCHAIN) and
# create-fself (ps4/probe/get_create_fself.sh), from the repository's root:
#
#   docker run --rm --user 0 -v "$PWD":/src -w /src openorbisofficial/toolchain:latest sh ps4/bgm/build.sh
set -eu
# Global symbols with no type (link.x's __text_start, __data_relro_start
# and the like, prx_crt's __dso_handle) made local before create-fself,
# which exports every global: GoldHEN's own plugins, which load where ours
# were refused (ENOEXEC), export only functions and objects.
localize() {
    args=""
    for s in $(objdump -t "$1" | awk 'NF==5 && $2=="g" {print $5}'); do
        args="$args --localize-symbol=$s"
    done
    [ -z "$args" ] || objcopy $args "$1"
    if objdump -t "$1" | awk 'NF==5 && $2=="g"' | grep -q .; then
        echo "$1: global symbols with no type remain" >&2
        exit 1
    fi
}
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
# 16 KB pages (-z max-page-size, common-page-size), as both projects that
# load a module into SceShellUI link theirs (j117mw3/ps4-hen-rest, with
# lld 18 too, and Zenalios/ps4-thumbgrid-ime): lld's default, 4 KB, left
# the read-only segment at 0x9510, and the PS4 refused the module
# (ENOEXEC) where it loaded OpenOrbis's own.
PAGES="-z max-page-size=0x4000 -z common-page-size=0x4000"
ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -e _init $PAGES \
    -o "$OUT/sc_bgm.elf" "$OUT/bgm_crt.o" "$OUT/bgm_sys.o" "$OUT/bgm.o" "$OUT/bgm_mp3.o" \
    -L"$OO/lib" -lkernel
localize "$OUT/sc_bgm.elf"
CREATE_FSELF=${CREATE_FSELF:-$OUT/create-fself}
"$CREATE_FSELF" -in="$OUT/sc_bgm.elf" -out="$OUT/sc_bgm.oelf" \
    --lib="$OUT/sc_bgm.prx" --paid 0x3800000000000011
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
for e in sc_bgm; do
    if ! objdump -p "$OUT/$e.elf" | grep -q EH_FRAME; then  # GNU objdump: the image's readelf prints nothing
        echo "bgm: $e.elf has no PT_GNU_EH_FRAME" >&2
        exit 1
    fi
    # Every LOAD segment aligned to 16 KB pages (where the segments start
    # is link.x's, as in the projects above).
    if objdump -p "$OUT/$e.elf" | grep LOAD | grep -qv 'align 2\*\*14'; then
        echo "bgm: $e.elf has a LOAD segment not aligned to 16 KB" >&2
        exit 1
    fi
done
ls -l "$OUT"/sc_bgm*.prx "$OUT/sc_bgm.bin"
