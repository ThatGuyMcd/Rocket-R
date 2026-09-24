#!/usr/bin/env python3
from pathlib import Path
import argparse, sys

MARKER='ROCKET-R GRAPHICS V27 UPDATE_GFX_CONTEXT HOOK'
FUNC='RECOMP_FUNC void update_gfx_context(uint8_t* rdram, recomp_context* ctx)'

def read_text(path):
    raw=path.read_bytes()
    return raw.decode('utf-8-sig').replace('\r\n','\n'), raw.startswith(b'\xef\xbb\xbf'), ('\r\n' if b'\r\n' in raw else '\n')

def write_text(path,text,bom,nl):
    text=text.replace('\r\n','\n')
    if nl=='\r\n': text=text.replace('\n','\r\n')
    data=text.encode('utf-8')
    path.write_bytes((b'\xef\xbb\xbf' if bom else b'')+data)

def function_span(text, marker):
    start=text.find(marker)
    if start<0: raise RuntimeError('function marker missing: '+marker)
    brace=text.find('{',start)
    if brace<0: raise RuntimeError('opening brace missing')
    depth=0; state='code'; i=brace
    while i<len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ''
        if state=='code':
            if c=='/' and n=='/': state='line'; i+=2; continue
            if c=='/' and n=='*': state='block'; i+=2; continue
            if c=='"': state='string'
            elif c=="'": state='char'
            elif c=='{': depth+=1
            elif c=='}':
                depth-=1
                if depth==0: return start,i+1
        elif state=='line':
            if c=='\n': state='code'
        elif state=='block':
            if c=='*' and n=='/': state='code'; i+=2; continue
        elif state=='string':
            if c=='\\': i+=2; continue
            if c=='"': state='code'
        elif state=='char':
            if c=='\\': i+=2; continue
            if c=="'": state='code'
        i+=1
    raise RuntimeError('unterminated function')

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); ns=ap.parse_args()
    root=Path(ns.root).resolve(); out=root/'runtime-recomp'/'RecompiledFuncs'
    hits=[]
    for p in out.glob('funcs_*.c'):
        t,b,n=read_text(p)
        if FUNC in t: hits.append((p,t,b,n))
    if len(hits)!=1:
        raise RuntimeError(f'Expected exactly one generated update_gfx_context, found {len(hits)}')
    p,t,b,n=hits[0]
    fs,fe=function_span(t,FUNC); body=t[fs:fe]
    if MARKER in body:
        print(f'[OK] v27 update_gfx_context hook already present: {p}')
        return
    if body.count('    return;') != 1:
        raise RuntimeError('Expected exactly one generated return in update_gfx_context')
    injection=(
        '    // === ROCKET-R GRAPHICS V27 UPDATE_GFX_CONTEXT HOOK BEGIN ===\n'
        '    extern void rocket_graphics_arena_v27(uint8_t*, recomp_context*);\n'
        '    rocket_graphics_arena_v27(rdram, ctx);\n'
        '    // === ROCKET-R GRAPHICS V27 UPDATE_GFX_CONTEXT HOOK END ===\n'
    )
    body=body.replace('    return;',injection+'    return;',1)
    t=t[:fs]+body+t[fe:]
    write_text(p,t,b,n)
    print(f'[OK] Patched v27 graphics-arena hook: {p}')

if __name__=='__main__':
    try: main()
    except Exception as e:
        print('v27 generated graphics-arena patch failed: '+str(e),file=sys.stderr)
        raise SystemExit(1)
