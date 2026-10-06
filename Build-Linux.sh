#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
Usage: Build-Linux.sh --arch <x86_64|aarch64> [--output-dir <dir>]

Builds Rocket-R in an Ubuntu 22.04 container and packages ROM-free Linux builds.
x86_64 is the correct build for Steam Deck and normal AMD64/x64 Linux PCs.
aarch64 is for ARM64 Linux devices and will not run natively on Steam Deck.
USAGE
  exit 2
}

ARCH=""
OUTPUT_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch) ARCH="${2:-}"; shift 2 ;;
    --output-dir) OUTPUT_DIR="${2:-}"; shift 2 ;;
    *) usage ;;
  esac
done
case "$ARCH" in x86_64|aarch64) ;; *) usage ;; esac

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(tr -d '\r\n' < "$PROJECT_ROOT/VERSION")"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/dist}"
mkdir -p "$OUTPUT_DIR"

for required in \
  "$PROJECT_ROOT/generated/rom_identity.generated.hpp" \
  "$PROJECT_ROOT/generated/bootstrap.generated.hpp" \
  "$PROJECT_ROOT/runtime-recomp/RecompiledFuncs/recomp_overlays.inl"; do
  [[ -f "$required" ]] || {
    echo "Missing shared generated output: $required" >&2
    echo "Run ONE-CLICK-BUILD.cmd through static recompilation first, then rerun this platform build." >&2
    exit 1
  }
done
find "$PROJECT_ROOT/runtime-recomp/RecompiledRSP" -maxdepth 1 -type f -name '*.cpp' -print -quit | grep -q . || {
  echo 'Missing shared generated RSP output. Run the One Click Builder first.' >&2
  exit 1
}

command -v rsync >/dev/null || { echo 'Missing rsync. Install it in WSL with: sudo apt-get install rsync' >&2; exit 1; }
command -v docker >/dev/null || { echo 'Missing Docker in WSL. The One Click Builder can install docker.io automatically.' >&2; exit 1; }

if ! sudo docker info >/dev/null 2>&1; then
  sudo service docker start >/dev/null 2>&1 || true
fi
sudo docker info >/dev/null 2>&1 || {
  echo 'Docker is installed but its daemon is not running in WSL.' >&2
  echo 'Try: sudo service docker start' >&2
  exit 1
}

if [[ "$ARCH" == aarch64 ]]; then
  if [[ -d /proc/sys/fs/binfmt_misc ]] && ! mountpoint -q /proc/sys/fs/binfmt_misc; then
    sudo mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc 2>/dev/null || true
  fi
  if command -v update-binfmts >/dev/null; then
    sudo update-binfmts --enable qemu-aarch64 >/dev/null 2>&1 || true
  fi
fi

WORK_BASE="${ROCKET_LINUX_WORK_ROOT:-$HOME/.cache/rocket-r/platform-builds}"
WORK="$WORK_BASE/FIXED34-$ARCH"
mkdir -p "$WORK"
rsync -a --delete \
  --exclude '.git/' --exclude 'build/' --exclude 'dist/' --exclude 'extern/' \
  --exclude '*.zip' --exclude '*.rar' --exclude '/Payload/' --exclude '/Live_Check_Windows/' \
  "$PROJECT_ROOT/" "$WORK/"
mkdir -p "$WORK/dist-container"
if [[ -d "$PROJECT_ROOT/build/sdk1-compatibility" ]]; then
  mkdir -p "$WORK/build/sdk1-compatibility"
  for fixture in Rocket_Cheat_Menu_3.0.1.nrm Rocket_Colour_Studio_1.0.0.nrm rocket_modern_camera.1.0.0.nrm; do
    [[ ! -f "$PROJECT_ROOT/build/sdk1-compatibility/$fixture" ]] || cp "$PROJECT_ROOT/build/sdk1-compatibility/$fixture" "$WORK/build/sdk1-compatibility/$fixture"
  done
fi

PLATFORM="linux/amd64"
[[ "$ARCH" == aarch64 ]] && PLATFORM="linux/arm64"
IMAGE="${ROCKET_LINUX_BUILD_IMAGE:-ubuntu:22.04}"

