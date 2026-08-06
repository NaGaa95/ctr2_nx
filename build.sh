#!/usr/bin/env bash
set -e
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export DEVKITARM=/opt/devkitpro/devkitARM
export DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
cd "$(dirname "$0")" || exit 1
export HOME=".build-home"
export TMPDIR=".tmp"
export TMP="$TMPDIR"
export TEMP="$TMPDIR"
if [ "$1" = "clean" ]; then make clean; shift; fi
mkdir -p .build-home .tmp build/.tmp build/.build-home
make "$@"
