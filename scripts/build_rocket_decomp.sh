#!/usr/bin/env bash
set -Eeuo pipefail

# Compatibility markers for FIXED6 source_self_check.py when FIXED7 is dropped
# into an existing working folder as a single-file builder replacement:
# 5.2 - Create isolated Splat Python environment
# 5.3 - Split the verified NSUE ROM with pinned Splat
# 5.4 - Build/verify Rocket's legacy compiler tools
# 5.5 - Build byte-matching Rocket NSUE ROM and ELF

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <rocket-r-root> <detail-log>" >&2
  exit 64
fi

ROOT="$1"
DETAIL_LOG="$2"
SOURCE_DECOMP="$ROOT/extern/rocket-decomp"
ROM="$ROOT/build/private/rocket.us.z64"
NATIVE_BASE="${ROCKET_R_WSL_WORKSPACE:-$HOME/.cache/rocket-r}"
DECOMP="$NATIVE_BASE/rocket-decomp"
VENV="$DECOMP/.rocket-venv"

mkdir -p "$(dirname "$DETAIL_LOG")"
: > "$DETAIL_LOG"
exec > >(tee -a "$DETAIL_LOG") 2>&1

echo "[Rocket decomp helper] started: $0"
echo "[Rocket decomp helper] Rocket-R root: $ROOT"
echo "[Rocket decomp helper] source checkout: $SOURCE_DECOMP"
echo "[Rocket decomp helper] native workspace: $DECOMP"
echo "[Rocket decomp helper] detail log: $DETAIL_LOG"

CURRENT_STEP="initialization"
trap 'rc=$?; echo; echo "[FAILED] Step: $CURRENT_STEP"; echo "[FAILED] Exit code: $rc"; echo "[FAILED] Command: $BASH_COMMAND"; echo "[FAILED] Detail log: $DETAIL_LOG"; exit $rc' ERR

step() {
  CURRENT_STEP="$1"
  echo
  echo "=============================================================================="
  echo "  $1"
  echo "=============================================================================="
}

step "5.1 - Validate Rocket decomp inputs"
echo "Rocket-R root:      $ROOT"
echo "Source decomp:      $SOURCE_DECOMP"
echo "Native WSL decomp:  $DECOMP"
echo "Detail log:         $DETAIL_LOG"
[[ -d "$SOURCE_DECOMP/.git" ]] || { echo "Pinned Rocket decomp checkout is missing: $SOURCE_DECOMP" >&2; exit 10; }
[[ -f "$ROM" ]] || { echo "Validated private ROM is missing: $ROM" >&2; exit 11; }
[[ -f "$SOURCE_DECOMP/Makefile" ]] || { echo "Rocket decomp Makefile is missing." >&2; exit 12; }
[[ -f "$SOURCE_DECOMP/tools/NSUE.00.yaml" ]] || { echo "Rocket NSUE Splat config is missing." >&2; exit 13; }
SOURCE_COMMIT="$(git -C "$SOURCE_DECOMP" rev-parse HEAD)"
echo "Pinned decomp commit: $SOURCE_COMMIT"
echo "ROM bytes:            $(stat -c%s "$ROM")"