cat > "$WORK/.rocket-linux-container-build.sh" <<'CONTAINER'
#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential git ca-certificates curl python3 python3-pip python3-dev \
  ninja-build pkg-config libsdl2-dev libgtk-3-dev libvulkan-dev \
  squashfs-tools patchelf file xz-utils zstd
python3 -m pip install --no-cache-dir 'cmake==3.27.9' 'tomli>=2.0,<3'

cd /work
python3 scripts/bootstrap_dependencies.py --root .
python3 scripts/self_check.py --root .
python3 scripts/verify_recomp_policy_v42.py --root . --with-generated
python3 scripts/verify_shared_mode0_v43_3.py --root .
python3 scripts/verify_presentation_policy.py --root . --with-generated
python3 -m unittest discover -s tests -p 'test_*.py'
rm -rf "build/linux-${ROCKET_TARGET_ARCH}"
cmake -S . -B "build/linux-${ROCKET_TARGET_ARCH}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "build/linux-${ROCKET_TARGET_ARCH}" --target RocketR RecompModTool RocketPresentationTests RocketRuntimeLogTests RocketAndroidSupportTests RocketControlsTests RocketControlsUiTests RocketModsTests RocketCameraModTests RocketGraphicsCameraTests RocketSdkServicesTests RocketSdkRuntimeTests RocketSdkCompatibilityTests RocketSdkWorldTests RocketSdkAudioTests RocketAssetLayersTests --parallel "${ROCKET_BUILD_JOBS:-6}"
ctest --test-dir "build/linux-${ROCKET_TARGET_ARCH}" --output-on-failure
BINARY="build/linux-${ROCKET_TARGET_ARCH}/bin/Rocket-R"
if [[ ! -x "$BINARY" ]]; then
  BINARY="$(find "build/linux-${ROCKET_TARGET_ARCH}" -type f -name Rocket-R -perm -111 | head -n1)"
fi
[[ -n "$BINARY" && -x "$BINARY" ]] || { echo 'Rocket-R Linux binary was not produced.' >&2; exit 1; }
file "$BINARY"
case "$ROCKET_TARGET_ARCH" in
  x86_64) file "$BINARY" | grep -Eq 'x86-64|x86_64' ;;
  aarch64) file "$BINARY" | grep -Eq 'ARM aarch64|aarch64' ;;
esac
OUT="/work/dist-container/Rocket-R-${ROCKET_VERSION}-Linux-${ROCKET_TARGET_ARCH}.AppImage"
rm -f "$OUT"
bash ./scripts/package_appimage.sh "$BINARY" "$ROCKET_TARGET_ARCH" "$OUT"
CONTAINER
chmod +x "$WORK/.rocket-linux-container-build.sh"

sudo docker run --rm --platform "$PLATFORM" \
  -e ROCKET_TARGET_ARCH="$ARCH" -e ROCKET_VERSION="$VERSION" \
  -e ROCKET_BUILD_JOBS="${ROCKET_BUILD_JOBS:-6}" \
  -v "$WORK:/work" -w /work "$IMAGE" \
  /bin/bash /work/.rocket-linux-container-build.sh

sudo chown -R "$(id -u):$(id -g)" "$WORK/dist-container" 2>/dev/null || true
SRC="$WORK/dist-container/Rocket-R-$VERSION-Linux-$ARCH.AppImage"
APPDIR_SRC="$WORK/dist-container/Rocket-R-$VERSION-Linux-$ARCH.AppDir"
DST="$OUTPUT_DIR/Rocket-R-$VERSION-Linux-$ARCH.AppImage"
[[ -s "$SRC" ]] || { echo "Missing AppImage after container build: $SRC" >&2; exit 1; }
[[ -x "$APPDIR_SRC/AppRun" ]] || { echo "Missing validated AppDir after container build: $APPDIR_SRC" >&2; exit 1; }
[[ -x "$APPDIR_SRC/usr/bin/Rocket-R" ]] || { echo "Missing Rocket-R in validated AppDir: $APPDIR_SRC/usr/bin/Rocket-R" >&2; exit 1; }
chmod 0755 "$SRC"
cp -f "$SRC" "$DST"
chmod 0755 "$DST" 2>/dev/null || true

