#!/usr/bin/env python3
from __future__ import annotations
from pathlib import Path
import argparse
import hashlib
import json
import re
import sys

MARKER='ROCKET-R SKYBOX INTERPOLATION V33'
PATCH=Path('patches/rt64/0008-rocket-dkrr-semantic-presentation-identities.patch')

def fail(msg):
    raise RuntimeError(msg)

def sha(path:Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); args=ap.parse_args()
    root=Path(args.root).resolve()
    one=(root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    if one.count('patch_render_capacity_v32_generated.py')!=1:
        fail('v32 single-live-queue OneClick integration is not exactly one invocation')
    for old in ('patch_render_capacity_v31_generated.py','patch_render_capacity_v30_generated.py','patch_render_queue_v28_generated.py'):
        if re.search(r'Invoke-Python[^\n]*'+re.escape(old),one): fail('retired queue system active: '+old)
    qp=(root/'scripts/patch_render_capacity_v32_generated.py').read_text(encoding='utf-8-sig')
    for m in ('ROCKET-R GRAPHICS V32 SINGLE LIVE RENDER QUEUE','0X8060','0X8070','0X8000'):
        if m not in qp: fail('v32 patcher marker missing: '+m)
    cpp=(root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    hpp=(root/'src/presentation_identity.hpp').read_text(encoding='utf-8-sig')
    for m in (MARKER,'rocket_presentation_background_begin','rocket_presentation_background_end',
              'rocket_presentation_background_display_list','BackgroundCommandRange',
              'g_active_background_ranges = std::move(frame.background_ranges)',
              'SubmittedFrame& frame = g_submitted.front()'):
        if m not in cpp and m not in hpp: fail('v33 presentation marker missing: '+m)
    for m in ('kMaximumTrackAge = 1U','kMaximumTrackDistance = 384.0F','BuildSharedMatrixSamples'):
        if m not in cpp: fail('locked world interpolation marker missing: '+m)
    policy=json.loads((root/'runtime-recomp/rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))
    specs=(('func_8008B594','0x8008B594','rocket_presentation_background_begin'),
           ('func_80046D58','0x80046D58','rocket_presentation_background_end'))
    for fn,addr,tok in specs:
        ms=[h for h in policy.get('functionHooks',[]) if isinstance(h,dict) and h.get('function')==fn and tok in str(h.get('text',''))]
        if len(ms)!=1 or ms[0].get('beforeVram')!=addr: fail('v33 hook invalid: '+fn)
    patch=(root/PATCH).read_text(encoding='utf-8-sig')
    for m in (MARKER,'rocketBackgroundPresentationStack','rocket_presentation_background_display_list',
              'state->rsp->matrixId(','state->rsp->setModelViewProjChanged(true)','state->rsp->popMatrixId(1, false)'):
        if m not in patch: fail('RT64 v33 patch marker missing: '+m)
    manifest=json.loads((root/'patches/manifest.json').read_text(encoding='utf-8-sig'))
    rt=next((d for d in manifest.get('dependencies',[]) if d.get('name')=='RT64'),None)
    if rt is None: fail('RT64 manifest entry missing')
    entries=[p for p in rt.get('patches',[]) if p.get('path')==str(PATCH).replace('\\','/')]
    if len(entries)!=1: fail('semantic patch manifest entry missing/duplicated')
    if entries[0].get('sha256')!=sha(root/PATCH): fail('semantic patch manifest SHA mismatch')
    print('[OK] v33 skybox interpolation verification PASS; v32 single-live-queue baseline remains active.')
    return 0

if __name__=='__main__':
    try: raise SystemExit(main())
    except Exception as exc:
        print('v33 verification FAILED: '+str(exc),file=sys.stderr); raise SystemExit(1)
