#!/usr/bin/env bash
# Build 1-Remind!.exe with MinGW-w64 (works on Linux as a cross-compiler,
# or on Windows under MSYS2). Produces a single, portable, dependency-free exe.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"

mkdir -p dist
$WINDRES -I src src/resource.rc -O coff -o src/resource.o

$CXX -std=c++17 -Os \
  -ffunction-sections -fdata-sections \
  -DUNICODE -D_UNICODE \
  -municode -mwindows \
  -static-libgcc -static-libstdc++ \
  -Wl,--gc-sections,--high-entropy-va,--dynamicbase,--nxcompat -s \
  src/main.cpp src/resource.o \
  -o 'dist/1-Remind!.exe' \
  -ldwmapi -lsapi -lwinmm -lwtsapi32 -lole32 -lshell32 -lgdi32 -ladvapi32

rm -f src/resource.o
echo "Built dist/1-Remind!.exe"
