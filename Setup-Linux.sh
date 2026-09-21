#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "Rocket-R Linux/AppImage helper prerequisite setup"
if command -v apt-get >/dev/null 2>&1; then
  echo "Installing Debian/Ubuntu helper prerequisites (sudo may prompt)..."
  sudo apt-get update
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    git python3 python3-tomli rsync docker.io qemu-user-static binfmt-support file
  sudo service docker start >/dev/null 2>&1 || true
  if command -v update-binfmts >/dev/null 2>&1; then
    sudo update-binfmts --enable qemu-aarch64 >/dev/null 2>&1 || true
  fi
else
  echo "Automatic package installation is only implemented for apt-based systems."
  echo "Install: Git, Python 3, rsync, Docker, qemu-user-static/binfmt support and file."
fi
python3 "$ROOT/scripts/self_check.py" --root "$ROOT"
echo
echo "Linux helper prerequisites are ready."
echo "Build-Linux.sh consumes the shared generated CPU/RSP output produced by the One Click Builder."
echo "Examples:"
echo "  ./Build-Linux.sh --arch x86_64"
echo "  ./Build-Linux.sh --arch aarch64"
