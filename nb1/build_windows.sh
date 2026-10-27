#!/bin/sh
# Cross-compile nb1_tool.exe, pbdesk.exe and pbvr.exe for 64-bit Windows with MinGW-w64 (apt install gcc-mingw-w64-x86-64 libopenxr-dev).
#   SDL2_MINGW = the x86_64-w64-mingw32 folder of SDL2-devel-2.x-mingw.tar.gz      ZLIB_SRC = a zlib source folder built with
#   make -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a
# Run the result with SDL2.dll beside it; pbvr also needs openxr_loader.dll from your OpenXR runtime / the OpenXR SDK next to it (or on PATH).
set -e
: "${SDL2_MINGW:?set SDL2_MINGW}"
# zlib: ZLIB_SRC (a source folder holding zlib.h and libz.a), or ZLIB_INC and ZLIB_LIB separately (MSYS2: /mingw64/include and /mingw64/lib/libz.a)
ZLIB_INC=${ZLIB_INC:-$ZLIB_SRC}; ZLIB_LIB=${ZLIB_LIB:-$ZLIB_SRC/libz.a}; : "${ZLIB_INC:?set ZLIB_SRC or ZLIB_INC/ZLIB_LIB}"
OPENXR_INC=${OPENXR_INC:-/usr/include}                      # folder that contains openxr/openxr.h (MSYS2: /mingw64/include)
 OUT=${OUT:-build-win}; mkdir -p "$OUT"
CC=${CC:-x86_64-w64-mingw32-gcc}
SRC="nb1_video.c nb1_scene.c nb1_table.c nb1_edits.c nb1_png.c nb1_json.c nb1_surround.c nb1_sprites.c nb1_bot.c nb1_flow.c"
CF="-O2 -std=gnu11 -ffp-contract=off -Wall -Wno-missing-field-initializers -Wno-unused-function -Wno-unused-variable -I$ZLIB_INC -I$SDL2_MINGW/include/SDL2 -I$SDL2_MINGW/include -idirafter $OPENXR_INC"
$CC $CF -o "$OUT/nb1_tool.exe" $SRC nb1_tool.c "$ZLIB_LIB" -lm
for t in desktop/pbdesk:pbdesk vr/pbvr:pbvr; do
  $CC $CF -o "$OUT/${t#*:}.exe" $SRC "${t%:*}.c" -L"$SDL2_MINGW/lib" -lmingw32 -lSDL2main -lSDL2 -lopengl32 "$ZLIB_LIB" -lm -Wl,--stack,67108864
done
$CC $CF -o "$OUT/pbbot.exe" $SRC desktop/pbbot.c "$ZLIB_LIB" -lm
cp "$SDL2_MINGW/bin/SDL2.dll" "$OUT/" 2>/dev/null || echo "copy SDL2.dll next to the executables yourself"
ls -l "$OUT"
