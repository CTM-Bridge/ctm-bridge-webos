#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIST_DIR="$(cd "$SOURCE_DIR/.." && pwd)/dist"
WORK_DIR="${WORK_DIR:-$SOURCE_DIR/build/webos-arm}"
SDK="${SDK:-$HOME/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$SDK/share/buildroot/toolchainfile.cmake}"
REUSE_LVGL="${REUSE_LVGL:-0}"
LVGL_CACHE_DIR="${LVGL_CACHE_DIR:-$WORK_DIR/prebuilt}"
LVGL_CACHE_LIB="$LVGL_CACHE_DIR/libctm_lvgl.a"

for tool in cmake cpack ares-package rsync pkg-config; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required tool: $tool" >&2
    exit 1
  fi
done

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
  echo "Missing webOS ARM toolchain file: $TOOLCHAIN_FILE" >&2
  echo "Set SDK=/path/to/arm-webos-linux-gnueabi_sdk-buildroot or TOOLCHAIN_FILE=/path/to/toolchainfile.cmake" >&2
  exit 1
fi

if [[ ! -x "$SDK/bin/arm-webos-linux-gnueabi-gcc" ]]; then
  echo "Missing webOS ARM compiler: $SDK/bin/arm-webos-linux-gnueabi-gcc" >&2
  exit 1
fi

if [[ ! -f "$SOURCE_DIR/third_party/lvgl/src/lvgl.h" ]]; then
  echo "Missing LVGL source: $SOURCE_DIR/third_party/lvgl" >&2
  echo "Put LVGL under third_party/lvgl before building" >&2
  exit 1
fi

mkdir -p "$WORK_DIR"
rsync -a --delete --exclude build --exclude dist "$SOURCE_DIR/" "$WORK_DIR/src/"

cd "$WORK_DIR/src"
mkdir -p assets
if [[ ! -f icon.png ]]; then
  printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII=' | base64 -d > icon.png
fi
if [[ ! -f assets/font.ttf ]]; then
  curl -fsSL -o assets/font.ttf 'https://github.com/dejavu-fonts/dejavu-fonts/raw/version_2_37/ttf/DejaVuSans.ttf'
fi

font_bytes="$(wc -c < assets/font.ttf | tr -d '[:space:]')"
if [[ "$font_bytes" -lt 32768 ]]; then
  echo "assets/font.ttf missing or invalid ($font_bytes bytes)" >&2
  exit 1
fi

ctm_prebuilt_lvgl=""
if [[ "$REUSE_LVGL" == "1" ]]; then
  if [[ -f "$LVGL_CACHE_LIB" ]]; then
    ctm_prebuilt_lvgl="$LVGL_CACHE_LIB"
    echo "Reusing prebuilt LVGL: $LVGL_CACHE_LIB"
  else
    echo "No prebuilt LVGL cache yet; building LVGL once and caching it at $LVGL_CACHE_LIB" >&2
  fi
fi

cmake -S "$WORK_DIR/src" -B "$WORK_DIR/build" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCTM_PREBUILT_LVGL="$ctm_prebuilt_lvgl" \
  -DCMAKE_AR="$SDK/bin/arm-webos-linux-gnueabi-ar" \
  -DCMAKE_RANLIB="$SDK/bin/arm-webos-linux-gnueabi-ranlib"
cmake --build "$WORK_DIR/build" --target package

if [[ -f "$WORK_DIR/build/libctm_lvgl.a" ]]; then
  mkdir -p "$LVGL_CACHE_DIR"
  cp -f "$WORK_DIR/build/libctm_lvgl.a" "$LVGL_CACHE_LIB"
fi

mkdir -p "$ROOT_DIST_DIR"
find "$WORK_DIR/src/dist" -maxdepth 1 -type f -name '*.ipk' -exec cp -f {} "$ROOT_DIST_DIR/" \;

echo "IPK output:"
find "$ROOT_DIST_DIR" -maxdepth 1 -type f -name 'com.local.ctmbridge_*_arm.ipk' -print
