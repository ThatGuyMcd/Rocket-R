from __future__ import annotations
from pathlib import Path
import re, subprocess, sys, time, zipfile, shutil

TARGETS = (
    'func_8001E954',
    'func_8001ECEC',
    'func_80020134',
    'func_8008AEA0',
    'add_render_entry',
    'func_8008B4BC',
    'func_8008B694',
    'func_80061300',
)
RANGES = (
    ('func_8001E954', 0x8001E954, 0x8001EA18),
    ('func_8001ECEC', 0x8001ECEC, 0x8001F128),
    ('func_80020134', 0x80020134, 0x80020714),
    ('add_render_entry', 0x8008B24C, 0x8008B3DC),
    ('func_8008B694', 0x8008B694, 0x8008BCE8),
    ('func_80061300', 0x80061300, 0x800613F8),
)
PATTERNS = (
    re.compile(r'-0X0?D00\b', re.I),
    re.compile(r'-0X2500\b', re.I),
    re.compile(r'800AF300', re.I),
    re.compile(r'800ADB00', re.I),
    re.compile(r'0X8040\b', re.I),
    re.compile(r'0X8058\b', re.I),
    re.compile(r'rocket_render_capacity_v31_', re.I),
    re.compile(r'ROCKET-R GRAPHICS V31', re.I),
)

def extract(text: str, name: str):
    m=re.search(r'RECOMP_FUNC\s+void\s+'+re.escape(name)+r'\s*\([^)]*\)\s*\{', text)
    if not m: return None
    b=text.find('{',m.start()); depth=0; state='code'; i=b
    while i < len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ''
        if state=='code':
            if c=='/' and n=='/': state='line'; i+=2; continue
            if c=='/' and n=='*': state='block'; i+=2; continue
            if c=='"': state='str'
            elif c=="'": state='chr'
            elif c=='{': depth+=1
            elif c=='}':
                depth-=1
                if depth==0: return text[m.start():i+1]+'\n'
        elif state=='line':
            if c=='\n': state='code'
        elif state=='block':
            if c=='*' and n=='/': state='code'; i+=2; continue
        elif state=='str':
            if c=='\\': i+=2; continue
            if c=='"': state='code'
        elif state=='chr':
            if c=='\\': i+=2; continue
            if c=="'": state='code'
        i+=1
    return None

def wsl_path(p: Path):
    s=str(p.resolve()); m=re.match(r'^([A-Za-z]):\\(.*)$',s)
    if not m: return None
    return f'/mnt/{m.group(1).lower()}/{m.group(2).replace(chr(92), "/")}'

def main():
    root=Path(sys.argv[1] if len(sys.argv)>1 else '.').resolve()
    gen=root/'runtime-recomp'/'RecompiledFuncs'
    if not gen.is_dir(): raise RuntimeError(f'Missing generated CPU directory: {gen}')
    stamp=time.strftime('%Y%m%d-%H%M%S')
    out=root/'build'/'diagnostics'/f'Rocket-R-queue-pool-audit-{stamp}'
    out.mkdir(parents=True,exist_ok=False)
    cfiles=sorted(gen.glob('*.c'))
    found={x:None for x in TARGETS}
    refs=[]
    for p in cfiles:
        text=p.read_text(encoding='utf-8',errors='replace')
        for name in TARGETS:
            if found[name] is None:
                block=extract(text,name)
                if block:
                    found[name]=p
                    (out/f'generated-{name}.c.txt').write_text(f'// source: {p.relative_to(root)}\n\n'+block,encoding='utf-8')
        lines=text.splitlines()
        for i,line in enumerate(lines,1):
            if any(rx.search(line) for rx in PATTERNS):
                lo=max(1,i-8); hi=min(len(lines),i+8)
                context='\n'.join(f'{n:6d}: {lines[n-1]}' for n in range(lo,hi+1))
                refs.append(f'===== {p.relative_to(root)} line {i} =====\n{context}\n')
    (out/'ALL-QUEUE-POOL-REFERENCES.txt').write_text('\n'.join(refs),encoding='utf-8')
    (out/'FUNCTION-INDEX.txt').write_text('\n'.join(f'{n}: {found[n].relative_to(root) if found[n] else "MISSING"}' for n in TARGETS)+'\n',encoding='utf-8')

    # Current v31 implementation and build integration, if present.
    extras=[
        root/'src'/'render_capacity_v31.cpp',
        root/'scripts'/'patch_render_capacity_v31_generated.py',
        root/'scripts'/'OneClickBuild.ps1',
        root/'scripts'/'self_check.py',
    ]
    for p in extras:
        if p.is_file(): shutil.copy2(p,out/p.name)

    # Recent logs only; useful for v31 telemetry without huge copies.
    logs=root/'build'/'logs'
    if logs.is_dir():
        recent=sorted((p for p in logs.glob('*.log') if p.is_file()),key=lambda p:p.stat().st_mtime,reverse=True)[:8]
        for p in recent: shutil.copy2(p,out/('log-'+p.name))

    elf=root/'extern'/'rocket-decomp'/'build'/'us'/'NSUE.elf'
    wp=wsl_path(elf) if elf.is_file() else None
    if wp:
        for name,start,stop in RANGES:
            sh=("OBJ=$(command -v mips-linux-gnu-objdump || command -v mips64-linux-gnuabi64-objdump || true); "
                "if [ -z \"$OBJ\" ]; then exit 127; fi; "
                f'\"$OBJ\" -d --start-address=0x{start:08X} --stop-address=0x{stop:08X} \"{wp}\"')
            result=None
            for cmd in (["wsl.exe","-d","Ubuntu-24.04","--","bash","-lc",sh],["wsl.exe","--","bash","-lc",sh]):
                try:
                    r=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
                except Exception: continue
                result=r
                if r.returncode==0: break
            (out/f'disasm-{name}.txt').write_text((result.stdout if result else '') + (('\nSTDERR:\n'+result.stderr) if result and result.stderr else ''),encoding='utf-8')

    info=[
        f'Root: {root}',f'Generated C files scanned: {len(cfiles)}',f'Queue/pool reference hits: {len(refs)}','',
        'READ ONLY: this collector changes no Rocket-R source or generated code.',
        'Goal: determine whether v31 is rendering only staged/overflow entries and identify every live RenderEntry consumer/mutator.',
    ]
    (out/'AUDIT-INFO.txt').write_text('\n'.join(info)+'\n',encoding='utf-8')
    z=out.with_suffix('.zip')
    with zipfile.ZipFile(z,'w',zipfile.ZIP_DEFLATED) as zz:
        for p in sorted(out.rglob('*')):
            if p.is_file(): zz.write(p,p.relative_to(out.parent))
    print(f'UPLOAD THIS ZIP TO CHAT: {z}')

if __name__=='__main__':
    try: main()
    except Exception as e:
        print(f'AUDIT FAILED: {e}',file=sys.stderr); raise SystemExit(1)
