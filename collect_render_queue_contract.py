from __future__ import annotations
from pathlib import Path
import re, subprocess, sys, time, zipfile

TARGET_FUNCTIONS = (
    'func_8001E954',
    'func_8001ECEC',
    'func_80020134',
    'add_render_entry',
    'func_8008B694',
)
DISASM_RANGES = (
    ('func_8001E954', 0x8001E954, 0x8001EA18),
    ('func_8001ECEC', 0x8001ECEC, 0x8001F128),
    ('func_80020134', 0x80020134, 0x80020714),
    ('add_render_entry', 0x8008B24C, 0x8008B3DC),
    ('func_8008B694', 0x8008B694, 0x8008BCE8),
)
QUEUE_PATTERNS = (
    re.compile(r'-0X0?D00\b', re.I),     # D_800AF300 via 0x800B - 0xD00
    re.compile(r'-0X2500\b', re.I),      # D_800ADB00 via 0x800B - 0x2500
    re.compile(r'800AF300', re.I),
    re.compile(r'800ADB00', re.I),
    re.compile(r'D_800AF300'),
    re.compile(r'D_800ADB00'),
)


def fail(msg: str):
    raise RuntimeError(msg)


def locate_root() -> Path:
    candidates=[]
    if len(sys.argv)>1: candidates.append(Path(sys.argv[1]))
    candidates += [Path.cwd(), Path(__file__).resolve().parent]
    for c in candidates:
        r=c.resolve()
        if (r/'runtime-recomp/RecompiledFuncs').is_dir() and (r/'scripts/OneClickBuild.ps1').is_file():
            return r
    fail(r'Could not locate Rocket-R. Run this from D:\Rocket-R after a successful OneClick generation/build.')


def extract_function(text: str, name: str):
    # N64Recomp normally emits: RECOMP_FUNC void name(uint8_t* rdram, recomp_context* ctx) {
    m=re.search(r'RECOMP_FUNC\s+void\s+'+re.escape(name)+r'\s*\([^)]*\)\s*\{', text)
    if not m:
        return None
    brace=text.find('{',m.start())
    depth=0
    for pos in range(brace,len(text)):
        ch=text[pos]
        if ch=='{': depth+=1
        elif ch=='}':
            depth-=1
            if depth==0:
                return text[m.start():pos+1]+'\n'
    return None


def write_generated_extracts(root: Path, out: Path):
    generated=root/'runtime-recomp/RecompiledFuncs'
    cfiles=sorted(generated.glob('*.c'))
    if not cfiles:
        fail('No generated CPU .c files found. Run OneClick through N64Recomp generation first.')
    combined=[]
    found={name:False for name in TARGET_FUNCTIONS}
    queue_hits=[]
    function_index=[]
    for path in cfiles:
        text=path.read_text(encoding='utf-8', errors='replace')
        for name in TARGET_FUNCTIONS:
            if found[name]: continue
            block=extract_function(text,name)
            if block:
                found[name]=True
                dest=out/f'generated-{name}.c.txt'
                dest.write_text(f'// Source: {path.relative_to(root)}\n\n'+block,encoding='utf-8')
                function_index.append(f'{name}: {path.relative_to(root)}')
        lines=text.splitlines()
        for i,line in enumerate(lines,1):
            if any(p.search(line) for p in QUEUE_PATTERNS):
                lo=max(1,i-5); hi=min(len(lines),i+5)
                context='\n'.join(f'{j:6d}: {lines[j-1]}' for j in range(lo,hi+1))
                queue_hits.append(f'===== {path.relative_to(root)} : line {i} =====\n{context}\n')
    (out/'generated-function-index.txt').write_text('\n'.join(function_index)+'\n',encoding='utf-8')
    (out/'ALL-generated-render-queue-references.txt').write_text('\n'.join(queue_hits),encoding='utf-8')
    missing=[name for name,ok in found.items() if not ok]
    (out/'missing-generated-functions.txt').write_text(('None\n' if not missing else '\n'.join(missing)+'\n'),encoding='utf-8')
    print('[OK] Generated function extraction: ' + ', '.join(name for name,ok in found.items() if ok))
    if missing:
        print('[WARN] Generated functions not found: ' + ', '.join(missing))
    print(f'[OK] Queue-reference contexts: {len(queue_hits)}')


