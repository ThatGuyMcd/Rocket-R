#!/usr/bin/env python3
from __future__ import annotations
import argparse, hashlib, json, re, subprocess, sys
from pathlib import Path


def fail(msg: str) -> None:
    print('[ERROR] ' + msg)
    raise SystemExit(1)

def require(cond: bool, msg: str) -> None:
    if not cond: fail(msg)

def read(p: Path) -> str:
    return p.read_text(encoding='utf-8-sig')

def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args=ap.parse_args()
    root=Path(args.root).resolve()

    required_files=[
        'src/graphics_enhancements.cpp','src/graphics_enhancements.hpp',
        'src/runtime_ui.cpp','src/rt64_renderer.cpp','src/rt64_renderer.hpp',
        'src/presentation_identity.cpp','src/presentation_identity.hpp',
        'src/widescreen_culling.cpp','runtime-recomp/rocket.us.recomp-policy.json',
        'patches/manifest.json'
    ]
    for rel in required_files:
        require((root/rel).is_file(), f'missing Graphics v9 file: {rel}')

    manifest=json.loads(read(root/'patches/manifest.json'))
    rt=next((d for d in manifest.get('dependencies',[]) if d.get('name')=='RT64'),None)
    require(rt is not None, 'RT64 dependency missing from manifest')
    patches=rt.get('patches',[])
    paths=[p.get('path','') for p in patches]
    expected=[
        'patches/rt64/0010-dkrr-configurable-default-anisotropy.patch',
        'patches/rt64/0011-dkrr-configurable-mip-lod-bias.patch',
        'patches/rt64/0012-rocket-world-hud-graphics-controls.patch',
        'patches/rt64/0013-rocket-vi-postprocess-and-presentation-stats.patch',
        'patches/rt64/0014-rocket-custom-vi-shader.patch',
    ]
    for rel in expected:
        require(paths.count(rel)==1, f'RT64 Graphics v9 patch must appear exactly once: {rel}')
    require(len(patches) >= 14, 'RT64 manifest does not contain the full Graphics v9 patch set')
    for item in patches:
        p=root/item['path']
        require(p.is_file(), f'manifest patch missing: {item["path"]}')
        digest=hashlib.sha256(p.read_bytes()).hexdigest()
        require(digest.lower()==str(item.get('sha256','')).lower(), f'patch SHA-256 mismatch: {item["path"]}')
        cp=subprocess.run(['git','apply','--numstat',str(p)],cwd=root,text=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        require(cp.returncode==0, f'malformed dependency patch {item["path"]}: {cp.stderr.strip()}')

    cmake=read(root/'CMakeLists.txt')
    gx_h=read(root/'src/graphics_enhancements.hpp')
    gx_c=read(root/'src/graphics_enhancements.cpp')
    ui=read(root/'src/runtime_ui.cpp')
    rr=read(root/'src/rt64_renderer.cpp')
    rrh=read(root/'src/rt64_renderer.hpp')
    pi=read(root/'src/presentation_identity.cpp')
    pih=read(root/'src/presentation_identity.hpp')
    wide=read(root/'src/widescreen_culling.cpp')
    policy=json.loads(read(root/'runtime-recomp/rocket.us.recomp-policy.json'))

    require('src/graphics_enhancements.cpp' in cmake, 'graphics_enhancements.cpp is not compiled')
    for token in ['GraphicsPreset','AspectPreset','TextureFiltering','TextureScaling2D','FramebufferPrecision',
                  'DisplayBuffering','HardwareResolve','ViFilterMode','ZFightingMode','PostProcessMode',
                  'fov_offset_degrees','draw_distance_multiplier',
                  'performance_overlay','interpolation_overlay']:
        require(token in gx_h, f'graphics settings model missing: {token}')
    for token in ['rocket_graphics_camera_begin','rocket_graphics_frustum_begin','effective_fov_radians',
                  'RestoreOwnedCamera','kInfiniteRenderDistance',]:
        require(token in gx_c, f'graphics guest enhancement missing: {token}')
    require('rocket::graphics::effective_fov_radians' in wide,
            'widescreen CPU culling is not using the same effective FOV as rendering')
    require('rocket::graphics::selected_aspect' in wide and 'kEdgeGuard = 1.10F' in wide,
            'widescreen culling integration is incomplete')

    hooks=policy.get('functionHooks',[])
    cam=[h for h in hooks if isinstance(h,dict) and h.get('function')=='func_8003ACD4']
    fr=[h for h in hooks if isinstance(h,dict) and h.get('function')=='frustum_test']
    require(len(cam)==1 and cam[0].get('beforeVram')=='0x8003ACD4' and 'rocket_graphics_camera_begin' in str(cam[0].get('text','')),
            'camera FOV/world graphics hook is missing or duplicated')
    require(len(fr)==1 and 'rocket_graphics_frustum_begin' in str(fr[0].get('text','')) and 'rocket_widescreen_frustum_begin' in str(fr[0].get('text','')),
            'frustum graphics/widescreen hook is missing or duplicated')
    require(str(fr[0].get('text','')).index('rocket_graphics_frustum_begin') < str(fr[0].get('text','')).index('rocket_widescreen_frustum_begin'),
            'draw-distance hook must run before object-local FOV culling')

    ui_tokens=[
        'Graphics API','Window mode','VSync','Aspect ratio','Resolution','Frame rate','Display buffering',
        'Framebuffer precision','Hardware resolve','Anti-aliasing','Output scaling filter',
        'Anisotropic filtering','Texture mip LOD bias',
        'Post-process shader',
        'Field of view offset','Draw distance',
        'Performance overlay','Interpolation coverage overlay','RESET GRAPHICS TO ORIGINAL'
    ]
    for token in ui_tokens:
        require(token in ui, f'Graphics v9 UI missing approved control: {token}')
    for token in [
        'const char* GraphicsPresetName(',
        'void DrawDiagnosticsOverlay()',
        'int g_graphics_section = 0;',
    ]:
        require(ui.count(token) == 1, f'Graphics v9 runtime UI helper missing or duplicated: {token}')
    require('Borderless fullscreen' in ui and 'Windowed' in ui, 'window/borderless modes are missing')
    require('Custom shader binaries are loaded when RT64 starts' in ui, 'custom shader restart semantics are not explained')

    for token in ['setDefaultSamplerAnisotropy','setDefaultSamplerMipLODBias','setRocketCustomShaderBasePath',
                  'setVsyncEnabled','publish_rt64_rocket_controls','totalPresentations','apply_extra_graphics',
                  'update_performance_stats']:
        require(token in rr, f'RT64 bridge missing: {token}')
    require('Preserve RT64\'s driver-specific Automatic decision' in rr and
            'extra.hardware_resolve == rocket::graphics::HardwareResolve::On' in rr and
            'rocket::graphics::HardwareResolve::Off' in rr,
            'hardware resolve Automatic no longer preserves RT64 driver workarounds')
    require('graphics_revision_' in rrh and 'performance_window_started_' in rrh,
            'renderer live-settings/performance state missing')

    require('ROCKET_INTERPOLATION_DYNAMIC_VERTICES' not in pi,
            'old opt-in dynamic vertex gate is still present')
    for token in ['root.interpolate_vertices = entry.dynamic_gfx','root.interpolate_texcoords = entry.dynamic_gfx',
                  'binding.interpolate_vertices = sample.dynamic_gfx','g_coverage_dynamic_vertex_bindings',
                  'g_coverage_snapped_bindings','G_EX_ID_IGNORE']:
        require(token in pi, f'global interpolation coverage missing: {token}')
    require('struct CoverageStats' in pih and 'dynamic_vertex_bindings' in pih,
            'interpolation coverage telemetry API missing')

    patch_text='\n'.join(read(root/p) for p in expected)
    for token in ['setDefaultSamplerAnisotropy','setDefaultSamplerMipLODBias','setRocketFogDistanceMultiplier',
                  'getRocketHudScale','zFightToleranceScale','rocketDebandStrength','rocketPostProcessMode',
                  'totalPresentations','videoInterfaceCustomValid']:
        require(token in patch_text, f'RT64 graphics patch implementation missing: {token}')

    print('[OK] Rocket-R Graphics Expansion v9 verification PASS.')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
