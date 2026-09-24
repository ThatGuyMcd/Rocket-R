#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import argparse
import json
import os
import py_compile
import re
import shutil
import sys
import time

BACKUP_POINTER = Path('build/repair-backups/LAST-GRAPHICS-V28-BACKUP.txt')

MODIFIED_FILES = [
    Path('src/presentation_identity.cpp'),
    Path('src/presentation_identity.hpp'),
    Path('CMakeLists.txt'),
    Path('scripts/OneClickBuild.ps1'),
    Path('scripts/self_check.py'),
    Path('runtime-recomp/rocket.us.recomp-policy.json'),
]

INSTALLED_FILES = [
    Path('src/render_queue_v28.cpp'),
    Path('scripts/patch_render_queue_v28_generated.py'),
]


def die(message: str) -> None:
    raise RuntimeError(message)


def read_text(path: Path) -> tuple[str, bool, str]:
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    nl = '\r\n' if '\r\n' in text else '\n'
    return text.replace('\r\n', '\n'), bom, nl


def write_text(path: Path, text: str, bom: bool = False, nl: str = '\n') -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = text.replace('\r\n', '\n')
    if nl == '\r\n':
        text = text.replace('\n', '\r\n')
    data = text.encode('utf-8')
    if bom:
        data = b'\xef\xbb\xbf' + data
    path.write_bytes(data)


def find_root(requested: str | None) -> Path:
    candidates: list[Path] = []
    if requested:
        candidates.append(Path(requested))
    # V28Payload/scripts/apply_graphics_v28.py -> repo root is two parents up.
    candidates.append(Path(__file__).resolve().parents[2])
    candidates.append(Path.cwd())
    for candidate in candidates:
        try:
            root = candidate.resolve()
        except OSError:
            continue
        if (root / 'scripts/OneClickBuild.ps1').is_file() and \
           (root / 'src/presentation_identity.cpp').is_file() and \
           (root / 'runtime-recomp/rocket.us.recomp-policy.json').is_file():
            return root
    die('Could not locate the Rocket-R repository root.')


def validate_stable_payload(root: Path) -> None:
    stable_cpp = root / 'Payload/src/presentation_identity.cpp'
    stable_hpp = root / 'Payload/src/presentation_identity.hpp'
    if not stable_cpp.is_file() or not stable_hpp.is_file():
        die('Stable Payload/src presentation identity files are missing.')
    cpp = stable_cpp.read_text(encoding='utf-8-sig')
    required = (
        'constexpr std::uint64_t kMaximumTrackAge = 1U;',
        'constexpr float kMaximumTrackDistance = 384.0F;',
        'struct SharedMatrixSample',
        'CanonicalGfxRef',
        'BuildSharedMatrixSamples',
        'MatchSharedMatrixSamples',
        'SubmittedFrame& frame = g_submitted.front();',
        'g_active_task_fail_closed = true;',
        'binding = IgnoredBinding();',
    )
    missing = [x for x in required if x not in cpp]
    if missing:
        die('Payload/src is not the preserved stable v5/v6 interpolation baseline: ' + ', '.join(missing))
    forbidden = (
        'PendingModelOwner', 'OwnerTrack', 'DURABLE-OWNER-V26',
        'rocket_graphics_arena_v27', 'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION',
    )
    bad = [x for x in forbidden if x in cpp]
    if bad:
        die('Payload/src stable interpolation copy contains later experimental markers: ' + ', '.join(bad))


