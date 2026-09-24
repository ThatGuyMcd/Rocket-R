#!/usr/bin/env python3
from pathlib import Path
import sys, zipfile, datetime, shutil

MARKERS = {
    'func_8008B694': 'RECOMP_FUNC void func_8008B694(uint8_t* rdram, recomp_context* ctx)',
    'add_render_entry': 'RECOMP_FUNC void add_render_entry(uint8_t* rdram, recomp_context* ctx)',
}

def find_generated(root: Path, marker: str):
    out = root / 'runtime-recomp' / 'RecompiledFuncs'
    if not out.is_dir():
        raise RuntimeError(f'Generated CPU directory missing: {out}')
    matches=[]
    for p in sorted(out.glob('*.c')):
        try:
            t=p.read_text(encoding='utf-8-sig')
        except Exception:
            continue
        if marker in t:
            matches.append(p)
    if len(matches)!=1:
        raise RuntimeError(f'Expected exactly one generated source for marker, found {len(matches)}: {marker}')
    return matches[0]

def main():
    root = Path(sys.argv[1] if len(sys.argv)>1 else Path(__file__).resolve().parents[2]).resolve()
    if not (root/'src').is_dir():
        raise RuntimeError(f'Rocket-R root not found: {root}')
    renderer=find_generated(root, MARKERS['func_8008B694'])
    addsrc=find_generated(root, MARKERS['add_render_entry'])
    wanted=[
        renderer,
        addsrc,
        root/'src'/'presentation_identity.cpp',
        root/'src'/'presentation_identity.hpp',
        root/'src'/'widescreen_culling.cpp',
        root/'src'/'graphics_enhancements.cpp',
        root/'scripts'/'OneClickBuild.ps1',
        root/'scripts'/'self_check.py',
        root/'scripts'/'patch_popin_diagnostics_generated.py',
    ]
    missing=[p for p in wanted if not p.is_file()]
    # The header/diagnostic patcher are useful but not mandatory.
    mandatory={renderer,addsrc,root/'src'/'presentation_identity.cpp',root/'src'/'widescreen_culling.cpp',root/'src'/'graphics_enhancements.cpp',root/'scripts'/'OneClickBuild.ps1',root/'scripts'/'self_check.py'}
    hard=[p for p in missing if p in mandatory]
    if hard:
        raise RuntimeError('Required files missing:\n  ' + '\n  '.join(str(x) for x in hard))

    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    outdir=root/'build'/'diagnostics'
    outdir.mkdir(parents=True,exist_ok=True)
    zip_path=outdir/f'Rocket-R-render-queue-snapshot-{stamp}.zip'
    manifest=[]
    with zipfile.ZipFile(zip_path,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for p in wanted:
            if not p.is_file():
                continue
            rel=p.relative_to(root).as_posix()
            z.write(p,rel)
            manifest.append(rel)
        info=(
            'Rocket-R render queue snapshot\n'
            f'Root: {root}\n'
            f'Renderer generated source: {renderer.relative_to(root)}\n'
            f'add_render_entry generated source: {addsrc.relative_to(root)}\n'
            '\nFiles:\n' + ''.join(f'  {x}\n' for x in manifest)
        )
        z.writestr('SNAPSHOT-INFO.txt',info)
    print('[OK] Found func_8008B694:', renderer)
    print('[OK] Found add_render_entry:', addsrc)
    print('[OK] Snapshot created:', zip_path)
    print('UPLOAD THIS ZIP BACK TO CHAT:')
    print(zip_path)

if __name__=='__main__':
    try:
        main()
    except Exception as exc:
        print('[ERROR]',exc,file=sys.stderr)
        raise SystemExit(1)
