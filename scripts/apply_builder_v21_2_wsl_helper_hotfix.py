from __future__ import annotations

from pathlib import Path
from datetime import datetime
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "scripts" / "OneClickBuild.ps1"
HELPER = ROOT / "scripts" / "bootstrap_wsl_packages.sh"
MARKER_21_1 = "# BUILDER v21.1: resilient Ubuntu/WSL package bootstrap"
MARKER_21_2 = "# BUILDER v21.2: direct WSL package bootstrap helper"
NEXT_STAGE = "    Banner '3/9 - Pinned source dependencies'"
OLD_ONESHOT = '    Invoke-WslBash "sudo -v && sudo apt-get update && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y $WslPackages"\n'

NEW_BLOCK = r'''    # BUILDER v21.2: direct WSL package bootstrap helper
    # Keep multi-line Bash out of PowerShell/native-process command strings.
    # The helper owns the package list, sudo and all apt quoting.
    $WslBootstrapWindows = Join-Path $Root 'scripts\bootstrap_wsl_packages.sh'
    if (-not (Test-Path $WslBootstrapWindows)) {
        throw "Missing WSL package bootstrap helper: $WslBootstrapWindows"
    }
    $WslBootstrapScript = "$WslRoot/scripts/bootstrap_wsl_packages.sh"
    if ($NeedLinuxPackages) {
        & wsl.exe -d $script:WslDistro -- bash $WslBootstrapScript --need-linux-packages
    } else {
        & wsl.exe -d $script:WslDistro -- bash $WslBootstrapScript
    }
    if ($LASTEXITCODE -ne 0) {
        throw "WSL package bootstrap helper failed with exit $LASTEXITCODE in $script:WslDistro."
    }

'''


def fail(message: str) -> None:
    print(f"[ERROR] {message}")
    raise SystemExit(1)


def normalize_newlines(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def locate_v211_block(text: str) -> tuple[int, int] | None:
    start = text.find("    " + MARKER_21_1)
    if start < 0:
        return None
    end = text.find(NEXT_STAGE, start)
    if end < 0:
        fail("Found v21.1 bootstrap marker but could not locate Stage 3 boundary. Refusing an ambiguous edit.")
    return start, end


def main() -> int:
    if not TARGET.is_file():
        fail(f"Missing OneClick builder: {TARGET}")
    if not HELPER.is_file():
        fail(f"Missing packaged WSL helper: {HELPER}")

    helper_text = normalize_newlines(HELPER.read_text(encoding="utf-8"))
    helper_required = [
        "set -euo pipefail",
        "base_packages=(",
        "--need-linux-packages",
        "dpkg-query -W -f='${Status}'",
        'missing_packages+=("$package")',
        "Required Rocket-R WSL packages are already installed; skipping package-index refresh.",
        "Acquire::Retries=3",
        'install -y "${missing_packages[@]}"',
    ]
    for token in helper_required:
        if token not in helper_text:
            fail(f"Packaged WSL helper is missing verification token: {token}")

    text = normalize_newlines(TARGET.read_text(encoding="utf-8-sig"))

    if MARKER_21_2 in text:
        print("[OK] Builder v21.2 direct WSL helper bootstrap is already installed.")
    else:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        backup_dir = ROOT / "build" / "repair-backups" / f"builder-v21.2-wsl-helper-{stamp}"
        backup_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(TARGET, backup_dir / "OneClickBuild.ps1")
        print(f"[OK] Backup: {backup_dir}")

        block = locate_v211_block(text)
        if block is not None:
            start, end = block
            text = text[:start] + NEW_BLOCK + text[end:]
            print("[OK] Removed v21.1 inline multi-line bash -lc bootstrap.")
        else:
            count = text.count(OLD_ONESHOT)
            if count != 1:
                fail(
                    "Could not find either the active v21.1 bootstrap block or exactly one FIXED34 one-shot apt line. "
                    f"Found old one-shot count={count}."
                )
            text = text.replace(OLD_ONESHOT, NEW_BLOCK, 1)
            print("[OK] Replaced FIXED34 one-shot apt bootstrap with direct WSL helper invocation.")

        TARGET.write_text(text, encoding="utf-8-sig", newline="\r\n")

    verify = normalize_newlines(TARGET.read_text(encoding="utf-8-sig"))
    required = [
        MARKER_21_2,
        "scripts\\bootstrap_wsl_packages.sh",
        '$WslBootstrapScript = "$WslRoot/scripts/bootstrap_wsl_packages.sh"',
        "& wsl.exe -d $script:WslDistro -- bash $WslBootstrapScript --need-linux-packages",
        "& wsl.exe -d $script:WslDistro -- bash $WslBootstrapScript",
        "WSL package bootstrap helper failed with exit $LASTEXITCODE",
    ]
    for token in required:
        if token not in verify:
            fail(f"Builder verification token missing after patch: {token}")

    if MARKER_21_1 in verify:
        fail("The broken v21.1 inline bootstrap is still active.")
    if "$WslAptBootstrap" in verify:
        fail("A stale inline WSL apt bootstrap variable remains active.")
    if OLD_ONESHOT.strip() in verify:
        fail("The original one-shot apt bootstrap is still active.")

    # Do not touch the graphics-generation pipeline.
    for hook in (
        "patch_popin_diagnostics_generated.py",
        "patch_render_queue_expansion_v21_generated.py",
    ):
        if (ROOT / "scripts" / hook).exists() and hook not in verify:
            fail(f"Existing generated-renderer hook disappeared from OneClickBuild.ps1: {hook}")

    print("[OK] v21/v20 generated renderer hooks preserved.")
    print("[OK] No multi-line bash program is transported through Invoke-WslBash/bash -lc.")
    print("[OK] Builder v21.2 direct WSL helper verification PASS.")

    self_check = ROOT / "scripts" / "self_check.py"
    if self_check.is_file():
        result = subprocess.run([sys.executable, str(self_check)], cwd=str(ROOT))
        if result.returncode != 0:
            fail(f"Rocket-R source self-check failed after builder-only patch (exit {result.returncode}).")
        print("[OK] Rocket-R source self-check PASS after builder-only patch.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
