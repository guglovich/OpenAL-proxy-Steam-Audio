#!/bin/bash
set -e

SRC="openal_proxy_sa_v11_hrtf.cpp"
OUT="OpenAL32.dll"

echo "=== Building LineageAudio LA 2.7 ==="
echo "Source: $SRC"
echo "Output: $OUT"

i686-w64-mingw32-g++ -O2 -Wall -std=c++17 -DWIN32_LEAN_AND_MEAN \
-shared -static-libgcc -static-libstdc++ \
-o "$OUT" \
"$SRC" \
-lkernel32 -luser32

file "$OUT" | grep -q "PE32" && echo "[OK] PE32 DLL built successfully" || echo "[WARN] Check DLL architecture"

ls -lh "$OUT"

echo "=== Build complete ==="
echo "LA 2.7: SA Direct Sound + HRTF binaural (440Hz test tone)"
