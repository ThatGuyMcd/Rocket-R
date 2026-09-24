#!/usr/bin/env python3
from pathlib import Path
import argparse, datetime, shutil, subprocess, sys

ARENA_MARKER='ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA'

def read_text(path: Path):
    raw=path.read_bytes()
    return raw.decode('utf-8-sig').replace('\r\n','\n'), raw.startswith(b'\xef\xbb\xbf'), ('\r\n' if b'\r\n' in raw else '\n')

def write_text(path: Path, text: str, bom: bool, nl: str):
    text=text.replace('\r\n','\n')
    if nl=='\r\n': text=text.replace('\n','\r\n')
    data=text.encode('utf-8')
    path.write_bytes((b'\xef\xbb\xbf' if bom else b'')+data)

def backup(root: Path, files):
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    base=root/'build'/'repair-backups'/f'graphics-v27-{stamp}'
    for rel in files:
        src=root/rel
        if src.is_file():
            dst=base/rel; dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
    ptr=root/'build'/'repair-backups'/'LAST-GRAPHICS-V27-BACKUP.txt'
    ptr.parent.mkdir(parents=True,exist_ok=True); ptr.write_text(str(base),encoding='utf-8')
    print(f'[OK] Backup: {base}')
    return base

def restore(root: Path, base: Path):
    if base is None or not base.is_dir(): return
    for src in base.rglob('*'):
        if src.is_file():
            dst=root/src.relative_to(base); dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
    print(f'[OK] Restored pre-v27 files from: {base}')

def patch_presentation(path: Path):
    text,bom,nl=read_text(path)
    if 'pre-render-object-gate=RECOVERED-V25' in text or 'ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE' in text:
        raise RuntimeError('v25 object-gate override is still active. Apply v26 clean rollback first, then v27.')
    if 'presentation-identity=DURABLE-OWNER-V26' not in text:
        raise RuntimeError('Expected v26 durable presentation baseline not found. Apply v26 first.')
    if 'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN' not in text:
        raise RuntimeError('v21 render-queue recovery missing; refusing unknown renderer state.')
    if ARENA_MARKER in text:
        print('[OK] v27 graphics-arena runtime already present')
        return

    anchor='constexpr std::uint32_t kGfxTaskDlStartOffset = 0x014U;\n'
    if text.count(anchor)!=1: raise RuntimeError('Could not uniquely locate GfxTask constants')
    block='''constexpr std::uint32_t kGfxTaskDlStartOffset = 0x014U;
// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA BEGIN ===
// Rocket hard-codes RAM_END=0x80400000 (4 MiB). N64ModernRuntime exposes 8 MiB,
// so these two arenas live entirely in otherwise-unused Expansion Pak RAM.
constexpr std::uint32_t kRocketV27Task0Address = 0x800C1460U;
constexpr std::uint32_t kRocketV27Task1Address = 0x800C15E8U;
constexpr std::uint32_t kRocketV27Arena0Address = 0x80600000U;
constexpr std::uint32_t kRocketV27Arena1Address = 0x80680000U;
constexpr std::uint32_t kRocketV27ArenaBytes = 0x00080000U;
constexpr std::uint32_t kGfxContextAddress = 0x800A5DA8U;
constexpr std::uint32_t kGfxTaskCtxDlHeadOffset = 0x00CU;
constexpr std::uint32_t kGfxTaskCtxMtxHeadOffset = 0x010U;
// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA END ===
'''
    text=text.replace(anchor,block,1)

    old='if (!ValidRange(buffer, 8U) || bytes == 0U || bytes > 0x20000U) return result;'
    new='if (!ValidRange(buffer, 8U) || bytes == 0U || bytes > kRocketV27ArenaBytes) return result;'
    if text.count(old)!=1: raise RuntimeError('Could not uniquely widen dynamic Gfx buffer validation')
    text=text.replace(old,new,1)

    telemetry='// v20: direct pop-in telemetry. This does not reorder, replay, expand or replace\n'
    if text.count(telemetry)!=1: raise RuntimeError('Could not uniquely locate v20 telemetry anchor')
    helper='''// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA RUNTIME BEGIN ===
extern "C" void rocket_graphics_arena_v27(std::uint8_t* rdram, recomp_context*) {
    if (rdram == nullptr) return;
    const std::uint32_t task = ReadU32(rdram, kCurGfxTaskAddress);
    std::uint32_t arena = 0U;
    if (task == kRocketV27Task0Address) arena = kRocketV27Arena0Address;
    else if (task == kRocketV27Task1Address) arena = kRocketV27Arena1Address;
    else return;

    const std::uint32_t arena_end = arena + kRocketV27ArenaBytes;
    if (!ValidRange(arena, kRocketV27ArenaBytes) ||
        !ValidRange(task, kGfxTaskCtxMtxHeadOffset + 4U) ||
        !ValidRange(kGfxContextAddress, 0x10U)) return;

    const auto write_u32 = [&](std::uint32_t address, std::uint32_t value) {
        MEM_W(0, RdramAddress(address)) = value;
    };

    // Persist the expanded template for this alternating task.
    write_u32(task + kGfxTaskCtxSizeOffset, kRocketV27ArenaBytes);
    write_u32(task + kGfxTaskCtxDlStartOffset, arena);
    write_u32(task + kGfxTaskCtxDlHeadOffset, arena);
    write_u32(task + kGfxTaskCtxMtxHeadOffset, arena_end);

    // update_gfx_context already copied the retail context this frame, so redirect
    // the active global context before func_80046D58 emits any frame commands.
    write_u32(kGfxContextAddress + 0x00U, kRocketV27ArenaBytes);
    write_u32(kGfxContextAddress + 0x04U, arena);
    write_u32(kGfxContextAddress + 0x08U, arena);
    write_u32(kGfxContextAddress + 0x0CU, arena_end);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[graphics] v27 GfxTask arena expansion active: 512 KiB x2.\\n");
    }
}
// === ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA RUNTIME END ===

'''
    text=text.replace(telemetry,helper+telemetry,1)
    text=text.replace('=== Rocket-R v26 DURABLE-MODEL-OWNERSHIP + QUEUE-RECOVERY diagnostic session ===',
                      '=== Rocket-R v27 EXPANDED-GFX-ARENA + DURABLE-OWNERSHIP diagnostic session ===')
    text=text.replace('presentation-identity=DURABLE-OWNER-V26 ',
                      'presentation-identity=DURABLE-OWNER-V26 gfx-arena=512Kx2-V27 ', 1)
    write_text(path,text,bom,nl)
    print('[OK] Added v27 512 KiB x2 graphics arenas')

