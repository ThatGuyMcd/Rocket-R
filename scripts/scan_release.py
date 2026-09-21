#!/usr/bin/env python3
from __future__ import annotations
import sys
from pathlib import Path

ROM_SUFFIXES = {'.z64', '.v64', '.n64'}
MAGICS = {bytes.fromhex('80371240'), bytes.fromhex('37804012'), bytes.fromhex('40123780')}


def main() -> int:
    if len(sys.argv) != 2:
        print('Usage: scan_release.py <directory>', file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    problems = []
    for path in root.rglob('*'):
        if not path.is_file():
            continue
        if path.suffix.lower() in ROM_SUFFIXES:
            problems.append(f'ROM extension: {path}')
            continue
        try:
            with path.open('rb') as f:
                first = f.read(4)
            if first in MAGICS:
                problems.append(f'N64 ROM header: {path}')
        except OSError:
            pass
    if problems:
        print('Release safety scan FAILED:', file=sys.stderr)
        for p in problems:
            print(' - ' + p, file=sys.stderr)
        return 1
    print('Release safety scan PASS: no N64 ROM files/headers detected.')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
