#!/bin/sh
# Builds the PS4 probes as ELF payloads for GoldHEN, with the OpenOrbis
# toolchain (openorbisofficial/toolchain, or OO_PS4_TOOLCHAIN set). From the
# repository's root:
#
#   docker run --rm -v "$PWD":/src -w /src openorbisofficial/toolchain:latest sh ps4/probe/build.sh
#
# -> build-ps4/sc_ps4probe_audio.elf and sc_ps4probe_music.elf, and the same
# as .bin, for a payload sender. Freestanding: no libc, no imports to
# resolve; the system's libraries are loaded at run time (sys.c).
set -eu
OO=${OO_PS4_TOOLCHAIN:?OO_PS4_TOOLCHAIN is not set}
SRC=$(dirname "$0")
OUT=${OUT:-build-ps4}
mkdir -p "$OUT"
CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -ffreestanding -fno-builtin -fno-stack-protector
        -funwind-tables -O2 -Wall -Wextra -Werror -isystem $OO/include"
for probe in audio music; do
    clang $CFLAGS -c "$SRC/sys.c" -o "$OUT/sys.o"
    clang $CFLAGS -c "$SRC/${probe}_probe.c" -o "$OUT/${probe}.o"
    ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -e _start \
        -o "$OUT/sc_ps4probe_$probe.elf" "$OUT/sys.o" "$OUT/$probe.o"
    cp "$OUT/sc_ps4probe_$probe.elf" "$OUT/sc_ps4probe_$probe.bin"
done
ls -l "$OUT"/*.elf
