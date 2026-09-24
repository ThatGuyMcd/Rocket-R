#!/usr/bin/env bash
set -euo pipefail

base_packages=(
    build-essential git python3 python3-venv python3-pip python3-dev
    libyaml-dev pkg-config make wget curl tar file libc6-i386
    gcc-mips-linux-gnu binutils-mips-linux-gnu
)
linux_platform_packages=(rsync docker.io qemu-user-static binfmt-support)

packages=("${base_packages[@]}")
if [ "$#" -gt 1 ]; then
    echo "[apt] ERROR: unexpected WSL bootstrap arguments: $*"
    exit 2
fi
if [ "$#" -eq 1 ]; then
    if [ "$1" != "--need-linux-packages" ]; then
        echo "[apt] ERROR: unknown WSL bootstrap argument: $1"
        exit 2
    fi
    packages+=("${linux_platform_packages[@]}")
fi

sudo -v

missing_packages=()
for package in "${packages[@]}"; do
    if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q '^install ok installed$'; then
        missing_packages+=("$package")
    fi
done

if [ "${#missing_packages[@]}" -eq 0 ]; then
    echo "[apt] Required Rocket-R WSL packages are already installed; skipping package-index refresh."
    exit 0
fi

echo "[apt] Missing Rocket-R WSL packages: ${missing_packages[*]}"
apt_update_ok=0
for attempt in 1 2 3 4 5; do
    echo "[apt] Package-index refresh attempt ${attempt}/5..."
    if sudo apt-get \
        -o Acquire::Retries=3 \
        -o Acquire::http::No-Cache=true \
        -o Acquire::https::No-Cache=true \
        update; then
        apt_update_ok=1
        break
    fi

    echo "[apt] Ubuntu mirror/index refresh failed; clearing partial indexes before retry."
    sudo rm -rf /var/lib/apt/lists/partial/*

    if [ "$attempt" -eq 2 ] || [ "$attempt" -eq 4 ]; then
        echo "[apt] Forcing a clean package-index refresh after repeated mirror/index failures."
        sudo rm -rf /var/lib/apt/lists/*
        sudo mkdir -p /var/lib/apt/lists/partial
    fi

    sleep $((attempt * 2))
done

if [ "$apt_update_ok" -ne 1 ]; then
    echo "[apt] ERROR: Ubuntu package indexes still failed after 5 clean/retry attempts."
    echo "[apt] This is normally a remote mirror synchronization problem, not a Rocket-R source failure."
    exit 100
fi

sudo env DEBIAN_FRONTEND=noninteractive apt-get \
    -o Acquire::Retries=3 \
    install -y "${missing_packages[@]}"