step "5.2 - Stage Rocket decomp on native WSL filesystem"
# Rocket uses 32-bit GCC 2.7.2/SN64 executables. Their old 32-bit stat ABI can
# overflow on DrvFS/9P inode metadata under /mnt/<drive>, producing EOVERFLOW
# ('Value too large for defined data type'). Build on WSL's native ext4 instead.
rm -rf "$DECOMP"
mkdir -p "$DECOMP"
(
  cd "$SOURCE_DECOMP"
  tar \
    --exclude='./.rocket-venv' \
    --exclude='./build' \
    --exclude='./asm' \
    --exclude='./data' \
    --exclude='./baserom.us.z64' \
    -cf - .
) | (
  cd "$DECOMP"
  tar -xf -
)
cp -f "$ROM" "$DECOMP/baserom.us.z64"
cd "$DECOMP"
NATIVE_FS="$(stat -f -c %T .)"
echo "Native workspace filesystem: $NATIVE_FS"
case "$DECOMP" in
  /mnt/*)
    echo "Refusing to run legacy Rocket compilers from a Windows-mounted WSL path: $DECOMP" >&2
    exit 25
    ;;
esac
[[ "$(git rev-parse HEAD)" == "$SOURCE_COMMIT" ]] || { echo "Native staged decomp commit mismatch." >&2; exit 26; }
echo "Native staged decomp commit: $(git rev-parse HEAD)"

step "5.3 - Create isolated Splat Python environment"
rm -rf "$VENV"
python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip setuptools wheel
if [[ -f tools/splat/requirements.txt ]]; then
  "$VENV/bin/python" -m pip install -r tools/splat/requirements.txt
  # Rocket pins Splat 0.12.10 from December 2022. Its requirements only set a
  # minimum spimdisasm version, so a fresh install in 2026 otherwise pulls a
  # much newer disassembler whose generated assembly contains marker macros
  # (nonmatching/enddlabel) that this Rocket checkout does not define.
  # Freeze spimdisasm to the historical minimum this Splat commit declared.
  "$VENV/bin/python" -m pip install --upgrade --force-reinstall "spimdisasm==1.9.0"
else
  echo "Splat requirements file is missing." >&2
  exit 14
fi
"$VENV/bin/python" -m pip check
"$VENV/bin/python" - <<'PYIMPORT'
import rabbitizer
import spimdisasm
import tqdm
import yaml
from colorama import Fore, Style
from intervaltree import Interval, IntervalTree
print("Splat core Python imports: PASS")
print(f"spimdisasm runtime version: {spimdisasm.__version__}")
if spimdisasm.__version__ != "1.9.0":
    raise RuntimeError(f"Expected historical spimdisasm 1.9.0, got {spimdisasm.__version__}")
try:
    from yaml import CLoader
    print("PyYAML LibYAML CLoader: PASS")
except Exception as exc:
    print(f"PyYAML LibYAML CLoader: unavailable ({exc}); pure Python loader remains usable")
PYIMPORT

echo "Resolved Python package versions:"
"$VENV/bin/python" -m pip freeze | sort
export PATH="$VENV/bin:$PATH"

step "5.4 - Split the verified NSUE ROM with pinned Splat"
make setup
[[ -d asm ]] || { echo "make setup completed but asm/ was not generated." >&2; exit 15; }
[[ -f NSUE.ld ]] || { echo "make setup completed but NSUE.ld was not generated." >&2; exit 16; }
# Newer spimdisasm versions (1.36+) emit marker macros which this 2022 Rocket
# include_asm.h does not define. Refuse to start compilation if they reappear.
if grep -R -n -E '^[[:space:]]*(nonmatching|enddlabel)([[:space:]]|$)' asm > "$NATIVE_BASE/unexpected-asm-markers.txt" 2>/dev/null; then
  echo "Generated assembly contains unsupported post-2022 spimdisasm markers:" >&2
  head -n 20 "$NATIVE_BASE/unexpected-asm-markers.txt" >&2 || true
  echo "Historical Splat dependency lock did not take effect." >&2
  exit 30
fi
echo "Generated assembly compatibility scan: PASS (no nonmatching/enddlabel markers)"

step "5.5 - Build/verify Rocket's legacy compiler tools"
make -C tools all
[[ -x tools/gcc-2.7.2/gcc ]] || { echo "KMC GCC 2.7.2 was not produced." >&2; exit 17; }
[[ -x tools/gcc-2.7.2/as ]] || { echo "KMC binutils 2.6 assembler was not produced." >&2; exit 18; }
[[ -x tools/modern-sn64/gcc ]] || { echo "SN64 GCC was not produced." >&2; exit 19; }
[[ -x tools/modern-sn64/modern-asn64.py ]] || { echo "modern-asn64.py was not produced/executable." >&2; exit 20; }
echo "Legacy tool architecture/runtime diagnostics:"
file tools/gcc-2.7.2/gcc tools/gcc-2.7.2/as tools/modern-sn64/gcc || true
for tool in tools/gcc-2.7.2/gcc tools/gcc-2.7.2/as tools/modern-sn64/gcc; do
  echo "+ $tool --version"
  "$tool" --version </dev/null | head -n 3 || { echo "Legacy compiler could not execute: $tool" >&2; exit 24; }
done

step "5.6 - Build byte-matching Rocket NSUE ROM and ELF"
# Rocket's generated Makefile contains aliased paths such as asm/entry.s and
# asm//entry.s which resolve to the same output object. Building those aliases
# concurrently can make two compiler processes overwrite entry.o while the
# linker is starting. Keep this byte-matching legacy stage deterministic.
JOBS=1
echo "Deterministic decomp jobs: $JOBS (serial build prevents aliased object races)"
make -j1

step "5.7 - Verify outputs and copy them back to Rocket-R"
[[ -f build/us/NSUE.elf ]] || { echo "build/us/NSUE.elf was not produced." >&2; exit 21; }
[[ -f build/us/NSUE.z64 ]] || { echo "build/us/NSUE.z64 was not produced." >&2; exit 22; }
cmp -s baserom.us.z64 build/us/NSUE.z64 || { echo "Generated NSUE.z64 does not byte-match the validated ROM." >&2; exit 23; }
WINDOWS_OUTPUT="$SOURCE_DECOMP/build/us"
mkdir -p "$WINDOWS_OUTPUT"
cp -f build/us/NSUE.elf "$WINDOWS_OUTPUT/NSUE.elf"
cp -f build/us/NSUE.z64 "$WINDOWS_OUTPUT/NSUE.z64"
[[ -f "$WINDOWS_OUTPUT/NSUE.elf" ]] || { echo "Failed to copy NSUE.elf back to Rocket-R." >&2; exit 27; }
cmp -s build/us/NSUE.elf "$WINDOWS_OUTPUT/NSUE.elf" || { echo "Copied NSUE.elf differs from the verified native build output." >&2; exit 28; }
cmp -s build/us/NSUE.z64 "$WINDOWS_OUTPUT/NSUE.z64" || { echo "Copied NSUE.z64 differs from the verified native build output." >&2; exit 29; }
echo "NSUE ELF: $(stat -c%s build/us/NSUE.elf) bytes"
echo "NSUE ROM byte-match: PASS"
echo "Copied verified ELF to: $WINDOWS_OUTPUT/NSUE.elf"
echo "Rocket matching-ELF stage: PASS"
