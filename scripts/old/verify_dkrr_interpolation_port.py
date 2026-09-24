#!/usr/bin/env python3
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path

def fail(msg: str) -> None:
    raise SystemExit("ERROR: " + msg)

def main() -> int:
    ap=argparse.ArgumentParser(); ap.add_argument('--root', required=True); ns=ap.parse_args()
    root=Path(ns.root).resolve()
    required=[
        root/'src/presentation_identity.cpp', root/'src/presentation_identity.hpp',
        root/'patches/rt64/0008-rocket-dkrr-semantic-presentation-identities.patch',
        root/'runtime-recomp/rocket.us.recomp-policy.json', root/'CMakeLists.txt',
        root/'src/rt64_renderer.cpp', root/'patches/manifest.json',
    ]
    for p in required:
        if not p.is_file(): fail(f'missing {p.relative_to(root)}')
    hpp=(root/'src/presentation_identity.hpp').read_text(encoding='utf-8')
    if '#include "recomp.h"' not in hpp: fail('presentation_identity.hpp must include the real librecomp recomp_context definition')
    if 'struct recomp_context;' in hpp: fail('presentation_identity.hpp must not forward-declare recomp_context; recomp.h defines it as a typedef')
    cm=(root/'CMakeLists.txt').read_text(encoding='utf-8')
    if cm.count('src/presentation_identity.cpp') != 1: fail('CMake presentation_identity source must appear exactly once')
    rr=(root/'src/rt64_renderer.cpp').read_text(encoding='utf-8')
    for marker in ['#include "presentation_identity.hpp"', 'TaskIdentityScope identity_scope']:
        if marker not in rr: fail(f'rt64_renderer missing marker {marker}')
    policy=json.loads((root/'runtime-recomp/rocket.us.recomp-policy.json').read_text(encoding='utf-8'))
    hooks=policy.get('functionHooks',[])
    expected={
        ('update_gfx_context','0x80046D20'):'rocket_presentation_frame_begin',
        ('add_render_entry','0x8008B24C'):'rocket_presentation_render_entry',
        ('schedule_gfx_task','0x800473F0'):'rocket_presentation_task_submitted',
    }
    for key,marker in expected.items():
        found=[h for h in hooks if (h.get('function'),str(h.get('beforeVram','')).upper()) == (key[0],key[1].upper()) and marker in h.get('text','')]
        if len(found)!=1: fail(f'expected exactly one hook {key[0]} {key[1]} -> {marker}, got {len(found)}')
    manifest=json.loads((root/'patches/manifest.json').read_text(encoding='utf-8'))
    rt=next((d for d in manifest.get('dependencies',[]) if d.get('name')=='RT64'),None)
    if rt is None: fail('RT64 manifest entry missing')
    rel='patches/rt64/0008-rocket-dkrr-semantic-presentation-identities.patch'
    rec=[p for p in rt.get('patches',[]) if p.get('path')==rel]
    if len(rec)!=1: fail('semantic RT64 patch manifest record missing/duplicated')
    digest=hashlib.sha256((root/rel).read_bytes()).hexdigest()
    if rec[0].get('sha256')!=digest: fail('semantic RT64 patch manifest hash mismatch')
    selfcheck=(root/'scripts/self_check.py').read_text(encoding='utf-8')
    if 'DKR-R interpolation port v4.2 semantic RT64 patch' not in selfcheck:
        fail('self_check.py v4.2 semantic RT64 integrity guard missing')
    # If this Rocket-R revision still uses an exact RT64 patch-count guard,
    # verify that it matches the manifest we just installed. Newer revisions
    # are allowed to use a flexible/count-independent validator.
    import re
    exact=re.search(r'len\(rt64_manifest\.get\("patches", \[\]\)\) == (\d+)', selfcheck)
    if exact is not None and int(exact.group(1)) != len(rt.get('patches', [])):
        fail('self_check.py RT64 exact patch-count guard does not match current manifest')
    bootstrap=(root/'scripts/bootstrap_dependencies.py').read_text(encoding='utf-8')
    if 'apply_interpolation_overhaul.py' in bootstrap: fail('stale v1/v2/v3 interpolation transformer still active in bootstrap')
    print('[OK] Rocket-R DKR-R semantic presentation identity port v4.2 verification PASS.')
    return 0

if __name__=='__main__': raise SystemExit(main())
