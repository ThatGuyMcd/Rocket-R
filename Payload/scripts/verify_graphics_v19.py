#!/usr/bin/env python3
from pathlib import Path
import argparse
import ast


def fail(msg):
    print('[ERROR] ' + msg)
    raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()

    paths = {
        'culling': root/'src/widescreen_culling.cpp',
        'graphics': root/'src/graphics_enhancements.cpp',
        'presentation': root/'src/presentation_identity.cpp',
        'patcher': root/'scripts/patch_render_queue_generated.py',
        'oneclick': root/'scripts/OneClickBuild.ps1',
        'selfcheck': root/'scripts/self_check.py',
    }
    for label, path in paths.items():
        if not path.is_file(): fail(f'Missing {label}: {path}')
    t = {k:p.read_text(encoding='utf-8-sig') for k,p in paths.items()}

    for token in ('kNoSideCullRadiusBits = 0x7F7FFFFFU','distance-only visibility active'):
        if token not in t['culling']: fail('distance-only culling token missing: '+token)
    for forbidden in ('kTargetFrustumGuard = 1.20F','v18 renderer-view-matrix viewport guard active'):
        if forbidden in t['culling']: fail('retired v18 culling heuristic remains: '+forbidden)

    for token in ('static_cast<std::uint32_t>(context->r7)','draw_distance_multiplier',
                  'authored_distance * multiplier'):
        if token not in t['graphics']: fail('Draw Distance slider/r7 path missing: '+token)

    for token in ('RocketBuildGlobalRenderOrder','RocketRenderHeapSort','g_expanded_render_order',
                  'rocket_render_queue_finalize_capture','rocket_render_queue_final_pass',
                  '[render-queue] GLOBAL'):
        if token not in t['presentation']: fail('v19 global queue token missing: '+token)
    for forbidden in ('g_render_queue_replay_active','rocket_render_queue_replay_end',
                      'replaying safely in %zu retail-sized passes'):
        if forbidden in t['presentation']: fail('retired v18 queue token remains: '+forbidden)

    for token in ('ROCKET-R GRAPHICS V19 GLOBAL RENDER QUEUE','rocket_capture_func_8008B694',
                  'rocket_draw_func_8008B694','rocket_render_queue_prepare_empty_final'):
        if token not in t['patcher']: fail('v19 generated patcher token missing: '+token)
    if 'patch_render_queue_generated.py' not in t['oneclick']:
        fail('OneClickBuild no longer patches generated queue after N64Recomp')
    try:
        sc_tree = ast.parse(t['selfcheck'])
    except SyntaxError as exc:
        fail(f'self_check.py is invalid Python: {exc}')
    sc_main = next((n for n in sc_tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == 'main'), None)
    if sc_main is None:
        fail('self_check.py no longer contains def main()')
    sc_names = {n.id for n in ast.walk(sc_main) if isinstance(n, ast.Name)}
    if '_v19_culling' not in sc_names or '_v19_required' not in sc_names:
        fail('v19.2 graphics self-check block is not active inside main()')
    stale_names = sorted(name for name in sc_names if name.startswith(('_v16', '_v17', '_v181', '_v182', '_v183')))
    if stale_names:
        fail('active legacy graphics self-check variables remain: ' + ', '.join(stale_names[:8]))
    sc_segment = ast.get_source_segment(t['selfcheck'], sc_main) or ''
    if 'scan_checked_source(root)' not in sc_segment or 'return 0' not in sc_segment:
        fail('normal self-check tail was not restored')
    runner_ok = any(isinstance(stmt, ast.If) and '__name__' in (ast.get_source_segment(t['selfcheck'], stmt.test) or '') and
                    '__main__' in (ast.get_source_segment(t['selfcheck'], stmt.test) or '') for stmt in sc_tree.body)
    if not runner_ok:
        fail('normal __main__ self-check runner is missing')


    print('[OK] Rocket-R Graphics v19.2 distance-only visibility/global render-order verification PASS.')
    print('[OK] Draw Distance slider preserved; v18 side-frustum heuristic and independent overflow-pass ordering removed.')

if __name__ == '__main__':
    main()