def windows_to_wsl(path: Path) -> str | None:
    s=str(path.resolve())
    m=re.match(r'^([A-Za-z]):\\(.*)$',s)
    if not m: return None
    drive=m.group(1).lower(); rest=m.group(2).replace('\\','/')
    return f'/mnt/{drive}/{rest}'


def collect_disassembly(root: Path, out: Path):
    elf=root/'extern/rocket-decomp/build/us/NSUE.elf'
    if not elf.is_file():
        (out/'ELF-DISASSEMBLY-NOT-COLLECTED.txt').write_text(
            f'Expected ELF was not found at: {elf}\nGenerated C extracts were still collected.\n',encoding='utf-8')
        print('[WARN] NSUE.elf not present; skipped MIPS disassembly.')
        return
    wsl_elf=windows_to_wsl(elf)
    if not wsl_elf:
        (out/'ELF-DISASSEMBLY-NOT-COLLECTED.txt').write_text('Could not convert ELF path to WSL path.\n',encoding='utf-8')
        return
    for name,start,stop in DISASM_RANGES:
        shell=(
            "set -o pipefail; "
            "OBJ=$(command -v mips-linux-gnu-objdump || command -v mips64-linux-gnuabi64-objdump || true); "
            "if [ -z \"$OBJ\" ]; then echo '__NO_MIPS_OBJDUMP__'; exit 127; fi; "
            f'"$OBJ" -d --start-address=0x{start:08X} --stop-address=0x{stop:08X} "{wsl_elf}"'
        )
        attempts=[
            ['wsl.exe','-d','Ubuntu-24.04','--','bash','-lc',shell],
            ['wsl.exe','--','bash','-lc',shell],
        ]
        result=None
        for cmd in attempts:
            try:
                candidate=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
            except Exception:
                continue
            result=candidate
            if candidate.returncode==0:
                break
        dest=out/f'disasm-{name}-0x{start:08X}-0x{stop:08X}.txt'
        if result is None:
            dest.write_text('Could not invoke WSL for objdump.\n',encoding='utf-8')
        else:
            dest.write_text((result.stdout or '') + ('\nSTDERR:\n'+result.stderr if result.stderr else ''),encoding='utf-8')
        if result is not None and result.returncode==0:
            print(f'[OK] ELF disassembly: {name}')
        else:
            print(f'[WARN] ELF disassembly failed for {name}; generated C is still included.')


def write_environment(root: Path, out: Path):
    lines=[
        f'Rocket-R root: {root}',
        f'Collected: {time.strftime("%Y-%m-%d %H:%M:%S")}',
        'Purpose: read-only render-queue contract capture; no source files are modified.',
        '',
        'Known retail queue addresses:',
        '  RenderEntry base D_800ADB00 = 0x800ADB00',
        '  RenderEntry end  D_800AF300 = 0x800AF300',
        '  Retail entry size = 24 bytes',
        '  Retail capacity = 256',
    ]
    (out/'COLLECTOR-INFO.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')


def make_zip(out: Path) -> Path:
    archive=out.with_suffix('.zip')
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as z:
        for p in sorted(out.rglob('*')):
            if p.is_file(): z.write(p,p.relative_to(out.parent))
    return archive


def main():
    root=locate_root()
    stamp=time.strftime('%Y%m%d-%H%M%S')
    out=root/'build/diagnostics'/f'Rocket-R-render-queue-contract-{stamp}'
    out.mkdir(parents=True,exist_ok=False)
    print(f'Repository: {root}')
    print(f'Output:     {out}')
    write_environment(root,out)
    write_generated_extracts(root,out)
    collect_disassembly(root,out)
    archive=make_zip(out)
    print()
    print('[OK] READ-ONLY queue-contract collection complete.')
    print(f'UPLOAD THIS ZIP TO CHAT: {archive}')

if __name__=='__main__':
    try: main()
    except Exception as e:
        print('Queue-contract collector FAILED: '+str(e),file=sys.stderr)
        raise SystemExit(1)
