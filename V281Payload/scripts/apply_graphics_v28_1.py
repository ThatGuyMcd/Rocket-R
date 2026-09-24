from __future__ import annotations

import argparse
import py_compile
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

POINTER = Path('build/repair-backups/LAST-GRAPHICS-V28-1-BACKUP.txt')
TARGETS = [
    Path('src/render_queue_v28.cpp'),
    Path('scripts/patch_render_queue_v28_generated.py'),
]


def die(message: str) -> None:
    raise RuntimeError(message)


def resolve_root(value: str) -> Path:
    root = Path(value).resolve()
    required = [
        root / 'ONE-CLICK-BUILD.cmd',
        root / 'scripts/OneClickBuild.ps1',
        root / 'src/render_queue_v28.cpp',
        root / 'scripts/patch_render_queue_v28_generated.py',
    ]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        die('This hotfix expects Rocket-R Graphics v28 to already be installed. Missing: ' + ', '.join(missing))
    return root


def package_root() -> Path:
    # .../V281Payload/scripts/apply_graphics_v28_1.py -> package root
    return Path(__file__).resolve().parents[2]


def backup(root: Path) -> Path:
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    backup_dir = root / 'build' / 'repair-backups' / f'graphics-v28.1-{stamp}'
    for rel in TARGETS:
        src = root / rel
        dst = backup_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    pointer = root / POINTER
    pointer.parent.mkdir(parents=True, exist_ok=True)
    pointer.write_text(str(backup_dir), encoding='utf-8')
    return backup_dir


def restore_backup(root: Path, backup_dir: Path) -> None:
    for rel in TARGETS:
        src = backup_dir / rel
        if not src.is_file():
            die(f'Backup is incomplete: {src}')
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def rollback_last(root: Path) -> None:
    pointer = root / POINTER
    if not pointer.is_file():
        die('No v28.1 backup pointer exists.')
    backup_dir = Path(pointer.read_text(encoding='utf-8').strip())
    if not backup_dir.is_dir():
        die(f'Last v28.1 backup directory no longer exists: {backup_dir}')
    restore_backup(root, backup_dir)
    print(f'[OK] Restored pre-v28.1 files from: {backup_dir}')


def validate_payload_source(path: Path) -> None:
    text = path.read_text(encoding='utf-8-sig')
    required = [
        'ReadU32(std::uint8_t* rdram, std::uint32_t address)',
        'WriteU32(std::uint8_t* rdram, std::uint32_t address, std::uint32_t value)',
        'WriteU8(std::uint8_t* rdram, std::uint32_t address, std::uint8_t value)',
        'DisplayListHeadroom(std::uint8_t* rdram)',
        'ObserveHeadroom(std::uint8_t* rdram)',
        'rocket_render_queue_v28_add(std::uint8_t* rdram, recomp_context* ctx)',
        'rocket_render_queue_v28_prepare(std::uint8_t* rdram, recomp_context*)',
        'rocket_render_queue_v28_entry_address(std::uint8_t* rdram, int index)',
        'rocket_render_queue_v28_end(std::uint8_t* rdram, recomp_context*)',
    ]
    for marker in required:
        if marker not in text:
            die(f'v28.1 render queue payload missing marker: {marker}')
    forbidden = [
        'ReadU32(std::uint32_t address)',
        'WriteU32(std::uint32_t address, std::uint32_t value)',
        'WriteU8(std::uint32_t address, std::uint8_t value)',
        'rocket_render_queue_v28_add(std::uint8_t*, recomp_context* ctx)',
        'rocket_render_queue_v28_entry_address(int index)',
    ]
    for marker in forbidden:
        if marker in text:
            die(f'Retired v28 compile-broken marker still present: {marker}')


def install(root: Path) -> None:
    pkg = package_root() / 'V281Payload'
    source_src = pkg / 'src' / 'render_queue_v28.cpp'
    patcher_src = pkg / 'scripts' / 'patch_render_queue_v28_generated.py'
    if not source_src.is_file() or not patcher_src.is_file():
        die('v28.1 package payload is incomplete.')
    validate_payload_source(source_src)
    py_compile.compile(str(patcher_src), doraise=True)

    backup_dir = backup(root)
    try:
        shutil.copy2(source_src, root / 'src' / 'render_queue_v28.cpp')
        shutil.copy2(patcher_src, root / 'scripts' / 'patch_render_queue_v28_generated.py')
        validate_payload_source(root / 'src' / 'render_queue_v28.cpp')
        py_compile.compile(str(root / 'scripts' / 'patch_render_queue_v28_generated.py'), doraise=True)

        # If the failed Windows build left generated C in place, upgrade and
        # verify it now. A normal OneClick rebuild will regenerate it anyway.
        generated = root / 'runtime-recomp' / 'RecompiledFuncs'
        if generated.is_dir() and any(generated.glob('*.c')):
            subprocess.run([
                sys.executable,
                str(root / 'scripts' / 'patch_render_queue_v28_generated.py'),
                '--root', str(root)
            ], check=True)
            subprocess.run([
                sys.executable,
                str(root / 'scripts' / 'patch_render_queue_v28_generated.py'),
                '--root', str(root), '--verify'
            ], check=True)

        # The broader v28 source policy should remain unchanged.
        self_check = root / 'scripts' / 'self_check.py'
        if self_check.is_file():
            subprocess.run([sys.executable, str(self_check), '--root', str(root)], check=True)

        print(f'[OK] v28.1 compile hotfix installed. Backup: {backup_dir}')
        print('[OK] Every recomp MEM_* access now has an explicit rdram pointer in scope.')
        print('[OK] Generated renderer selector ABI now passes rdram explicitly.')
    except Exception:
        print('[ERROR] v28.1 install failed; restoring backup...', file=sys.stderr)
        restore_backup(root, backup_dir)
        raise


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--rollback-last', action='store_true')
    ns = ap.parse_args()
    root = resolve_root(ns.root)
    print('')
    print('==============================================================================')
    print('  Rocket-R Graphics v28.1 - Compile Hotfix')
    print('==============================================================================')
    print(f'Repository: {root}')
    print('')
    if ns.rollback_last:
        rollback_last(root)
    else:
        install(root)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f'Graphics v28.1 failed: {exc}', file=sys.stderr)
        raise SystemExit(1)
