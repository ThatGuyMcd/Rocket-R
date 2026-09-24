from __future__ import annotations

from pathlib import Path
from datetime import datetime
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "scripts" / "OneClickBuild.ps1"
MARKER = "# BUILDER v21.1: resilient Ubuntu/WSL package bootstrap"
OLD = '    Invoke-WslBash "sudo -v && sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y $WslPackages"\n'
NEW = r'''    # BUILDER v21.1: resilient Ubuntu/WSL package bootstrap
    # Do not hit Ubuntu mirrors on every build when all prerequisites are already installed.
    # If packages are missing, tolerate transient mirror synchronization / stale index failures.
    $WslAptBootstrap = @'
set -e
sudo -v
packages="__ROCKET_WSL_PACKAGES__"
missing_packages=""
for package in $packages; do
    if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'install ok installed'; then
        missing_packages="$missing_packages $package"
    fi
done

if [ -z "$missing_packages" ]; then
    echo "[apt] Required Rocket-R WSL packages are already installed; skipping package-index refresh."
    exit 0
fi

echo "[apt] Missing Rocket-R WSL packages:$missing_packages"
apt_update_ok=0
for attempt in 1 2 3 4 5; do
    echo "[apt] Package-index refresh attempt $attempt/5..."
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

sudo DEBIAN_FRONTEND=noninteractive apt-get \
    -o Acquire::Retries=3 \
    install -y $missing_packages
'@
    $WslAptBootstrap = $WslAptBootstrap.Replace('__ROCKET_WSL_PACKAGES__', $WslPackages)
    Invoke-WslBash $WslAptBootstrap
'''


def fail(message: str) -> None:
    print(f"[ERROR] {message}")
    raise SystemExit(1)


def main() -> int:
    if not TARGET.is_file():
        fail(f"Missing OneClick builder: {TARGET}")

    text = TARGET.read_text(encoding="utf-8-sig")

    if MARKER in text:
        print("[OK] Builder v21.1 resilient WSL/APT bootstrap is already installed.")
    else:
        count = text.count(OLD)
        if count != 1:
            fail(f"Expected exactly one FIXED34 WSL apt bootstrap line, found {count}. Refusing an ambiguous edit.")

        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        backup_dir = ROOT / "build" / "repair-backups" / f"builder-v21.1-apt-hotfix-{stamp}"
        backup_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(TARGET, backup_dir / "OneClickBuild.ps1")
        print(f"[OK] Backup: {backup_dir}")

        text = text.replace(OLD, NEW, 1)
        TARGET.write_text(text, encoding="utf-8-sig", newline="\r\n")
        print("[OK] Replaced one-shot apt bootstrap with installed-package fast path + mirror-sync retries.")

    verify = TARGET.read_text(encoding="utf-8-sig")
    required = [
        MARKER,
        "dpkg-query -W -f='${Status}'",
        "Required Rocket-R WSL packages are already installed; skipping package-index refresh.",
        "Acquire::Retries=3",
        "Acquire::http::No-Cache=true",
        "Forcing a clean package-index refresh",
        "Invoke-WslBash $WslAptBootstrap",
    ]
    for token in required:
        if token not in verify:
            fail(f"Verification token missing after patch: {token}")

    if OLD.strip() in verify:
        fail("The old one-shot apt bootstrap is still active.")

    # Preserve the current graphics-generation pipeline when present.
    v21_hook = "patch_render_queue_expansion_v21_generated.py"
    if (ROOT / "scripts" / v21_hook).exists() and v21_hook not in verify:
        fail("v21 generated render-queue hook disappeared from OneClickBuild.ps1.")

    v20_hook = "patch_popin_diagnostics_generated.py"
    if (ROOT / "scripts" / v20_hook).exists() and v20_hook not in verify:
        fail("v20 generated diagnostics hook disappeared from OneClickBuild.ps1.")

    print("[OK] v21/v20 generated renderer hooks preserved.")
    print("[OK] Builder v21.1 WSL/APT mirror-sync hotfix verification PASS.")

    self_check = ROOT / "scripts" / "self_check.py"
    if self_check.is_file():
        result = subprocess.run([sys.executable, str(self_check)], cwd=str(ROOT))
        if result.returncode != 0:
            fail(f"Rocket-R source self-check failed after builder patch (exit {result.returncode}).")
        print("[OK] Rocket-R source self-check PASS after builder-only patch.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
