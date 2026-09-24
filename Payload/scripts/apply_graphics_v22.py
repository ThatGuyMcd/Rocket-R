#!/usr/bin/env python3
from pathlib import Path
import argparse, ast, datetime, shutil, subprocess, sys

CULLING_REPLACEMENT = '''extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // v22 NO-POP policy: disable ONLY Rocket's four CPU side-plane tests.
    // r7/renderDistance is untouched, so the configured View Distance cutoff
    // and Rocket's final-distance fade remain authoritative. v21 recovers any
    // submissions beyond the retail 256-entry staging list.
    rocket_popdiag_frustum_call();
    constexpr std::uint32_t kDisableCpuSidePlanesBits = 0x7F7FFFFFU; // FLT_MAX
    context->r6 = static_cast<gpr>(kDisableCpuSidePlanesBits);
}'''

SELF_CHECK_V22 = '''    # v22: no camera-angle CPU side-plane rejection + v21 in-function queue recovery.
    # r7 remains the authored/scaled render-distance path, preserving Rocket's distance fade.
    _v22_root = __import__('pathlib').Path(__file__).resolve().parents[1]
    _v22_culling = (_v22_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    _v22_graphics = (_v22_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    _v22_presentation = (_v22_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
    _v22_oneclick = (_v22_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    _v22_patcher_path = _v22_root / 'scripts' / 'patch_render_queue_expansion_v21_generated.py'
    require(_v22_patcher_path.is_file(), 'v21 generated queue patcher missing')
    _v22_patcher = _v22_patcher_path.read_text(encoding='utf-8-sig')
    _v22_required = (
        'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' in _v22_culling and
        'rocket_popdiag_frustum_call()' in _v22_culling and
        'context->r6 = static_cast<gpr>(kDisableCpuSidePlanesBits)' in _v22_culling and
        'static_cast<std::uint32_t>(context->r7)' in _v22_graphics and
        's.draw_distance_multiplier' in _v22_graphics and
        'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN' in _v22_presentation and
        'rocket_render_queue_prepare_first_batch' in _v22_presentation and
        'rocket_render_queue_prepare_next_batch' in _v22_presentation and
        'side-plane-cpu-cull=DISABLED-V22' in _v22_presentation and
        'queue-overflow=RECOVERED-V21' in _v22_presentation and
        'patch_popin_diagnostics_generated.py' in _v22_oneclick and
        'patch_render_queue_expansion_v21_generated.py' in _v22_oneclick and
        'ROCKET_QUEUE_V21_PROCESS_BATCH' in _v22_patcher
    )
    _v22_forbidden = any(token in (_v22_presentation + _v22_oneclick) for token in (
        'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',
        'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',
        'rocket_original_func_8008B694', 'rocket_capture_func_8008B694',
        'rocket_draw_func_8008B694', '[render-queue] GLOBAL',
    ))
    if not (_v22_required and not _v22_forbidden):
        raise SystemExit('SOURCE SELF-CHECK FAILED: v22 no-side-cull + v21 queue-recovery state missing')
'''

def read_text(path):
    raw=path.read_bytes(); return raw.decode('utf-8-sig'),raw.startswith(b'\xef\xbb\xbf'),('\r\n' if b'\r\n' in raw else '\n')
def write_text(path,text,bom,nl):
    text=text.replace('\r\n','\n');
    if nl=='\r\n': text=text.replace('\n','\r\n')
    data=text.encode('utf-8'); path.write_bytes((b'\xef\xbb\xbf' if bom else b'')+data)
def function_span(text,marker):
    start=text.find(marker)
    if start<0: raise RuntimeError('Function marker not found: '+marker)
    brace=text.find('{',start); depth=0; state='code'; i=brace
    if brace<0: raise RuntimeError('Opening brace not found: '+marker)
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
                if depth==0:return start,i+1
        elif state=='line':
            if c=='\n':state='code'
        elif state=='block':
            if c=='*' and n=='/':state='code';i+=2;continue
        elif state=='string':
            if c=='\\':i+=2;continue
            if c=='"':state='code'
        elif state=='char':
            if c=='\\':i+=2;continue
            if c=="'":state='code'
        i+=1
    raise RuntimeError('Unterminated function: '+marker)

def patch_culling(path):
    text,bom,nl=read_text(path)
    if 'rocket_popdiag_frustum_call' not in text: raise RuntimeError('Expected v21 culling/telemetry tree not found')
    s,e=function_span(text,'extern "C" void rocket_widescreen_frustum_begin')
    cur=text[s:e]
    if 'v22 NO-POP policy' in cur and 'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' in cur:return
    if 'v16 viewport-locked FOV/aspect guard active' not in cur and 'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in cur:
        raise RuntimeError('Expected v20.2/v21 culling implementation not found; refusing ambiguous edit')
    write_text(path,text[:s]+CULLING_REPLACEMENT+text[e:],bom,nl)