def make_backup(root: Path) -> tuple[Path, dict]:
    stamp = time.strftime('%Y%m%d-%H%M%S')
    backup = root / 'build/repair-backups' / f'graphics-v28-{stamp}'
    backup.mkdir(parents=True, exist_ok=False)
    manifest = {'backed_up': [], 'created_files': []}

    for rel in MODIFIED_FILES:
        src = root / rel
        if not src.is_file():
            die(f'Missing required file before backup: {rel}')
        dst = backup / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        manifest['backed_up'].append(str(rel).replace('\\', '/'))

    for rel in INSTALLED_FILES:
        src = root / rel
        if src.exists():
            dst = backup / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            manifest['backed_up'].append(str(rel).replace('\\', '/'))
        else:
            manifest['created_files'].append(str(rel).replace('\\', '/'))

    (backup / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    pointer = root / BACKUP_POINTER
    pointer.parent.mkdir(parents=True, exist_ok=True)
    pointer.write_text(str(backup) + '\n', encoding='utf-8')
    return backup, manifest


def rollback(root: Path, backup: Path, manifest: dict) -> None:
    for rel_text in manifest.get('backed_up', []):
        rel = Path(rel_text)
        src = backup / rel
        dst = root / rel
        if not src.is_file():
            die(f'Backup is incomplete: {src}')
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    for rel_text in manifest.get('created_files', []):
        target = root / Path(rel_text)
        if target.exists():
            target.unlink()


def rollback_last(root: Path) -> None:
    pointer = root / BACKUP_POINTER
    if not pointer.is_file():
        die('No v28 backup pointer exists.')
    backup = Path(pointer.read_text(encoding='utf-8').strip())
    if not backup.is_dir():
        die(f'Last v28 backup directory no longer exists: {backup}')
    manifest_path = backup / 'manifest.json'
    if not manifest_path.is_file():
        die(f'Backup manifest missing: {manifest_path}')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    rollback(root, backup, manifest)
    print(f'[OK] Restored pre-v28 files from: {backup}')


def restore_v6_interpolation(root: Path) -> None:
    shutil.copy2(root / 'Payload/src/presentation_identity.cpp',
                 root / 'src/presentation_identity.cpp')
    shutil.copy2(root / 'Payload/src/presentation_identity.hpp',
                 root / 'src/presentation_identity.hpp')


def install_v28_payload(root: Path) -> None:
    package_root = Path(__file__).resolve().parents[1]
    pairs = [
        (package_root / 'src/render_queue_v28.cpp', root / 'src/render_queue_v28.cpp'),
        (package_root / 'scripts/patch_render_queue_v28_generated.py',
         root / 'scripts/patch_render_queue_v28_generated.py'),
    ]
    for src, dst in pairs:
        if not src.is_file():
            die(f'V28 package payload missing: {src}')
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def patch_cmake(root: Path) -> None:
    path = root / 'CMakeLists.txt'
    text, bom, nl = read_text(path)
    marker = '    src/presentation_identity.cpp\n'
    line = '    src/render_queue_v28.cpp\n'
    if line not in text:
        if text.count(marker) != 1:
            die('Could not locate unique presentation_identity.cpp source entry in CMakeLists.txt')
        text = text.replace(marker, marker + line, 1)
    write_text(path, text, bom, nl)


def patch_policy(root: Path) -> None:
    path = root / 'runtime-recomp/rocket.us.recomp-policy.json'
    data = json.loads(path.read_text(encoding='utf-8-sig'))
    hooks = data.get('functionHooks', [])
    if not isinstance(hooks, list):
        die('rocket.us.recomp-policy.json functionHooks is not a list')
    kept = []
    removed = 0
    for hook in hooks:
        if isinstance(hook, dict) and hook.get('function') == 'func_8001ECEC' and \
           'rocket_presentation_model_entry_owner' in str(hook.get('text', '')):
            removed += 1
            continue
        kept.append(hook)
    data['functionHooks'] = kept
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    if removed:
        print(f'[OK] Removed {removed} v26 durable-owner policy hook(s).')
    else:
        print('[OK] v26 durable-owner policy hook already absent.')


def patch_oneclick(root: Path) -> None:
    path = root / 'scripts/OneClickBuild.ps1'
    text, bom, nl = read_text(path)

    # Remove the three experimental generated patch calls and their associated
    # contiguous Graphics v20/v21/v27 comment block.  Fresh N64Recomp output is
    # then patched only once by V28.
    start_marker = '    # Graphics v20: diagnostic-only instrumentation.'
    end_call = "    Invoke-Python @((Join-Path $Root 'scripts\\patch_graphics_arena_v27_generated.py'),'--root',$Root)"
    if start_marker in text and end_call in text:
        start = text.index(start_marker)
        end = text.index(end_call, start) + len(end_call)
        while end < len(text) and text[end] in '\r\n':
            end += 1
        replacement = (
            '    # v28 stable v5/v6 interpolation + unbounded render queue.\n'
            '    # Keep the original single-pass Rocket draw/state loop, but move RenderEntry\n'
            '    # storage out of the retail 256-slot list. No v21 batching or v27 arena redirect.\n'
            "    Invoke-Python @((Join-Path $Root 'scripts\\patch_render_queue_v28_generated.py'),'--root',$Root)\n\n"
        )
        text = text[:start] + replacement + text[end:]
    elif 'patch_render_queue_v28_generated.py' not in text:
        die('Could not locate the current v20/v21/v27 generated patch block in OneClickBuild.ps1')

    # Ensure retired patchers are not still invoked elsewhere.
    for retired in (
        'patch_popin_diagnostics_generated.py',
        'patch_render_queue_expansion_v21_generated.py',
        'patch_graphics_arena_v27_generated.py',
    ):
        if re.search(r'Invoke-Python[^\n]*' + re.escape(retired), text):
            die(f'OneClickBuild.ps1 still invokes retired patcher: {retired}')

    replacements = {
        "Write-Host 'FIXED34 deliberately restores the complete FIXED27 interpolation/runtime baseline; platform support is layered around it.' -ForegroundColor Green":
            "Write-Host 'FIXED34 gameplay timing retained; v28 stable v5/v6 interpolation + unbounded render queue enabled.' -ForegroundColor Green",
        "Write-Host 'FIXED34 prefers locally cached pinned Git commits, keeps the accepted FIXED27 renderer/interpolation patch set, and only fetches a dependency when its required object is missing.' -ForegroundColor DarkGreen":
            "Write-Host 'FIXED34 keeps pinned dependencies; v28 restores stable v5/v6 interpolation and removes the retail 256-entry render-list ceiling.' -ForegroundColor DarkGreen",
        "Write-Host 'Rocket runtime policy: FIXED27 gameplay/interpolation baseline restored exactly (retail 30 Hz simulation + RT64 presentation interpolation + safe-area crop + widescreen CPU frustum), with platform-only build/packaging changes layered around it.' -ForegroundColor DarkGreen":
            "Write-Host 'Rocket runtime policy: retail 30 Hz simulation + stable v5/v6 RT64 semantic interpolation + v28 unbounded single-pass render queue + safe-area crop + widescreen CPU frustum.' -ForegroundColor DarkGreen",
    }
    for old, new in replacements.items():
        if old in text:
            text = text.replace(old, new, 1)

    write_text(path, text, bom, nl)


def patch_self_check(root: Path) -> None:
    path = root / 'scripts/self_check.py'
    text, bom, nl = read_text(path)

    old_builder = '''    require("FIXED27 gameplay/interpolation baseline restored exactly" in builder and
            "retail 30 Hz simulation + RT64 presentation interpolation + safe-area crop + widescreen CPU frustum" in builder,
            "OneClickBuild.ps1 must report the FIXED34 rollback/runtime policy")'''
    new_builder = '''    require("v28 stable v5/v6 interpolation + unbounded render queue" in builder,
            "OneClickBuild.ps1 must report the v28 stable interpolation/unbounded queue policy")'''
    if old_builder in text:
        text = text.replace(old_builder, new_builder, 1)
    elif 'v28 stable v5/v6 interpolation + unbounded render queue' not in text:
        die('Could not update the FIXED27 builder-policy assertion in self_check.py')

    start_token = '    # v26: keep the proven v21 queue recovery and retail draw-distance path, but bind\n'
    end_token = '    scan_checked_source(root)\n'
    start = text.find(start_token)
    end = text.find(end_token, start if start >= 0 else 0)
    if start >= 0 and end >= 0:
        block = '''    # v28: restore the user-qualified v5/v6 semantic interpolation path exactly,
    # remove v26 durable-owner guessing and v27 arena redirection, and replace v21's
    # 256-entry replay batches with one Expansion-Pak-backed single-pass render list.
    _v28_root = __import__('pathlib').Path(__file__).resolve().parents[1]
    _v28_presentation = (_v28_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
    _v28_header = (_v28_root / 'src' / 'presentation_identity.hpp').read_text(encoding='utf-8-sig')
    _v28_queue_source = (_v28_root / 'src' / 'render_queue_v28.cpp').read_text(encoding='utf-8-sig')
    _v28_patcher = (_v28_root / 'scripts' / 'patch_render_queue_v28_generated.py').read_text(encoding='utf-8-sig')
    _v28_oneclick = (_v28_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    _v28_cmake = (_v28_root / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
    _v28_policy = json.loads((_v28_root / 'runtime-recomp' / 'rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))
    _v28_owner_hooks = [h for h in _v28_policy.get('functionHooks', [])
                        if isinstance(h, dict) and h.get('function') == 'func_8001ECEC'
                        and 'rocket_presentation_model_entry_owner' in str(h.get('text', ''))]
    _v28_required = (
        'constexpr std::uint64_t kMaximumTrackAge = 1U;' in _v28_presentation and
        'constexpr float kMaximumTrackDistance = 384.0F;' in _v28_presentation and
        'BuildSharedMatrixSamples' in _v28_presentation and
        'MatchSharedMatrixSamples' in _v28_presentation and
        'SubmittedFrame& frame = g_submitted.front();' in _v28_presentation and
        'g_active_task_fail_closed = true;' in _v28_presentation and
        'PendingModelOwner' not in _v28_presentation and
        'OwnerTrack' not in _v28_presentation and
        'DURABLE-OWNER-V26' not in _v28_presentation and
        'rocket_graphics_arena_v27' not in _v28_presentation and
        'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION' not in _v28_presentation and
        'rocket_presentation_model_entry_owner' not in _v28_header and
        len(_v28_owner_hooks) == 0 and
        'ROCKET-R GRAPHICS V28 UNBOUNDED RENDER QUEUE BEGIN' in _v28_queue_source and
        'kQueueCapacity = 32768U' in _v28_queue_source and
        'ROCKET-R GRAPHICS V28 PREPARE UNBOUNDED QUEUE' in _v28_patcher and
        'src/render_queue_v28.cpp' in _v28_cmake and
        'patch_render_queue_v28_generated.py' in _v28_oneclick and
        'patch_render_queue_expansion_v21_generated.py' not in _v28_oneclick and
        'patch_graphics_arena_v27_generated.py' not in _v28_oneclick and
        'patch_popin_diagnostics_generated.py' not in _v28_oneclick
    )
    if not _v28_required:
        raise SystemExit('SOURCE SELF-CHECK FAILED: v28 stable interpolation/unbounded render queue state missing')
'''
        text = text[:start] + block + text[end:]
    elif 'v28 stable interpolation/unbounded render queue state missing' not in text:
        die('Could not locate the current v26 self-check block')

    write_text(path, text, bom, nl)


def validate_installed(root: Path) -> None:
    p = (root / 'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    if 'SubmittedFrame& frame = g_submitted.front();' not in p:
        die('Stable FIFO task matching was not restored')
    for bad in ('PendingModelOwner', 'OwnerTrack', 'DURABLE-OWNER-V26',
                'rocket_graphics_arena_v27',
                'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION'):
        if bad in p:
            die(f'Retired presentation marker still active: {bad}')

    policy = json.loads((root / 'runtime-recomp/rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))
    owner = [h for h in policy.get('functionHooks', [])
             if isinstance(h, dict) and h.get('function') == 'func_8001ECEC'
             and 'rocket_presentation_model_entry_owner' in str(h.get('text', ''))]
    if owner:
        die('v26 durable-owner hook is still present in recomp policy')

    cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
    if cmake.count('src/render_queue_v28.cpp') != 1:
        die('CMake v28 source wiring missing/duplicated')

    oneclick = (root / 'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    if oneclick.count('patch_render_queue_v28_generated.py') != 1:
        die('OneClick v28 generated patch call missing/duplicated')
    for retired in ('patch_render_queue_expansion_v21_generated.py',
                    'patch_graphics_arena_v27_generated.py',
                    'patch_popin_diagnostics_generated.py'):
        if re.search(r'Invoke-Python[^\n]*' + re.escape(retired), oneclick):
            die(f'OneClick still invokes retired generated patch: {retired}')

    py_compile.compile(str(root / 'scripts/patch_render_queue_v28_generated.py'), doraise=True)
    py_compile.compile(str(root / 'scripts/self_check.py'), doraise=True)


def apply(root: Path) -> None:
    validate_stable_payload(root)
    backup, manifest = make_backup(root)
    print(f'[OK] Backup: {backup}')
    try:
        restore_v6_interpolation(root)
        print('[OK] Restored stable v5/v6 semantic interpolation from Payload/src.')
        install_v28_payload(root)
        print('[OK] Installed v28 single-pass Expansion Pak render queue source/patcher.')
        patch_cmake(root)
        patch_policy(root)
        patch_oneclick(root)
        patch_self_check(root)
        validate_installed(root)
        print('[OK] v28 source validation PASS.')
        print('[OK] Interpolation: stable v5/v6 FIFO/fail-closed task ownership.')
        print('[OK] Render queue: 32,768-entry Expansion Pak storage; no 256-entry batching.')
        print('[OK] GfxTask arena: retail allocation retained; runtime headroom telemetry enabled.')
        print('[NEXT] Run ONE-CLICK-BUILD.cmd and build Windows first.')
    except Exception:
        print('[ERROR] v28 install failed; restoring the pre-v28 backup...', file=sys.stderr)
        rollback(root, backup, manifest)
        raise


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--root')
    ap.add_argument('--rollback-last', action='store_true')
    ns = ap.parse_args()
    root = find_root(ns.root)
    print('')
    print('==============================================================================')
    print('  Rocket-R Graphics v28 - Stable Interpolation + Unbounded Render Queue')
    print('==============================================================================')
    print(f'Repository: {root}')
    if ns.rollback_last:
        rollback_last(root)
    else:
        apply(root)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f'Graphics v28 failed: {exc}', file=sys.stderr)
        raise SystemExit(1)
