#!/bin/sh
# hello.prx, built as j117mw3/ps4-hen-rest's keeper/Makefile builds its
# SceShellUI module (verbatim flags, its crt from GoldHEN_Plugins_SDK at
# bcea3c7, which it pins). Usage: build.sh <out dir>
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
SRC=$(cd "$(dirname "$0")" && pwd)
OUT=$1
mkdir -p "$OUT"
FSELF=$OO/bin/linux/create-fself
[ -x "$FSELF" ] || FSELF=${CREATE_FSELF:-$OUT/create-fself}
CFLAGS="--target=x86_64-pc-freebsd12-elf -std=c11 -fPIC -funwind-tables -mstackrealign -O1 -g
        -Wall -Wextra -isysroot $OO -I$OO/include"
LDFLAGS="-m elf_x86_64 -pie --script $OO/link.x -e _init --eh-frame-hdr
         -z max-page-size=0x4000 -z common-page-size=0x4000 -L$OO/lib"
clang $CFLAGS -c "$SRC/crtprx.c" -o "$OUT/crtprx.o"
clang $CFLAGS -c "$SRC/hello.c" -o "$OUT/hello.o"
ld.lld $LDFLAGS "$OUT/crtprx.o" "$OUT/hello.o" -lSceLibcInternal -lkernel -o "$OUT/hello.elf"
localize "$OUT/hello.elf"
"$FSELF" -in="$OUT/hello.elf" -out="$OUT/hello.oelf" --lib="$OUT/hello.prx" \
    --paid 0x3800000000000011 >/dev/null
ls -l "$OUT/hello.prx"
