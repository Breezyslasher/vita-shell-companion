#!/bin/sh
# Builds OpenOrbis's create-fself, at a pinned commit, into $1 (default
# build-ps4/create-fself): the tool GoldHEN's plugins are made into .prx
# with (create-fself --lib). The toolchain image's create-lib stopped with
# "Failed to build FSPRX: EOF" on the shell probe. Needs git and Go, e.g.:
#
#   docker run --rm -v "$PWD":/src -w /src golang:1.22 sh ps4/probe/get_create_fself.sh
set -eu
OUT=${1:-build-ps4/create-fself}
COMMIT=f2da6229684ce320f673c06fceac9c218f621788
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
git clone --quiet https://github.com/OpenOrbis/create-fself "$WORK/create-fself"
git -C "$WORK/create-fself" checkout --quiet "$COMMIT"
mkdir -p "$(dirname "$OUT")"
OUT_ABS=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
(cd "$WORK/create-fself/cmd/create-fself" &&
    CGO_ENABLED=0 GOFLAGS=-mod=mod go build -o "$OUT_ABS" -modfile=go-linux.mod .)
echo "create-fself $COMMIT -> $OUT"
