#!/usr/bin/env python3
from pathlib import Path
import argparse
import re
import sys

GOOD = '"retail %zu-entry list; replaying safely in %zu retail-sized passes.\\n",'
BAD = re.compile(r'"retail %zu-entry list; replaying safely in %zu retail-sized passes\.\r?\n",')
MARKER = '# v18.3: render-queue expansion diagnostic must remain a valid escaped C++ string literal.'

def fail(msg):
    print('[ERROR] ' + msg)
    raise SystemExit(1)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    p_path = root / 'src' / 'presentation_identity.cpp'
    sc_path = root / 'scripts' / 'self_check.py'
    patcher = root / 'scripts' / 'patch_render_queue_generated.py'
    for path in (p_path, sc_path, patcher):
        if not path.is_file(): fail(f'Missing required file: {path}')
    p = p_path.read_text(encoding='utf-8-sig')
    sc = sc_path.read_text(encoding='utf-8-sig')
    for token in ('ExpandedRenderEntry','rocket_render_queue_capture','rocket_render_queue_begin_batch','rocket_render_queue_next','[render-queue] EXPANDED'):
        if token not in p: fail('v18 queue baseline missing: ' + token)
    if BAD.search(p): fail('malformed physical newline remains inside queue diagnostic C++ string')
    if GOOD not in p: fail('correct escaped queue diagnostic literal is missing')
    if MARKER not in sc or '_v183_queue_status_literal' not in sc:
        fail('v18.3 source self-check guard is missing')
    print('[OK] Rocket-R Graphics v18.3 queue-string compile hotfix verification PASS.')

if __name__ == '__main__': main()
