#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
Usage: Build-Linux.sh --arch <x86_64|aarch64> [--output-dir <dir>]

Builds Rocket-R in an Ubuntu 22.04 container and packages a ROM-free AppImage.
The aarch64 build uses Docker's ARM64/QEMU execution path, so it is a genuine
ARM64 binary rather than an x86 binary inside an ARM-labelled package.
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

# Platform builds intentionally consume the exact same generated CPU/RSP output
# produced once by the One Click Builder. This is what keeps all four outputs
# on one game/runtime revision instead of recompiling different ROM-derived code.
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
# Preserve downloaded extern/ and per-platform build caches between runs, but
# refresh every source/generated file from the exact Windows source tree.
rsync -a --delete \
  --exclude '.git/' --exclude 'build/' --exclude 'dist/' --exclude 'extern/' \
  "$PROJECT_ROOT/" "$WORK/"
mkdir -p "$WORK/dist-container"

PLATFORM="linux/amd64"
[[ "$ARCH" == aarch64 ]] && PLATFORM="linux/arm64"
IMAGE="ubuntu:22.04"

cat > "$WORK/.rocket-linux-container-build.sh" <<'CONTAINER'
#!/usr/bin/env bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential git ca-certificates curl python3 python3-pip python3-dev \
  ninja-build pkg-config libsdl2-dev libgtk-3-dev libvulkan-dev \
  squashfs-tools patchelf file xz-utils
python3 -m pip install --no-cache-dir 'cmake==3.27.9' 'tomli>=2.0,<3'

cd /work
python3 scripts/bootstrap_dependencies.py --root .
python3 scripts/self_check.py --root .
rm -rf "build/linux-${ROCKET_TARGET_ARCH}"
cmake -S . -B "build/linux-${ROCKET_TARGET_ARCH}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "build/linux-${ROCKET_TARGET_ARCH}" --target RocketR --parallel
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

# Use a native userspace for each target. On an x86_64 WSL host, Docker/QEMU
# transparently executes the ARM64 Ubuntu image for the aarch64 selection.
sudo docker run --rm --platform "$PLATFORM" \
  -e ROCKET_TARGET_ARCH="$ARCH" -e ROCKET_VERSION="$VERSION" \
  -v "$WORK:/work" -w /work "$IMAGE" \
  /bin/bash /work/.rocket-linux-container-build.sh

sudo chown -R "$(id -u):$(id -g)" "$WORK/dist-container" 2>/dev/null || true
SRC="$WORK/dist-container/Rocket-R-$VERSION-Linux-$ARCH.AppImage"
DST="$OUTPUT_DIR/Rocket-R-$VERSION-Linux-$ARCH.AppImage"
[[ -s "$SRC" ]] || { echo "Missing AppImage after container build: $SRC" >&2; exit 1; }
cp -f "$SRC" "$DST"
chmod +x "$DST"
echo "Linux $ARCH build complete: $DST"
sha256sum "$DST"
