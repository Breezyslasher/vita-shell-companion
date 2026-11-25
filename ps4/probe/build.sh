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
# Hidden visibility: every reference to data is relative to the code, with
# no GOT entry or relocation for a raw loader to fill in.
CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -fvisibility=hidden -ffreestanding -fno-builtin
        -fno-stack-protector -fno-jump-tables -O2 -Wall -Wextra -Werror -isystem $OO/include"
for probe in audio music; do
    clang $CFLAGS -c "$SRC/sys.c" -o "$OUT/sys.o"
    clang $CFLAGS -c "$SRC/${probe}_probe.c" -o "$OUT/${probe}.o"
    # An ELF, for an ELF loader.
    ld.lld -m elf_x86_64 -pie --script "$OO/link.x" --eh-frame-hdr -e _start \
        -o "$OUT/sc_ps4probe_$probe.elf" "$OUT/sys.o" "$OUT/$probe.o"
    # The raw payload (.bin), for GoldHEN's BinLoader: the image from 0,
    # .bss included as zeros.
    # Linked as PIE, so that an absolute address in data shows as a
    # relocation, which a raw loader would not apply: the check below then
    # refuses it.
    ld.lld -m elf_x86_64 -pie --script "$SRC/raw.x" -e _start \
        -o "$OUT/sc_ps4probe_${probe}_raw.elf" "$OUT/sys.o" "$OUT/$probe.o"
    # GNU objdump: the image's readelf is OpenOrbis's own, which prints
    # nothing for this.
    if objdump -R "$OUT/sc_ps4probe_${probe}_raw.elf" | grep R_X86_64; then
        echo "$probe: the raw payload has relocations: a pointer kept in data" >&2
        exit 1
    fi
    objcopy --set-section-flags .bss=alloc,load,contents -O binary -R .rela.dyn \
        "$OUT/sc_ps4probe_${probe}_raw.elf" "$OUT/sc_ps4probe_$probe.bin"
done
ls -l "$OUT"/*.elf "$OUT"/*.bin
