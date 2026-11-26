#!/bin/sh
# hello.prx, built as j117mw3/ps4-hen-rest's keeper/Makefile builds its
# SceShellUI module (verbatim flags, its crt from GoldHEN_Plugins_SDK at
# bcea3c7, which it pins). Usage: build.sh <out dir>
set -eu
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
"$FSELF" -in="$OUT/hello.elf" -out="$OUT/hello.oelf" --lib="$OUT/hello.prx" \
    --paid 0x3800000000000011 >/dev/null
ls -l "$OUT/hello.prx"