def patch_presentation(path):
    text,bom,nl=read_text(path)
    for tok in ('ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN','rocket_render_queue_prepare_first_batch','rocket_render_queue_prepare_next_batch','queue-overflow=RECOVERED-V21'):
        if tok not in text: raise RuntimeError('Required v21 queue token missing: '+tok)
    old='side-plane-cpu-cull=V17.1-VIEWPORT-GUARD'; new='side-plane-cpu-cull=DISABLED-V22'
    if old in text:text=text.replace(old,new)
    elif new not in text:raise RuntimeError('v21 diagnostic culling-mode label missing')
    text=text.replace('=== Rocket-R v21 IN-FUNCTION QUEUE-EXPANSION diagnostic session ===','=== Rocket-R v22 NO-SIDE-CULL + QUEUE-RECOVERY diagnostic session ===')
    write_text(path,text,bom,nl)

def patch_self_check(path):
    text,bom,nl=read_text(path)
    if '# v22: no camera-angle CPU side-plane rejection + v21 in-function queue recovery.' in text:return
    start=text.find('    # v21: preserve v20.2 culling/distance policy')
    end=text.find('    scan_checked_source(root)',start)
    if start<0 or end<0:raise RuntimeError('Current v21 graphics self-check block not found')
    text=text[:start]+SELF_CHECK_V22+text[end:]; ast.parse(text); write_text(path,text,bom,nl)

def verify(root):
    c=(root/'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    g=(root/'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    p=(root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    o=(root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    sc=(root/'scripts/self_check.py').read_text(encoding='utf-8-sig')
    s,e=function_span(c,'extern "C" void rocket_widescreen_frustum_begin'); body=c[s:e]
    for tok in ('rocket_popdiag_frustum_call();','kDisableCpuSidePlanesBits = 0x7F7FFFFFU','context->r6 = static_cast<gpr>(kDisableCpuSidePlanesBits)'):
        if tok not in body:raise RuntimeError('v22 culling token missing: '+tok)
    if 'context->r7' in body:raise RuntimeError('v22 culling hook must not modify r7')
    gs,ge=function_span(g,'extern "C" void rocket_graphics_frustum_begin'); gb=g[gs:ge]
    for tok in ('static_cast<std::uint32_t>(context->r7)','s.draw_distance_multiplier','context->r7 ='):
        if tok not in gb:raise RuntimeError('Draw Distance r7 path missing: '+tok)
    for tok in ('ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN','rocket_render_queue_prepare_first_batch','rocket_render_queue_prepare_next_batch','side-plane-cpu-cull=DISABLED-V22','queue-overflow=RECOVERED-V21'):
        if tok not in p:raise RuntimeError('v21/v22 token missing: '+tok)
    for tok in ('patch_popin_diagnostics_generated.py','patch_render_queue_expansion_v21_generated.py'):
        if tok not in o:raise RuntimeError('OneClick generated hook missing: '+tok)
    if '# v22: no camera-angle CPU side-plane rejection + v21 in-function queue recovery.' not in sc:raise RuntimeError('v22 self-check missing')
    for tok in ('ExpandedRenderEntry','RocketBuildGlobalRenderOrder','rocket_render_queue_begin_batch(','rocket_original_func_8008B694','rocket_capture_func_8008B694','rocket_draw_func_8008B694'):
        if tok in p+o:raise RuntimeError('Retired renderer token present: '+tok)

def run_checked(cmd,cwd,label):
    print('+ '+' '.join(str(x) for x in cmd)); cp=subprocess.run(cmd,cwd=str(cwd))
    if cp.returncode:raise RuntimeError(f'{label} failed with exit code {cp.returncode}')

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); a=ap.parse_args(); root=Path(a.root).resolve()
    targets=[root/'src/widescreen_culling.cpp',root/'src/presentation_identity.cpp',root/'scripts/self_check.py']
    for t in targets:
        if not t.is_file():raise RuntimeError('Required file missing: '+str(t))
    patcher=root/'scripts/patch_render_queue_expansion_v21_generated.py'
    if not patcher.is_file():raise RuntimeError('v21 queue expansion is not installed')
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S'); backup=root/'build'/'repair-backups'/f'graphics-v22-{stamp}'
    for t in targets:
        out=backup/t.relative_to(root); out.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(t,out)
    print('[OK] Backup:',backup)
    try:
        patch_culling(targets[0]); patch_presentation(targets[1]); patch_self_check(targets[2]); verify(root)
        run_checked([sys.executable,str(patcher),'--root',str(root),'--verify'],root,'v21 generated renderer verification')
        run_checked([sys.executable,str(root/'scripts/self_check.py')],root,'Rocket-R source self-check')
    except Exception:
        for t in targets:
            src=backup/t.relative_to(root)
            if src.is_file():shutil.copy2(src,t)
        raise
    print('[OK] Rocket-R Graphics v22 NO-POP policy installed.')
    print('[OK] CPU side-plane rejection disabled through r6 only.')
    print('[OK] v21 in-function queue overflow recovery retained.')
    print('[OK] Extended Draw Distance remains on r7; retail distance fade remains active.')
    print('[OK] Re-run ONE-CLICK-BUILD.cmd to produce the updated executable.')
if __name__=='__main__':
    try:main()
    except Exception as exc:print('[ERROR] '+str(exc),file=sys.stderr);sys.exit(1)
