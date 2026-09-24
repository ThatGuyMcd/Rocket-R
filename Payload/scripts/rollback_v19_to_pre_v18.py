#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil
import datetime
import re
import sys

RESTORE_FILES = [
    Path('src/graphics_enhancements.cpp'),
    Path('src/widescreen_culling.cpp'),
    Path('src/presentation_identity.cpp'),
    Path('scripts/OneClickBuild.ps1'),
    Path('scripts/self_check.py'),
]

V19_TOKENS = (
    'RocketBuildGlobalRenderOrder',
    'rocket_render_queue_finalize_capture',
    'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE',
    'kNoSideCullRadiusBits = 0x7F7FFFFFU',
)


def read(path: Path) -> str:
    return path.read_text(encoding='utf-8-sig', errors='strict')


def candidate_ok(path: Path):
    missing = [str(rel) for rel in RESTORE_FILES if not (path / rel).is_file()]
    if missing:
        return False, 'missing: ' + ', '.join(missing)
    try:
        g = read(path/'src/graphics_enhancements.cpp')
        c = read(path/'src/widescreen_culling.cpp')
        p = read(path/'src/presentation_identity.cpp')
        o = read(path/'scripts/OneClickBuild.ps1')
        sc = read(path/'scripts/self_check.py')
    except Exception as exc:
        return False, f'read failed: {exc}'

    required = [
        ('graphics r7 Draw Distance', 'draw_distance_multiplier' in g and 'context->r7' in g),
        ('v16 viewport/FOV baseline', 'v16 viewport-locked FOV/aspect guard active' in c),
        ('v17 saturation diagnostic', '[render-queue] SATURATION' in p),
    ]
    bad = [name for name, ok in required if not ok]
    if bad:
        return False, 'not v17.1 lineage: ' + ', '.join(bad)
    if any(t in g or t in c or t in p or t in o or t in sc for t in V19_TOKENS):
        return False, 'contains v19 renderer tokens'

    gen = path/'runtime-recomp/RecompiledFuncs'
    if not gen.is_dir():
        return False, 'generated RecompiledFuncs backup missing'
    renderer = []
    add = []
    for f in gen.glob('*.c'):
        try:
            txt = read(f)
        except Exception:
            continue
        if 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)' in txt:
            renderer.append(f)
        if 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)' in txt:
            add.append(f)
    if len(renderer) != 1 or len(add) != 1:
        return False, f'expected one original generated renderer and add_render_entry, found {len(renderer)}/{len(add)}'
    for f in renderer + add:
        txt = read(f)
        if 'ROCKET-R GRAPHICS V18.1 SAFE MULTI-PASS RENDER QUEUE' in txt or 'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE' in txt:
            return False, 'generated backup is already queue-wrapped'
    return True, ''


def find_source_backup(root: Path):
    backup_root = root/'build/repair-backups'
    preferred = backup_root/'graphics-v18.1-20260924-010724'
    candidates = []
    if preferred.is_dir():
        candidates.append(preferred)
    if backup_root.is_dir():
        for p in sorted(backup_root.glob('graphics-v18.1-*')):
            if p not in candidates:
                candidates.append(p)
    reasons = []
    for p in candidates:
        ok, reason = candidate_ok(p)
        if ok:
            return p
        reasons.append(f'{p.name}: {reason}')
    msg = 'No safe pre-v18.1/v17.1 renderer backup was found.'
    if reasons:
        msg += '\nChecked:\n  ' + '\n  '.join(reasons)
    raise RuntimeError(msg)


def backup_current(root: Path):
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    dst = root/'build/repair-backups'/f'graphics-v19.4-broken-v19-{stamp}'
    for rel in RESTORE_FILES + [Path('src/runtime_ui.cpp'), Path('scripts/patch_render_queue_generated.py')]:
        src = root/rel
        if src.is_file():
            out = dst/rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, out)
    gen = root/'runtime-recomp/RecompiledFuncs'
    if gen.is_dir():
        gd = dst/'runtime-recomp/RecompiledFuncs'
        gd.mkdir(parents=True, exist_ok=True)
        for f in gen.glob('*.c'):
            try:
                txt = read(f)
            except Exception:
                continue
            if ('func_8008B694' in txt or 'add_render_entry' in txt or
                'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE' in txt or
                'ROCKET-R GRAPHICS V18.1 SAFE MULTI-PASS RENDER QUEUE' in txt):
                shutil.copy2(f, gd/f.name)
    return dst


