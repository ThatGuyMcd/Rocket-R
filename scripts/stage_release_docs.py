#!/usr/bin/env python3
"""Stage the maintained guides and original dependency licence notices."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil

DOCS = (
    'README.md', 'CHANGELOG.md', 'LICENSE', 'LICENSE.md', 'THIRD_PARTY.md',
    'docs/BUILDING.md', 'docs/CONTROLS.md', 'docs/DEVELOPMENT.md',
    'docs/ARCHITECTURE.md', 'docs/TROUBLESHOOTING.md',
    'docs/CUSTOM_SHADERS.md', 'docs/TESTING.md',
    'docs/modding.md',
    'docs/custom-shaders/example-rocket-postprocess.hlsl',
)
DEPENDENCIES = ('rt64', 'n64-modern-runtime', 'sdl2')


def release_documents(root: Path) -> dict[str, Path]:
    files = {name: root / name for name in DOCS}
    for component in DEPENDENCIES:
        base = root / 'extern' / component
        if not base.is_dir():
            raise FileNotFoundError(f'Missing dependency notices: {base}')
        count = 0
        for directory, folders, names in os.walk(base):
            folders[:] = sorted(n for n in folders if n not in
                                {'.git', 'build', '__pycache__', 'node_modules'})
            for name in sorted(names):
                upper = name.upper()
                if upper in {'LICENSE', 'LICENCE', 'COPYING', 'NOTICE'} or upper.startswith(
                        ('LICENSE.', 'LICENCE.', 'COPYING.', 'NOTICE.')):
                    source = Path(directory) / name
                    relative = source.relative_to(base).as_posix()
                    files[f'licenses/{component}/{relative}'] = source
                    count += 1
        if not count:
            raise FileNotFoundError(f'No licence notices found in {base}')
    for source in files.values():
        if not source.is_file():
            raise FileNotFoundError(source)
    return files


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    files = release_documents(args.root.resolve())
    for relative, source in files.items():
        destination = args.output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    print(f'Staged {len(files)} documentation and licence files in {args.output}')


if __name__ == '__main__':
    main()
