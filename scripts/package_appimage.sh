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
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/Rocket-R" "$@"
RUNEOF
chmod 0755 "$APPDIR/AppRun"
ln -sfn rocket-r.desktop "$APPDIR/Rocket-R.desktop"

# Bundle user-space dependencies while deliberately leaving glibc, the dynamic
# loader and Vulkan loader/driver stack to the target distribution. This keeps
# the AppImage portable across GPU vendors and avoids shipping host graphics
# drivers from the build container.
mapfile -t libs < <(ldd "$BINARY" | awk '
  /=> \/.*\(/ {print $3}
  /^\// {print $1}
' | sort -u)
for lib in "${libs[@]}"; do
  base="$(basename "$lib")"
  case "$base" in
    libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libnss_*.so.*|ld-linux*.so.*|libvulkan.so.*) continue ;;
  esac
  [[ -f "$lib" ]] || continue
  cp -L "$lib" "$APPDIR/usr/lib/$base"
done

python3 "$PROJECT_ROOT/scripts/scan_release.py" "$APPDIR"

RUNTIME_CACHE="${ROCKET_APPIMAGE_RUNTIME_CACHE:-$HOME/.cache/rocket-r/appimage-runtime}"
mkdir -p "$RUNTIME_CACHE"
RUNTIME="$RUNTIME_CACHE/runtime-$ARCH"
if [[ ! -s "$RUNTIME" ]]; then
  url="https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH"
  echo "Downloading AppImage $ARCH runtime..."
  curl -fL --retry 4 --retry-delay 2 "$url" -o "$RUNTIME"
fi
chmod 0755 "$RUNTIME"
case "$ARCH" in
  x86_64) file "$RUNTIME" | grep -Eq 'x86-64|x86_64' || { echo 'Downloaded AppImage runtime is not x86_64.' >&2; exit 1; } ;;
  aarch64) file "$RUNTIME" | grep -Eq 'ARM aarch64|aarch64' || { echo 'Downloaded AppImage runtime is not aarch64.' >&2; exit 1; } ;;
esac

mkdir -p "$(dirname "$OUTPUT")"
mksquashfs "$APPDIR" "${OUTPUT}.squashfs" -root-owned -noappend -comp xz >/dev/null
cat "$RUNTIME" "${OUTPUT}.squashfs" > "$OUTPUT"
chmod 0755 "$OUTPUT"
rm -f "${OUTPUT}.squashfs"

# Verify the type-2 marker without trying to execute a foreign-architecture
# AppImage on the build host.
python3 - "$OUTPUT" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
data = p.read_bytes()[:16]
if len(data) < 11 or data[8:11] != b"AI\x02":
    raise SystemExit(f"Invalid AppImage type-2 header: {p}")
PY
echo "Created $OUTPUT"
sha256sum "$OUTPUT"