def copy_file(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def restore(root: Path, source: Path):
    for rel in RESTORE_FILES:
        copy_file(source/rel, root/rel)

    # v17.1 had no persistent generated-queue wrapper. If the pre-v18 backup
    # contains one, restore that exact historical file; otherwise retire v19's.
    old_patcher = source/'scripts/patch_render_queue_generated.py'
    live_patcher = root/'scripts/patch_render_queue_generated.py'
    if old_patcher.is_file():
        copy_file(old_patcher, live_patcher)
    elif live_patcher.exists():
        live_patcher.unlink()

    source_gen = source/'runtime-recomp/RecompiledFuncs'
    live_gen = root/'runtime-recomp/RecompiledFuncs'
    live_gen.mkdir(parents=True, exist_ok=True)
    restored = []
    for f in source_gen.glob('*.c'):
        txt = read(f)
        if ('RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)' in txt or
            'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)' in txt):
            copy_file(f, live_gen/f.name)
            restored.append(f.name)
    if len(restored) != 2:
        raise RuntimeError(f'Expected to restore exactly two generated renderer files, restored {restored}')


def verify(root: Path):
    g = read(root/'src/graphics_enhancements.cpp')
    c = read(root/'src/widescreen_culling.cpp')
    p = read(root/'src/presentation_identity.cpp')
    o = read(root/'scripts/OneClickBuild.ps1')
    sc = read(root/'scripts/self_check.py')

    checks = {
        'Draw Distance remains on r7': 'draw_distance_multiplier' in g and 'context->r7' in g,
        'v16 viewport/FOV renderer baseline restored': 'v16 viewport-locked FOV/aspect guard active' in c,
        'v17 render queue diagnostic restored': '[render-queue] SATURATION' in p,
        'v19 global-order host queue removed': 'RocketBuildGlobalRenderOrder' not in p and 'rocket_render_queue_finalize_capture' not in p,
        'v19 distance-only r6 override removed': 'kNoSideCullRadiusBits = 0x7F7FFFFFU' not in c,
        'OneClick no longer applies v19 generated wrapper': 'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE' not in o,
        'self-check no longer requires v19 policy': 'distance-only object visibility + globally ordered expanded render queue' not in sc,
    }
    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        raise RuntimeError('Rollback verification failed: ' + '; '.join(failed))

    gen = root/'runtime-recomp/RecompiledFuncs'
    renderer = []
    add = []
    for f in gen.glob('*.c'):
        txt = read(f)
        if 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)' in txt:
            renderer.append(f)
        if 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)' in txt:
            add.append(f)
    if len(renderer) != 1 or len(add) != 1:
        raise RuntimeError(f'Generated renderer verification expected 1/1, found {len(renderer)}/{len(add)}')
    for f in renderer + add:
        txt = read(f)
        if 'ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE' in txt or 'rocket_capture_func_8008B694' in txt or 'rocket_draw_func_8008B694' in txt:
            raise RuntimeError(f'v19 generated renderer token remains in {f.name}')

    print('[OK] Restored v17.1-era renderer source and original generated renderer.')
    print('[OK] Extended Draw Distance slider remains on frustum_test r7.')
    print('[OK] Removed v19 capture-first/global-order renderer from the active build path.')
    print('[OK] Removed v19 FLT_MAX r6 side-plane override from the active build path.')
    print('[OK] runtime_ui.cpp was deliberately left untouched so the newer overlay-font fix remains.')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    source = find_source_backup(root)
    print(f'[OK] Recovery source: {source}')
    current_backup = backup_current(root)
    print(f'[OK] Backed up current broken v19 state: {current_backup}')
    restore(root, source)
    verify(root)

if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'[ERROR] {exc}', file=sys.stderr)
        raise SystemExit(1)
