#!/usr/bin/env python3
"""Check release packages and record their source and artifact hashes.

Run on Linux or WSL with unsquashfs installed so both AppImages can be inspected.
This checks packaging, not gameplay or device compatibility.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tarfile
import tempfile
import tomllib
import zipfile

from stage_release_docs import release_documents
from rocket_sdk import validate_package, MODULES


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def rom_check(name: str, data: bytes) -> None:
    require(Path(name).suffix.lower() not in {'.z64', '.v64', '.n64', '.rom'}, f'ROM filename: {name}')
    require(data[:4] not in {bytes.fromhex(s) for s in ('80371240', '37804012', '40123780')},
            f'ROM header: {name}')


def elf(data: bytes, machine: int, appimage: bool = False) -> None:
    require(data[:6] == b'\x7fELF\x02\x01', 'Expected little-endian ELF64')
    require(struct.unpack_from('<H', data, 18)[0] == machine, 'Wrong ELF CPU architecture')
    if appimage:
        require(data[8:11] == b'AI\x02', 'Expected type-2 AppImage')


def source_files(root: Path) -> list[Path]:
    """Include new owned source files even when they have not been committed."""
    excluded = {'.git', 'build', 'dist', 'extern', 'generated', '__pycache__',
                '.vs', '.vscode', '.idea', 'RecompiledFuncs', 'RecompiledRSP', 'RecompiledPatches', 'Live_Check_Windows'}
    result = []
    for directory, folders, names in os.walk(root):
        folders[:] = sorted(name for name in folders if name not in excluded)
        for name in sorted(names):
            path = Path(directory) / name
            if path.suffix.lower() in {'.pyc', '.log', '.dmp', '.jks', '.keystore'}:
                continue
            if 'roms' in path.relative_to(root).parts and name != 'README.md':
                continue
            result.append(path)
    return sorted(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output-dir', type=Path, help='Package directory; defaults to <root>/dist')
    args = parser.parse_args()
    root = args.root.resolve()
    dist = (args.output_dir or root / 'dist').resolve()
    version = (root / 'VERSION').read_text(encoding='utf-8-sig').strip()
    prefix = f'Rocket-R-{version}'
    documents = {name: path.read_bytes() for name, path in release_documents(root).items()}
    artifacts = []
    binaries = {}
    camera_payload = None
    camera_manifest = root / 'modding/examples/modern-camera/mod.toml'
    if camera_manifest.is_file():
        path = dist / 'rocket_modern_camera.nrm'
        expected = tomllib.loads(camera_manifest.read_text(encoding='utf-8'))['manifest']
        with zipfile.ZipFile(path) as package:
            require(package.testzip() is None, 'Corrupt camera mod package')
            manifest = json.loads(package.read('mod.json'))
            require(manifest['id'] == expected['id'] and manifest['version'] == expected['version'],
                    'Camera mod package does not match the source manifest')
            for item in package.infolist():
                if not item.is_dir():
                    with package.open(item) as stream:
                        rom_check(item.filename, stream.read(4))
        camera_payload = path.read_bytes()
        artifacts.append(path)

    def embedded_mod(data: bytes, platform: str) -> None:
        if camera_payload is not None:
            require(camera_payload in data, f'Outdated or missing built-in camera mod: {platform}')

    def docs(read, base: str = '') -> None:
        for name, expected in documents.items():
            require(read(base + name) == expected, f'Outdated or missing packaged document: {base}{name}')

    for platform in ('Windows-x64.zip', 'Android-arm64-v8a.apk'):
        path = dist / f'{prefix}-{platform}'
        with zipfile.ZipFile(path) as package:
            require(package.testzip() is None, f'Corrupt archive: {path.name}')
            for item in package.infolist():
                if not item.is_dir():
                    with package.open(item) as stream:
                        rom_check(item.filename, stream.read(4))
            if platform.startswith('Windows'):
                docs(package.read)
                data = package.read('Rocket-R.exe')
                require(data[:2] == b'MZ', 'Missing Windows executable')
                pe = struct.unpack_from('<I', data, 0x3c)[0]
                require(data[pe:pe+4] == b'PE\0\0', 'Invalid PE header')
                require(struct.unpack_from('<H', data, pe + 4)[0] == 0x8664, 'Expected Windows x64')
                require(struct.unpack_from('<H', data, pe + 24 + 68)[0] == 2, 'Expected Windows GUI subsystem')
                for name in ('SDL2.dll', 'dxcompiler.dll', 'dxil.dll', 'assets/ui/Rocket-R-green-full-resolution.png'):
                    require(bool(package.read(name)), f'Missing runtime file: {name}')
                binaries['Windows-x64'] = hashlib.sha256(data).hexdigest()
                embedded_mod(data, 'Windows-x64')
            else:
                docs(package.read, 'assets/rocket-r-docs/')
                libs = {name for name in package.namelist() if name.startswith('lib/') and name.endswith('.so')}
                require(libs == {'lib/arm64-v8a/libmain.so', 'lib/arm64-v8a/libSDL2.so'}, 'Unexpected Android ABI/libraries')
                for name in sorted(libs):
                    data = package.read(name)
                    elf(data, 183)
                    offset = struct.unpack_from('<Q', data, 32)[0]
                    size, count = struct.unpack_from('<HH', data, 54)
                    loads = []
                    for index in range(count):
                        header = struct.unpack_from('<IIQQQQQQ', data, offset + index * size)
                        if header[0] == 1:
                            loads.append(header)
                            require(header[7] >= 16384 and (header[2] - header[3]) % 16384 == 0,
                                    f'Android library is not 16 KiB aligned: {name}')
                    require(bool(loads), f'No ELF load segments: {name}')
                    binaries[name] = hashlib.sha256(data).hexdigest()
                    if name.endswith('/libmain.so'):
                        embedded_mod(data, 'Android-arm64-v8a')
        artifacts.append(path)

    for arch, machine in (('x86_64', 62), ('aarch64', 183)):
        path = dist / f'{prefix}-Linux-{arch}.AppImage'
        data = path.read_bytes()
        elf(data, machine, True)
        # Select a valid SquashFS superblock, not an arbitrary magic-string match.
        offsets = []
        start = 0
        while (offset := data.find(b'hsqs', start)) >= 0:
            if offset + 96 <= len(data):
                major, minor = struct.unpack_from('<HH', data, offset + 28)
                used = struct.unpack_from('<Q', data, offset + 40)[0]
                if (major, minor) == (4, 0) and 96 <= used <= len(data) - offset:
                    offsets.append(offset)
            start = offset + 4
        require(len(offsets) == 1, f'Expected one SquashFS payload in {path.name}')
        with tempfile.TemporaryDirectory(prefix='rocket-release-check-') as temporary:
            extracted = Path(temporary) / 'AppDir'
            subprocess.run(['unsquashfs', '-q', '-o', str(offsets[0]), '-d', str(extracted), str(path)], check=True)
            for file in extracted.rglob('*'):
                if file.is_file():
                    with file.open('rb') as stream:
                        rom_check(file.name, stream.read(4))
            for name in ('AppRun', 'usr/bin/Rocket-R'):
                require(os.access(extracted / name, os.X_OK), f'Not executable: {name}')
            binary = (extracted / 'usr/bin/Rocket-R').read_bytes()
            elf(binary, machine)
            binaries[f'Linux-{arch}'] = hashlib.sha256(binary).hexdigest()
            embedded_mod(binary, f'Linux-{arch}')
            docs(lambda name: (extracted / 'usr/share/doc/rocket-r' / name).read_bytes())
            require(not any(p.name.startswith(('libstdc++.so', 'libgcc_s.so')) for p in extracted.rglob('*')),
                    'AppImage must use the host C++ driver runtime')
        artifacts.append(path)
        kinds = ('Portable', 'SteamDeck') if arch == 'x86_64' else ('Portable',)
        for kind in kinds:
            archive = dist / f'{prefix}-Linux-{arch}-{kind}.tar.gz'
            with tarfile.open(archive) as package:
                members = package.getmembers()
                for member in members:
                    if member.isfile():
                        with package.extractfile(member) as stream:
                            rom_check(member.name, stream.read(4))
                suffix = '.AppImage' if kind == 'Portable' else '/usr/bin/Rocket-R'
                targets = [m for m in members if m.isfile() and m.name.endswith(suffix)]
                require(len(targets) == 1 and targets[0].mode & 0o111, f'Missing executable in {archive.name}')
                packed = package.extractfile(targets[0]).read()
                require(packed == (data if kind == 'Portable' else binary), f'Binary mismatch in {archive.name}')
                if kind == 'SteamDeck':
                    base = targets[0].name.removesuffix('usr/bin/Rocket-R') + 'usr/share/doc/rocket-r/'
                    docs(lambda name: package.extractfile(name).read(), base)
                    require(not any(Path(m.name).name.startswith(('libstdc++.so', 'libgcc_s.so')) for m in members),
                            'Steam Deck package must use the host C++ driver runtime')
            artifacts.append(archive)

    def git(*arguments: str) -> str:
        return subprocess.check_output(['git', *arguments], cwd=root, text=True).strip()

    sdk_path = dist / f'{prefix}-SDK2.zip'
    if sdk_path.exists():
        with zipfile.ZipFile(sdk_path) as package:
            require(package.testzip() is None, 'Corrupt SDK archive')
            metadata = json.loads(package.read('sdk.json'))
            require(metadata['sdk'] == 2 and metadata['modules'] == MODULES, 'SDK module manifest is outdated')
            require(package.read('modding/include/rocket/mod.h') == (root/'modding/include/rocket/mod.h').read_bytes(),
                    'SDK 1 header changed in SDK bundle')
            for name, value in metadata['symbols_sha256'].items():
                require(hashlib.sha256(package.read(name)).hexdigest() == value, 'SDK symbol checksum mismatch')
                require(package.read(name) == (root/'build/generated'/Path(name).name).read_bytes(), 'SDK symbols do not match this build')
            for item in package.infolist():
                if not item.is_dir(): rom_check(item.filename, package.read(item.filename)[:4])
            for name, machine in (('tools/linux-x86_64/RecompModTool', 62), ('tools/linux-aarch64/RecompModTool', 183)):
                elf(package.read(name), machine)
                require((package.getinfo(name).external_attr >> 16) & 0o111, 'SDK native tool is not executable')
            require(package.read('tools/RecompModTool.exe')[:2] == b'MZ', 'SDK Windows tool missing')
            require(package.read('docs/SDK2.md') == (root/'docs/SDK2.md').read_bytes(), 'SDK guide outdated')
        artifacts.append(sdk_path)
        workshop = dist/'rocket_workshop.nrm'
        actual_workshop = validate_package(workshop)
        expected_workshop = tomllib.loads((root/'modding/examples/workshop/mod.toml').read_text())['manifest']
        require(actual_workshop['api'] == 2, 'Workshop needs SDK 2 metadata')
        require(actual_workshop['id'] == expected_workshop['id'] and
                actual_workshop['version'] == expected_workshop['version'],
                'Workshop package does not match the source manifest')
        artifacts.append(workshop)

    hashes = {p.relative_to(root).as_posix(): digest(p) for p in source_files(root)}
    record = {
        'version': version, 'createdAt': datetime.now(timezone.utc).isoformat(),
        'baseCommit': git('rev-parse', 'HEAD'), 'branch': git('branch', '--show-current'),
        'workingTreeDirty': bool(git('status', '--porcelain')),
        'sourceNote': 'The source hashes include uncommitted and untracked owned files; baseCommit alone does not identify this build.',
        'sourceSha256': hashes,
        'sourceManifestSha256': hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest(),
        'binarySha256': binaries,
        'checks': ['ZIP integrity', 'ROM scans', 'CPU architectures', 'Windows GUI subsystem',
                   'Android 16 KiB ELF alignment', 'AppImage extraction', 'portable binary equality',
                   'current guides and dependency notices'] +
                  (['SDK modules, frozen SDK 1 header, symbols, native tools and Workshop package'] if sdk_path.exists() else []) +
                  (['camera mod version and embedded package equality'] if camera_payload is not None else []),
        'coverageNote': 'These are packaging checks. See docs/TESTING.md for separate test and gameplay coverage. APK signing is checked by the Android builder.',
        'artifacts': [{'file': p.name, 'bytes': p.stat().st_size, 'sha256': digest(p)} for p in artifacts],
    }
    output = dist / f'{prefix}-build-info.json'
    output.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    sums = ''.join(f'{digest(p)}  {p.name}\n' for p in sorted(artifacts + [output]))
    (dist / f'{prefix}-SHA256SUMS.txt').write_text(sums, encoding='utf-8')
    print(f'PASS: {len(artifacts)} packages; {len(hashes)} owned source files recorded in {output}')


if __name__ == '__main__':
    main()
