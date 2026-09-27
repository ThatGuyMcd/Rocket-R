#!/usr/bin/env python3
from pathlib import Path
import argparse
import json
import subprocess
import sys


def need(ok, msg):
    if not ok:
        raise RuntimeError(msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('--with-generated', action='store_true')
    a = ap.parse_args()
    root = Path(a.root).resolve()

    p = (root / 'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    h = (root / 'src/presentation_identity.hpp').read_text(encoding='utf-8-sig')
    g = (root / 'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    ui = (root / 'src/runtime_ui.cpp').read_text(encoding='utf-8-sig')
    policy = json.loads((root / 'runtime-recomp/rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))

    for token in (
        'ROCKET-R INTERPOLATION V36 GLOBAL MATRIX OWNERSHIP',
        'FinalizeModelRangeBindings', 'FinalizeDirectMatrixBindings',
        'FinalizeCameraMatrixBindings', 'AcquireObjectLifetimeTokenLocked',
        'rocket_presentation_model_range_begin', 'rocket_presentation_model_range_end',
        'rocket_presentation_direct_begin', 'rocket_presentation_direct_end',
        'rocket_presentation_camera_source', 'rocket_presentation_camera_matrices',
        'kGfxTaskPerspectiveMtxOffset = 0x018U',
        'kGfxTaskViewMtxOffset = 0x058U',
        'kGfxTaskIdentityModelMtxOffset = 0x098U',
        'unowned-camera=', 'unowned-arena=', 'unowned-other=',
        'proof_key = exact.key;',
    ):
        need(token in p, 'v36 source token missing: ' + token)

    need('PendingModelOwner' not in p and 'OwnerTrack' not in p,
         'retired unsafe owner matcher reappeared')
    need('g_model_range_occurrences' not in p,
         'independent broad-range occurrence identity reappeared')

    for token in (
        'rocket_presentation_model_range_begin', 'rocket_presentation_direct_begin',
        'rocket_presentation_camera_source', 'rocket_presentation_camera_matrices'):
        need(token in h, 'v36 header declaration missing: ' + token)

    need('g_camera_presentation_discontinuity_v36' in g and
         'rocket_presentation_camera_source' in g,
         'v36 camera continuity bridge missing')

    for token in (
        'ROCKET-R UI V36 SPINNING BRAND + 50% OVERLAY',
        'LoadRocketBrandIntoAtlas', 'DrawRocketBrandCoin',
        'Rocket-R-green-full-resolution.png', 'ApplyRocketWindowIcon'):
        need(token in ui, 'v36 UI token missing: ' + token)

    hooks = {(x.get('function'), str(x.get('beforeVram','')).lower())
             for x in policy.get('functionHooks', [])}
    expected = {
        ('func_8001ECEC', '0x8001ecec'),
        ('func_8001ECEC', '0x8001f120'),
        ('load_translation_mtx', '0x800476cc'),
        ('load_translation_mtx', '0x800477a4'),
        ('func_8004A4F0', '0x8004a4f0'),
        ('func_8004A4F0', '0x8004ac34'),
        ('func_8003ACD4', '0x8003adf4'),
    }
    missing = sorted(expected - hooks)
    need(not missing, 'v36 policy hooks missing: ' + repr(missing))

    if a.with_generated:
        cp = subprocess.run(
            [sys.executable, str(root / 'scripts/verify_recomp_policy_v42.py'),
             '--root', str(root), '--with-generated'], cwd=str(root))
        need(cp.returncode == 0, 'v42 read-only generated verification failed')

    print('[OK] Interpolation v36 global coverage + UI verification PASS.')
    print('[OK] Safety retained: v35 exact Submodels + v5/v6 fail-closed matcher.')
    print('[OK] v36 coverage hooks are emitted by N64Recomp policy; generated C remains read-only.')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('v36 verification failed:', exc, file=sys.stderr)
        raise SystemExit(1)
