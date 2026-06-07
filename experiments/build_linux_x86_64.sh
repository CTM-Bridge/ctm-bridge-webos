#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIST_DIR="$(cd "$SOURCE_DIR/.." && pwd)/dist"
BUILD_DIR="${BUILD_DIR:-$SOURCE_DIR/build/linux-x86_64}"
BUNDLE_DIR="$ROOT_DIST_DIR/ctm-bridge-test-linux-x86_64"

for tool in cmake; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required tool: $tool" >&2
    exit 1
  fi
done

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target ctm_bridge_test

rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/bin"
cp "$BUILD_DIR/ctm_bridge_test" "$BUNDLE_DIR/bin/"

cat > "$BUNDLE_DIR/run.sh" <<'RUNSH'
#!/usr/bin/env bash
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$APP_DIR/bin/ctm_bridge_test" "$@"
RUNSH
chmod +x "$BUNDLE_DIR/run.sh"

echo "Linux bundle output:"
echo "$BUNDLE_DIR"
