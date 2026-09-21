#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 <binary> <x86_64|aarch64> <output.AppImage>" >&2
  exit 2
}

[[ $# -eq 3 ]] || usage
BINARY="$(readlink -f "$1")"
ARCH="$2"
OUTPUT="$3"
[[ -x "$BINARY" ]] || { echo "Missing Linux runtime: $BINARY" >&2; exit 1; }
case "$ARCH" in x86_64|aarch64) ;; *) usage ;; esac

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPDIR="${OUTPUT%.AppImage}.AppDir"
rm -rf "$APPDIR" "${OUTPUT}.squashfs"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/scalable/apps" "$APPDIR/usr/share/doc/rocket-r"
install -m 0755 "$BINARY" "$APPDIR/usr/bin/Rocket-R"
install -m 0644 "$PROJECT_ROOT/packaging/linux/rocket-r.desktop" "$APPDIR/rocket-r.desktop"
install -m 0644 "$PROJECT_ROOT/packaging/linux/rocket-r.desktop" "$APPDIR/usr/share/applications/rocket-r.desktop"
install -m 0644 "$PROJECT_ROOT/packaging/linux/rocket-r.svg" "$APPDIR/rocket-r.svg"
install -m 0644 "$PROJECT_ROOT/packaging/linux/rocket-r.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/rocket-r.svg"
ln -sfn rocket-r.svg "$APPDIR/.DirIcon"
install -m 0644 "$PROJECT_ROOT/LICENSE.md" "$APPDIR/usr/share/doc/rocket-r/LICENSE.md"
install -m 0644 "$PROJECT_ROOT/THIRD_PARTY.md" "$APPDIR/usr/share/doc/rocket-r/THIRD_PARTY.md"
install -m 0644 "$PROJECT_ROOT/README.md" "$APPDIR/usr/share/doc/rocket-r/README.md"
install -m 0644 "$PROJECT_ROOT/docs/STATUS.md" "$APPDIR/usr/share/doc/rocket-r/STATUS.md"

cat > "$APPDIR/AppRun" <<'RUNEOF'
#!/usr/bin/env bash
set -uo pipefail
HERE="${APPDIR:-$(cd "$(dirname "$0")" && pwd)}"
STATE_ROOT="${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/rocket-r"
mkdir -p "$STATE_ROOT" 2>/dev/null || STATE_ROOT="/tmp/rocket-r-${UID:-0}"
mkdir -p "$STATE_ROOT" 2>/dev/null || true
LOG="$STATE_ROOT/appimage-launch.log"
{
  echo "[$(date -Is 2>/dev/null || date)] Rocket-R AppRun"
  echo "APPDIR=$HERE"
  echo "ARCH=$(uname -m 2>/dev/null || true)"
  echo "PWD=$(pwd 2>/dev/null || true)"
  echo "STEAM_GAME_ID=${SteamGameId:-${SteamAppId:-}}"
  echo "LD_PRELOAD(before)=${LD_PRELOAD:-}"
} >>"$LOG" 2>&1 || true

# Steam can inject overlay libraries through LD_PRELOAD. Those are not required
# by Rocket-R and can prevent portable Linux binaries/AppImages from starting.
unset LD_PRELOAD
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
{
  echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
  echo "Launching $HERE/usr/bin/Rocket-R"
} >>"$LOG" 2>&1 || true

"$HERE/usr/bin/Rocket-R" "$@" >>"$LOG" 2>&1
rc=$?
echo "Rocket-R exit code: $rc" >>"$LOG" 2>&1 || true
exit "$rc"
RUNEOF
chmod 0755 "$APPDIR/AppRun"
ln -sfn rocket-r.desktop "$APPDIR/Rocket-R.desktop"

