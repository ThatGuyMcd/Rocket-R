#!/usr/bin/env python3
from pathlib import Path
import argparse
import subprocess
import sys


def fail(msg: str):
    print('[ERROR] ' + msg)
    raise SystemExit(1)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        fail('Missing function: ' + marker)
    brace = text.find('{', start)
    if brace < 0:
        fail('Missing opening brace: ' + marker)
    depth = 0
    state = 'code'
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/': state = 'line'; i += 2; continue
            if c == '/' and n == '*': state = 'block'; i += 2; continue
            if c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0: return text[start:i + 1]
        elif state == 'line':
            if c == '\n': state = 'code'
        elif state == 'block':
            if c == '*' and n == '/': state = 'code'; i += 2; continue
        elif state == 'string':
            if c == '\\': i += 2; continue
            if c == '"': state = 'code'
        elif state == 'char':
            if c == '\\': i += 2; continue
            if c == "'": state = 'code'
        i += 1
    fail('Unterminated function: ' + marker)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--verify-generated', action='store_true')
    args = ap.parse_args()
    root = Path(args.root).resolve()

    files = {
        'culling': root / 'src' / 'widescreen_culling.cpp',
        'presentation': root / 'src' / 'presentation_identity.cpp',
        'graphics': root / 'src' / 'graphics_enhancements.cpp',
        'oneclick': root / 'scripts' / 'OneClickBuild.ps1',
        'selfcheck': root / 'scripts' / 'self_check.py',
        'patcher': root / 'scripts' / 'patch_render_queue_generated.py',
        'ui': root / 'src' / 'runtime_ui.cpp',
    }
    for label, path in files.items():
        if not path.is_file(): fail(f'Missing {label}: {path}')

    c = files['culling'].read_text(encoding='utf-8-sig')
    p = files['presentation'].read_text(encoding='utf-8-sig')
    g = files['graphics'].read_text(encoding='utf-8-sig')
    o = files['oneclick'].read_text(encoding='utf-8-sig')
    sc = files['selfcheck'].read_text(encoding='utf-8-sig')
    u = files['ui'].read_text(encoding='utf-8-sig')

    for token in (
        'position_address = static_cast<std::uint32_t>(context->r5)',
        'static_cast<std::uint32_t>(context->r6)',
        'kViewMatrixOffset = 0x30',
        'view_matrix[12]',
        'view_matrix[13]',
        'view_matrix[14]',
        'view_z_axis',
        'view_z_sign',
        'kTargetFrustumGuard = 1.20F',
        'v18 renderer-view-matrix viewport guard active',
    ):
        if token not in c: fail('v18 culling token missing: ' + token)
    for retired in ('requested_horizontal_half', 'target_unit_planes', 'plane_pairs'):
        if retired in c: fail('retired inferred-plane culling remains: ' + retired)

    for token in (
        'ExpandedRenderEntry',
        'rocket_render_queue_capture(',
        'rocket_render_queue_begin(',
        'rocket_render_queue_begin_batch(',
        'rocket_render_queue_next(',
        'rocket_render_queue_finish_batch(',
        'rocket_render_queue_replay_end(',
        'g_render_queue_replay_loading',
        'kGuestRenderQueueBase = 0x800ADB00U',
        'kGuestRenderQueueEndPointerAddress = 0x800AF300U',
        '[render-queue] EXPANDED',
    ):
        if token not in p: fail('v18 queue token missing: ' + token)
    if '[render-queue] SATURATION' in p:
        fail('retired saturation-only diagnostic remains')

    body = function_span(g, 'extern "C" void rocket_graphics_frustum_begin')
    for token in ('context->r7', 'draw_distance_multiplier', 'kInfiniteRenderDistance'):
        if token not in body: fail('Draw Distance hook missing: ' + token)
    if 'maximum_detail' in body: fail('retired maximum_detail remains in Draw Distance hook')
    if 'MEM_W(0x14' in body: fail('Draw Distance hook incorrectly reads stack +0x14')

    if "scripts\\patch_render_queue_generated.py" not in o:
        fail('OneClickBuild does not reapply v18 generated renderer patch')
    if '# v18.1: renderer-view-matrix culling + direct add_render_entry capture + safe multi-pass render queue + overlay fonts.' not in sc:
        fail('v18.1 source self-check block missing')
    for token in ('v18.1: RT64 Inspector owns a fresh ImGui context', 'ConfigureLauncherFonts();'):
        if token not in u: fail('overlay font token missing: ' + token)

    # Verify the generated renderer too when it already exists. A fresh repo is
    # allowed to have no generated CPU C yet; OneClickBuild patches it after N64Recomp.
    cpu = root / 'runtime-recomp' / 'RecompiledFuncs'
    if cpu.is_dir():
        generated = []
        for path in sorted(cpu.glob('*.c')):
            try:
                text = path.read_text(encoding='utf-8-sig')
            except UnicodeDecodeError:
                continue
            if ('RECOMP_FUNC void func_8008B694(' in text or
                    'ROCKET-R GRAPHICS V18 SAFE MULTI-PASS RENDER QUEUE' in text):
                generated.append(path)
        if generated:
            if len(generated) != 1:
                fail(f'Expected one generated renderer source, found {len(generated)}')
            result = subprocess.run(
                [sys.executable, str(files['patcher']), '--root', str(root), '--verify'],
                check=False)
            if result.returncode != 0:
                fail('generated render-queue wrapper verification failed')
            print('[OK] Existing generated renderer v18 wrapper PASS.')
        elif args.verify_generated:
            fail('generated CPU sources exist but func_8008B694 was not found')

    print('[OK] Rocket-R Graphics v18.1 render-visibility/queue/font verification PASS.')


if __name__ == '__main__':
    main()
