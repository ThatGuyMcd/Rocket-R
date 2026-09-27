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

    source = (root / 'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    header = (root / 'src/presentation_identity.hpp').read_text(encoding='utf-8-sig')
    policy = json.loads((root / 'runtime-recomp/rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))

    for marker in (
        'constexpr std::uint64_t kMaximumTrackAge = 1U;',
        'constexpr float kMaximumTrackDistance = 384.0F;',
        'BuildSharedMatrixSamples',
        'MatchSharedMatrixSamples',
        'SubmittedFrame& frame = g_submitted.front();',
        'g_active_task_fail_closed = true;',
        'ROCKET-R SKYBOX INTERPOLATION V33',
        'ROCKET-R INTERPOLATION V35 SPECIFIC SUBMODEL MATRICES + RANGE-STABLE SKY',
        'rocket_presentation_submodel_matrix_begin',
        'rocket_presentation_submodel_matrix_end',
        'FinalizeSpecificMatrixBindings',
        'binding = IgnoredBinding();',
    ):
        need(marker in source, 'v35 safety/source marker missing: ' + marker)

    need('PendingModelOwner' not in source and 'OwnerTrack' not in source,
         'retired unsafe owner matcher reappeared')

    hooks = {(x.get('function'), str(x.get('beforeVram','')).lower())
             for x in policy.get('functionHooks', [])}
    need(('func_8001EA18', '0x8001ea18') in hooks,
         'v35 begin hook is not in recomp policy')
    need(('func_8001EA18', '0x8001ece4') in hooks,
         'v35 end hook is not in recomp policy')
    need('rocket_presentation_submodel_matrix_begin' in header and
         'rocket_presentation_submodel_matrix_end' in header,
         'v35 presentation header declarations missing')

    if a.with_generated:
        cp = subprocess.run(
            [sys.executable, str(root / 'scripts/verify_recomp_policy_v42.py'),
             '--root', str(root), '--with-generated'], cwd=str(root))
        need(cp.returncode == 0, 'v42 read-only generated verification failed')

    print('[OK] Rocket-R interpolation v35 verification PASS.')
    print('[OK] Stable v5/v6 matcher, strict FIFO sidecar, V33 RT64 bridge and fail-closed unknown matrices remain intact.')
    print('[OK] v35 exact Submodel hooks are emitted by N64Recomp policy; no generated C patcher is used.')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('Interpolation v35 verification failed:', exc, file=sys.stderr)
        raise SystemExit(1)