def install_generated_patcher(root: Path, payload: Path):
    dst=root/'scripts'/'patch_graphics_arena_v27_generated.py'
    shutil.copy2(payload,dst)
    print('[OK] Installed v27 generated-code patcher')

def patch_oneclick(path: Path):
    text,bom,nl=read_text(path)
    call="Invoke-Python @((Join-Path $Root 'scripts\\patch_graphics_arena_v27_generated.py'),'--root',$Root)"
    if call in text:
        print('[OK] OneClickBuild already invokes v27 patcher'); return
    anchor="    Invoke-Python @((Join-Path $Root 'scripts\\patch_render_queue_expansion_v21_generated.py'),'--root',$Root)\n"
    if text.count(anchor)!=1: raise RuntimeError('Could not uniquely locate v21 OneClick hook')
    ins=(anchor+"\n    # Graphics v27: expand only the GfxTask command/matrix arena in Expansion Pak RAM.\n"
         "    # Draw Distance, visibility and interpolation math are untouched.\n    "+call+"\n")
    text=text.replace(anchor,ins,1); write_text(path,text,bom,nl)
    print('[OK] OneClickBuild will reapply v27 after N64Recomp')

def run_generated(root: Path):
    out=root/'runtime-recomp'/'RecompiledFuncs'
    if out.is_dir() and any(out.glob('funcs_*.c')):
        cp=subprocess.run([sys.executable,str(root/'scripts/patch_graphics_arena_v27_generated.py'),'--root',str(root)],cwd=str(root))
        if cp.returncode: raise RuntimeError('current generated v27 hook failed')

