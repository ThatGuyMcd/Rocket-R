#!/usr/bin/env python3
from pathlib import Path
import argparse
import datetime
import shutil
import subprocess
import sys

BEGIN = '// === ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN ==='
END = '// === ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION END ==='

OLD_SIG_DL = '[[nodiscard]] std::uint32_t RocketV21DisplayListHeadroomBytes() {'
NEW_SIG_DL = '[[nodiscard]] std::uint32_t RocketV21DisplayListHeadroomBytes(std::uint8_t* rdram) {'
OLD_SIG_OBS = 'void RocketV21ObserveDisplayListHeadroom() {'
NEW_SIG_OBS = 'void RocketV21ObserveDisplayListHeadroom(std::uint8_t* rdram) {'
OLD_CALL_DL = 'state.min_dl_headroom_bytes, RocketV21DisplayListHeadroomBytes());'
NEW_CALL_DL = 'state.min_dl_headroom_bytes, RocketV21DisplayListHeadroomBytes(rdram));'
OLD_CALL_OBS = 'RocketV21ObserveDisplayListHeadroom();'
NEW_CALL_OBS = 'RocketV21ObserveDisplayListHeadroom(rdram);'


def read_text(path: Path):
    raw = path.read_bytes()
    return raw.decode('utf-8-sig'), raw.startswith(b'\xef\xbb\xbf'), ('\r\n' if b'\r\n' in raw else '\n')


def write_text(path: Path, text: str, bom: bool, nl: str):
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def backup(root: Path, path: Path):
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    dest = root / 'build' / 'repair-backups' / f'graphics-v21.3-{stamp}' / path.relative_to(root)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, dest)
    print(f'[OK] Backup: {dest}')


def verify(text: str):
    if text.count(BEGIN) != 1 or text.count(END) != 1:
        raise RuntimeError('v21 render-queue block is missing or duplicated')
    checks = {
        'RDRAM-aware display-list headroom reader': text.count(NEW_SIG_DL) == 1,
        'RDRAM-aware headroom observer': text.count(NEW_SIG_OBS) == 1,
        'observer forwards RDRAM to reader': text.count(NEW_CALL_DL) == 1,
        'both staging call sites forward RDRAM': text.count(NEW_CALL_OBS) == 2,
        'old zero-argument headroom reader removed': OLD_SIG_DL not in text,
        'old zero-argument observer removed': OLD_SIG_OBS not in text,
        'old zero-argument observer calls removed': OLD_CALL_OBS not in text,
        'v21 capture hook retained': 'rocket_render_queue_capture_entry' in text,
        'v21 first-batch hook retained': 'rocket_render_queue_prepare_first_batch' in text,
        'v21 continuation hook retained': 'rocket_render_queue_prepare_next_batch' in text,
    }
    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        raise RuntimeError('v21.3 verification failed: ' + '; '.join(failed))


def patch(path: Path):
    text, bom, nl = read_text(path)
    verify_ready = NEW_SIG_DL in text and NEW_SIG_OBS in text
    if verify_ready:
        verify(text)
        print('[OK] v21.3 compile hotfix already present; no source rewrite needed.')
        return False

    expected = {
        OLD_SIG_DL: 1,
        OLD_SIG_OBS: 1,
        OLD_CALL_DL: 1,
        OLD_CALL_OBS: 2,
    }
    for token, count in expected.items():
        actual = text.count(token)
        if actual != count:
            raise RuntimeError(f'Expected {count} occurrence(s) of {token!r}, found {actual}; refusing ambiguous edit')

    text = text.replace(OLD_SIG_DL, NEW_SIG_DL, 1)
    text = text.replace(OLD_SIG_OBS, NEW_SIG_OBS, 1)
    text = text.replace(OLD_CALL_DL, NEW_CALL_DL, 1)
    text = text.replace(OLD_CALL_OBS, NEW_CALL_OBS)
    verify(text)
    write_text(path, text, bom, nl)
    print('[OK] Threaded the real RDRAM pointer through v21 display-list headroom telemetry.')
    print('[OK] Queue capture/order/staging behaviour was not changed.')
    return True


def run_checked(cmd, cwd: Path, label: str):
    print('+ ' + ' '.join(str(x) for x in cmd))
    cp = subprocess.run(cmd, cwd=str(cwd))
    if cp.returncode != 0:
        raise RuntimeError(f'{label} failed with exit code {cp.returncode}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    target = root / 'src' / 'presentation_identity.cpp'
    if not target.is_file():
        raise RuntimeError(f'Missing Rocket-R source file: {target}')

    text, _, _ = read_text(target)
    if BEGIN not in text:
        raise RuntimeError('v21 in-function queue expansion is not installed; this hotfix is only for the current v21 tree')

    if NEW_SIG_DL not in text:
        backup(root, target)
    patch(target)

    final, _, _ = read_text(target)
    verify(final)

    generated = root / 'scripts' / 'patch_render_queue_expansion_v21_generated.py'
    if generated.is_file():
        run_checked([sys.executable, str(generated), '--root', str(root), '--verify'], root,
                    'v21 generated renderer verification')

    self_check = root / 'scripts' / 'self_check.py'
    if self_check.is_file():
        run_checked([sys.executable, str(self_check)], root, 'Rocket-R source self-check')

    print('[OK] Rocket-R Graphics v21.3 compile verification PASS.')
    print('[OK] Re-run ONE-CLICK-BUILD.cmd; no clean/reinstall is required.')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'[ERROR] {exc}', file=sys.stderr)
        sys.exit(1)