mapfile -t libs < <(ldd "$BINARY" | awk '
  /=> \/.*\(/ {print $3}
  /^\// {print $1}
' | sort -u)
for lib in "${libs[@]}"; do
  base="$(basename "$lib")"
  case "$base" in
    libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libnss_*.so.*|ld-linux*.so.*|libvulkan.so.*) continue ;;
    # Keep the host SteamOS/Mesa display/graphics stack together. Bundling these
    # Ubuntu copies ahead of the host libraries can break Vulkan driver loading.
    libX11.so.*|libX11-xcb.so.*|libXext.so.*|libXau.so.*|libXdmcp.so.*|libxcb*.so.*|libwayland*.so.*|libdrm*.so.*|libgbm.so.*|libGL.so.*|libGLX.so.*|libEGL.so.*|libGLES*.so.*|libOpenGL.so.*|libglapi.so.*|libxkbcommon*.so.*) continue ;;
  esac
  [[ -f "$lib" ]] || continue
  cp -L "$lib" "$APPDIR/usr/lib/$base"
done

python3 "$PROJECT_ROOT/scripts/scan_release.py" "$APPDIR"

# Pin the modern static type-2 runtime. It does not require host libfuse2, but
# normal AppImage execution still uses a FUSE mount. The separate Steam Deck
# bundle emitted by Build-Linux.sh runs this AppDir directly and needs no FUSE.
RUNTIME_TAG="20251108"
RUNTIME_CACHE="${ROCKET_APPIMAGE_RUNTIME_CACHE:-$HOME/.cache/rocket-r/appimage-runtime}"
mkdir -p "$RUNTIME_CACHE"
RUNTIME="$RUNTIME_CACHE/runtime-$RUNTIME_TAG-$ARCH"
case "$ARCH" in
  x86_64) RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d" ;;
  aarch64) RUNTIME_SHA256="00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444" ;;
esac
if [[ ! -s "$RUNTIME" ]] || ! echo "$RUNTIME_SHA256  $RUNTIME" | sha256sum -c - >/dev/null 2>&1; then
  rm -f "$RUNTIME"
  url="https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_TAG/runtime-$ARCH"
  echo "Downloading pinned AppImage $ARCH runtime $RUNTIME_TAG..."
  curl -fL --retry 4 --retry-delay 2 "$url" -o "$RUNTIME"
fi
echo "$RUNTIME_SHA256  $RUNTIME" | sha256sum -c -
chmod 0755 "$RUNTIME"
case "$ARCH" in
  x86_64) file "$RUNTIME" | grep -Eq 'x86-64|x86_64' || { echo 'Downloaded AppImage runtime is not x86_64.' >&2; exit 1; } ;;
  aarch64) file "$RUNTIME" | grep -Eq 'ARM aarch64|aarch64' || { echo 'Downloaded AppImage runtime is not aarch64.' >&2; exit 1; } ;;
esac

mkdir -p "$(dirname "$OUTPUT")"
mksquashfs "$APPDIR" "${OUTPUT}.squashfs" -root-owned -noappend -comp zstd >/dev/null
cat "$RUNTIME" "${OUTPUT}.squashfs" > "$OUTPUT"
chmod 0755 "$OUTPUT"
rm -f "${OUTPUT}.squashfs"

python3 - "$OUTPUT" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
data = p.read_bytes()[:16]
if len(data) < 11 or data[8:11] != b"AI\x02":
    raise SystemExit(f"Invalid AppImage type-2 header: {p}")
PY

# Validate the exact embedded runtime/payload combination without requiring FUSE.
VERIFY_DIR="$(mktemp -d)"
cleanup_verify() { rm -rf "$VERIFY_DIR"; }
trap cleanup_verify EXIT
(
  cd "$VERIFY_DIR"
  "$OUTPUT" --appimage-extract >/dev/null
)
[[ -x "$VERIFY_DIR/squashfs-root/AppRun" ]] || { echo 'AppImage extraction validation did not produce executable AppRun.' >&2; exit 1; }
[[ -x "$VERIFY_DIR/squashfs-root/usr/bin/Rocket-R" ]] || { echo 'AppImage extraction validation did not produce executable Rocket-R.' >&2; exit 1; }
case "$ARCH" in
  x86_64) file "$VERIFY_DIR/squashfs-root/usr/bin/Rocket-R" | grep -Eq 'x86-64|x86_64' ;;
  aarch64) file "$VERIFY_DIR/squashfs-root/usr/bin/Rocket-R" | grep -Eq 'ARM aarch64|aarch64' ;;
esac
trap - EXIT
cleanup_verify

echo "Created and extraction-validated $OUTPUT"
sha256sum "$OUTPUT"