# Generic portable AppImage archive. Its launcher explicitly uses extract-and-run,
# so it works even when a Linux machine cannot mount AppImages through FUSE.
BUNDLE_NAME="Rocket-R-$VERSION-Linux-$ARCH-Portable"
BUNDLE_DIR="$WORK/dist-container/$BUNDLE_NAME"
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR"
cp -f "$SRC" "$BUNDLE_DIR/Rocket-R.AppImage"
chmod 0755 "$BUNDLE_DIR/Rocket-R.AppImage"
cat > "$BUNDLE_DIR/Launch-Rocket-R.sh" <<'LAUNCHER'
#!/usr/bin/env bash
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$HERE/Rocket-R.AppImage"
STATE="${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/rocket-r"
mkdir -p "$STATE" 2>/dev/null || STATE="/tmp/rocket-r-${UID:-0}"
mkdir -p "$STATE" 2>/dev/null || true
LOG="$STATE/portable-launch.log"
exec >>"$LOG" 2>&1
echo "[$(date -Is 2>/dev/null || date)] Rocket-R portable launcher"
echo "APP=$APP"
echo "ARCH=$(uname -m 2>/dev/null || true)"
chmod 0755 "$APP" 2>/dev/null || true
unset LD_PRELOAD
export APPIMAGE_EXTRACT_AND_RUN=1
exec "$APP" --appimage-extract-and-run "$@"
LAUNCHER
chmod 0755 "$BUNDLE_DIR/Launch-Rocket-R.sh"
cat > "$BUNDLE_DIR/README.txt" <<README
Rocket-R Linux portable AppImage bundle

Architecture: $ARCH

For Steam Deck use ONLY x86_64. Steam Deck is AMD64/x86-64, not ARM64.

This launcher uses AppImage extract-and-run mode, so it does not need a working
FUSE AppImage mount. It writes a log to:
  ~/.local/state/rocket-r/portable-launch.log

Launch:
  ./Launch-Rocket-R.sh
README
TAR="$OUTPUT_DIR/Rocket-R-$VERSION-Linux-$ARCH-Portable.tar.gz"
rm -f "$TAR"
tar -C "$WORK/dist-container" -czf "$TAR" "$BUNDLE_NAME"

STEAMDECK_TAR=""
if [[ "$ARCH" == x86_64 ]]; then
  # Steam Deck release: no AppImage runtime at launch time at all. Package the
  # validated AppDir itself and execute AppRun directly. This bypasses FUSE,
  # AppImage integration tools and AppImage execute-bit handling completely.
  DECK_NAME="Rocket-R-$VERSION-SteamDeck-x86_64"
  DECK_DIR="$WORK/dist-container/$DECK_NAME"
  rm -rf "$DECK_DIR"
  mkdir -p "$DECK_DIR"
  cp -a "$APPDIR_SRC" "$DECK_DIR/Rocket-R.AppDir"
  chmod 0755 "$DECK_DIR/Rocket-R.AppDir/AppRun" "$DECK_DIR/Rocket-R.AppDir/usr/bin/Rocket-R"

  cat > "$DECK_DIR/START-ROCKET-R.sh" <<'DECKLAUNCH'
#!/usr/bin/env bash
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPDIR_PATH="$HERE/Rocket-R.AppDir"
STATE="${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/rocket-r"
mkdir -p "$STATE" 2>/dev/null || STATE="/tmp/rocket-r-${UID:-0}"
mkdir -p "$STATE" 2>/dev/null || true
LOG="$STATE/steamdeck-launch.log"
exec >>"$LOG" 2>&1

echo "============================================================"
echo "[$(date -Is 2>/dev/null || date)] Rocket-R Steam Deck launch"
echo "HERE=$HERE"
echo "ARCH=$(uname -m 2>/dev/null || true)"
echo "OS=$(grep '^PRETTY_NAME=' /etc/os-release 2>/dev/null || true)"
echo "DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}"
echo "LD_PRELOAD(before)=${LD_PRELOAD:-}"

