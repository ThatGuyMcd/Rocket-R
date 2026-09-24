#!/usr/bin/env python3
from pathlib import Path
import argparse

REMOVED_UI = [
    "Use original 4:3 framing during detected cutscenes",
    "Screen shake strength",
    "Maximum visibility (disable distance culling)",
    "Widescreen culling fix",
    "Fog distance",
    "HUD aspect policy",
    "HUD scale",
    "HUD safe-area margin",
    'GraphicsSectionButton("HUD"',
]
REMOVED_FIELDS = [
    "original_aspect_cutscenes",
    "screen_shake_strength",
    "maximum_detail",
    "widescreen_culling",
    "fog_distance_multiplier",
    "hud_aspect",
    "hud_scale_percent",
    "hud_safe_margin_percent",
    "HudAspectMode",
]
REMOVED_KEYS = [
    "cutscene_original_aspect=",
    "screen_shake_strength=",
    "maximum_detail=",
    "widescreen_culling=",
    "fog_distance=",
    "hud_aspect=",
    "hud_scale=",
    "hud_safe_margin=",
]

def fail(msg):
    print('[ERROR] ' + msg)
    raise SystemExit(1)

def require(cond, msg):
    if not cond: fail(msg)

def read(p):
    return p.read_text(encoding='utf-8-sig')

def function_body(text, marker):
    start = text.find(marker)
    if start < 0: fail(f'Missing function: {marker}')
    brace = text.find('{', start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == '{': depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0: return text[start:i+1]
    fail(f'Unterminated function: {marker}')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    root = Path(ap.parse_args().root).resolve()
    h = read(root/'src/graphics_enhancements.hpp')
    g = read(root/'src/graphics_enhancements.cpp')
    w = read(root/'src/widescreen_culling.cpp')
    u = read(root/'src/runtime_ui.cpp')
    r = read(root/'src/rt64_renderer.cpp')
    rh = read(root/'src/rt64_renderer.hpp')

    for token in REMOVED_FIELDS:
        require(token not in h, f'Removed setting still present in Settings model: {token}')
    for token in REMOVED_UI:
        require(token not in u, f'Removed Graphics control/tab is still visible: {token}')
    for token in REMOVED_KEYS:
        require(token not in u, f'Removed setting is still persisted: {token}')

    require('Preserve authored cutscene FOV' in u, 'Preserve authored cutscene FOV should remain available')
    require('HUD framing is automatic' in u, 'Automatic HUD policy explanation missing')
    require('Aspect/FOV culling protection is automatic' in u, 'Automatic culling explanation missing')
    require('GraphicsSectionButton("DIAGNOSTICS", 3)' in u, 'Diagnostics did not move into the removed HUD slot')

    selected = function_body(g, 'float rocket::graphics::selected_aspect')
    require('cutscene_active()' not in selected, 'Aspect selection still changes from heuristic cutscene detection')
    aspect_set = function_body(g, 'void rocket::graphics::set_window_aspect')
    require('request_renderer_refresh();' in aspect_set, 'Fit-window aspect changes do not republish automatic HUD state')

    camera = function_body(g, 'extern "C" void rocket_graphics_camera_begin')
    require('100.0F, raw_matrix' in camera, 'Retail camera shake history path is not fixed at 100%')
    require('maximum_detail' not in camera, 'Removed Maximum visibility still affects camera far clip')
    frustum = function_body(g, 'extern "C" void rocket_graphics_frustum_begin')
    require('context->r7' in frustum and 'draw_distance_multiplier' in frustum, 'v10 real renderDistance r7 fix was lost')
    require('maximum_detail' not in frustum, 'Removed Maximum visibility still affects object distance')

    aspect_update = function_body(w, 'void rocket::widescreen::update_window_aspect')
    require('g_expand_enabled.store(true' in aspect_update, 'Culling guard is not automatically enabled')
    guard = function_body(w, 'extern "C" void rocket_widescreen_frustum_begin')
    for token in ['desired_vertical_half_fov', 'desired_horizontal_half_fov', 'pair_index < order.size()', 'kEdgeGuard']:
        require(token in guard, f'Four-plane FOV culling guard lost: {token}')

    publish = function_body(r, 'void publish_rt64_rocket_controls')
    require('setRocketFogDistanceMultiplier(1.0F)' in publish, 'Fog control is not pinned neutral')
    require('widescreen_active(4.0F / 3.0F)' in publish, 'Automatic HUD does not follow selected aspect')
    require('hud_widescreen ? 1U : 0U' in publish, 'Automatic 4:3/16:9 HUD policy missing')
    apply = function_body(r, 'void rocket::renderer::RT64Context::apply_extra_graphics')
    require('cutscene_active()' not in apply and 'cutscene_aspect' not in apply, 'Renderer still changes aspect from cutscene heuristic')
    require('cutscene_aspect_active_' not in rh and 'cutscene_aspect_active_' not in r, 'Obsolete cutscene-aspect renderer state remains')

    print('[OK] Rocket-R Graphics v11 simplified settings + automatic HUD/culling verification PASS.')

if __name__ == '__main__':
    main()