def verify(root: Path):
    p=(root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    o=(root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    for tok in ('ROCKET-R GRAPHICS V27 EXPANDED GFX ARENA BEGIN',
                'kRocketV27Arena0Address = 0x80600000U',
                'kRocketV27Arena1Address = 0x80680000U',
                'kRocketV27ArenaBytes = 0x00080000U',
                'rocket_graphics_arena_v27', 'gfx-arena=512Kx2-V27',
                'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN',
                'presentation-identity=DURABLE-OWNER-V26'):
        if tok not in p: raise RuntimeError('v27 verification token missing: '+tok)
    if 'bytes > kRocketV27ArenaBytes' not in p: raise RuntimeError('dynamic task-buffer validation not widened')
    if 'patch_graphics_arena_v27_generated.py' not in o: raise RuntimeError('OneClick v27 hook missing')
    for bad in ('pre-render-object-gate=RECOVERED-V25','ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE'):
        if bad in p: raise RuntimeError('bad v25 state remains: '+bad)
    gen=root/'runtime-recomp'/'RecompiledFuncs'
    if gen.is_dir() and any(gen.glob('funcs_*.c')):
        hits=[]
        for f in gen.glob('funcs_*.c'):
            t=f.read_text(encoding='utf-8-sig')
            if 'RECOMP_FUNC void update_gfx_context(uint8_t* rdram, recomp_context* ctx)' in t: hits.append(t)
        if len(hits)!=1 or 'ROCKET-R GRAPHICS V27 UPDATE_GFX_CONTEXT HOOK' not in hits[0]:
            raise RuntimeError('current generated update_gfx_context lacks v27 hook')
    print('[OK] v27 verification PASS')

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); ap.add_argument('--rollback-latest',action='store_true'); ns=ap.parse_args()
    root=Path(ns.root).resolve()
    if ns.rollback_latest:
        ptr=root/'build'/'repair-backups'/'LAST-GRAPHICS-V27-BACKUP.txt'
        if not ptr.is_file(): raise RuntimeError('No v27 backup pointer exists')
        base=Path(ptr.read_text(encoding='utf-8').strip())
        if not base.is_dir(): raise RuntimeError('Latest v27 backup missing: '+str(base))
        restore(root,base)
        if not (base/'scripts/patch_graphics_arena_v27_generated.py').is_file():
            (root/'scripts/patch_graphics_arena_v27_generated.py').unlink(missing_ok=True)
        print('[OK] Rolled back latest v27 installation'); return

    req=['src/presentation_identity.cpp','scripts/OneClickBuild.ps1','scripts/patch_render_queue_expansion_v21_generated.py']
    for rel in req:
        if not (root/rel).is_file(): raise RuntimeError('Required Rocket-R file missing: '+rel)
    files=list(req)
    if (root/'scripts/patch_graphics_arena_v27_generated.py').is_file(): files.append('scripts/patch_graphics_arena_v27_generated.py')
    gen=root/'runtime-recomp'/'RecompiledFuncs'
    if gen.is_dir(): files += [str(x.relative_to(root)) for x in gen.glob('funcs_*.c')]
    base=backup(root,files)
    try:
        patch_presentation(root/'src/presentation_identity.cpp')
        payload=Path(__file__).with_name('patch_graphics_arena_v27_generated.py')
        install_generated_patcher(root,payload)
        patch_oneclick(root/'scripts/OneClickBuild.ps1')
        run_generated(root)
        verify(root)
    except Exception:
        restore(root,base)
        if not (base/'scripts/patch_graphics_arena_v27_generated.py').is_file():
            (root/'scripts/patch_graphics_arena_v27_generated.py').unlink(missing_ok=True)
        raise
    print('[OK] Graphics v27 installed: 512 KiB x2 GfxTask arenas in Expansion Pak RAM.')
    print('[OK] Draw Distance, culling, gameplay and interpolation math were not changed.')

if __name__=='__main__':
    try: main()
    except Exception as exc:
        print('Graphics v27 installation failed: '+str(exc),file=sys.stderr); raise SystemExit(1)