if [[ "$(uname -m 2>/dev/null || true)" != "x86_64" ]]; then
  echo "ERROR: This Steam Deck package requires x86_64."
  exit 126
fi
if [[ ! -x "$APPDIR_PATH/AppRun" ]]; then
  echo "ERROR: Missing executable $APPDIR_PATH/AppRun"
  exit 126
fi

# Steam/non-Steam shortcuts may inject overlay preload libraries. Rocket-R does
# not require them, and clearing them makes the portable native launch deterministic.
unset LD_PRELOAD
unset APPIMAGE APPDIR APPIMAGE_EXTRACT_AND_RUN
export APPDIR="$APPDIR_PATH"

echo "Launching AppDir directly (FUSE/AppImage runtime bypassed)."
"$APPDIR_PATH/AppRun" "$@"
rc=$?
echo "Rocket-R returned exit code $rc"
if [[ $rc -ne 0 ]]; then
  msg="Rocket-R failed to launch (exit $rc). Log: $LOG"
  if command -v kdialog >/dev/null 2>&1; then
    kdialog --error "$msg" --title "Rocket-R" >/dev/null 2>&1 || true
  elif command -v zenity >/dev/null 2>&1; then
    zenity --error --title="Rocket-R" --text="$msg" >/dev/null 2>&1 || true
  elif command -v notify-send >/dev/null 2>&1; then
    notify-send "Rocket-R failed to launch" "See $LOG" >/dev/null 2>&1 || true
  fi
fi
exit "$rc"
DECKLAUNCH
  chmod 0755 "$DECK_DIR/START-ROCKET-R.sh"

  cat > "$DECK_DIR/INSTALL-MENU-SHORTCUT.sh" <<'INSTALLER'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$HOME/.local/share/applications/rocket-r-steamdeck.desktop"
mkdir -p "$(dirname "$TARGET")"
ICON="$HERE/Rocket-R.AppDir/rocket-r.png"
LAUNCHER="$HERE/START-ROCKET-R.sh"
cat > "$TARGET" <<EOF_DESKTOP
[Desktop Entry]
Type=Application
Name=Rocket-R
Comment=Rocket: Robot on Wheels - Recompiled
Exec=$LAUNCHER
Icon=$ICON
Terminal=false
Categories=Game;
StartupNotify=true
EOF_DESKTOP
chmod 0644 "$TARGET"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
echo "Installed Rocket-R menu shortcut: $TARGET"
echo "Launch log: ~/.local/state/rocket-r/steamdeck-launch.log"
INSTALLER
  chmod 0755 "$DECK_DIR/INSTALL-MENU-SHORTCUT.sh"

  cat > "$DECK_DIR/README-STEAMDECK.txt" <<'DECKREADME'
Rocket-R - Steam Deck x86_64 bundle
====================================

This bundle deliberately does NOT mount or launch an AppImage.
It runs the validated Rocket-R AppDir directly, so FUSE is not involved.

1. Extract this .tar.gz ON THE STEAM DECK (not on Windows).
2. In Desktop Mode, open this folder.
3. Run START-ROCKET-R.sh.

Optional:
Run INSTALL-MENU-SHORTCUT.sh once to add Rocket-R to the KDE application menu.
You can then add that menu entry to Steam as a non-Steam game.

Every launch writes:
  ~/.local/state/rocket-r/steamdeck-launch.log

The normal AppImage is still produced separately for standard Linux systems.
DECKREADME

  STEAMDECK_TAR="$OUTPUT_DIR/Rocket-R-$VERSION-Linux-x86_64-SteamDeck.tar.gz"
  rm -f "$STEAMDECK_TAR"
  tar -C "$WORK/dist-container" -czf "$STEAMDECK_TAR" "$DECK_NAME"
fi

echo "Linux $ARCH build complete: $DST"
echo "Linux $ARCH portable archive: $TAR"
if [[ -n "$STEAMDECK_TAR" ]]; then
  echo "Steam Deck FUSE-free archive: $STEAMDECK_TAR"
  sha256sum "$DST" "$TAR" "$STEAMDECK_TAR"
else
  sha256sum "$DST" "$TAR"
fi
