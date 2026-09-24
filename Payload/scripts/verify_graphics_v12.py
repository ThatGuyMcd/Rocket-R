#!/usr/bin/env python3
from pathlib import Path
import argparse
import re


def fail(msg):
    print('[ERROR] ' + msg)
    raise SystemExit(1)


def require(cond, msg):
    if not cond:
        fail(msg)


def read(path):
    return path.read_text(encoding='utf-8-sig')


def function_body(text, marker):
    start = text.find(marker)
    if start < 0:
        fail(f'Missing function: {marker}')
    brace = text.find('{', start)
    depth = 0
    state = 'code'
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i+1] if i+1 < len(text) else ''
        if state == 'code':
            if c == '/' and n == '/': state='line'; i+=2; continue
            if c == '/' and n == '*': state='block'; i+=2; continue
            if c == '"': state='string'
            elif c == "'": state='char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return text[start:i+1]
        elif state == 'line':
            if c == '\n': state='code'
        elif state == 'block':
            if c == '*' and n == '/': state='code'; i+=2; continue
        elif state == 'string':
            if c == '\\': i+=2; continue
            if c == '"': state='code'
        elif state == 'char':
            if c == '\\': i+=2; continue
            if c == "'": state='code'
        i += 1
    fail(f'Unterminated function: {marker}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    root = Path(ap.parse_args().root).resolve()

    g = read(root/'src/graphics_enhancements.cpp')
    w = read(root/'src/widescreen_culling.cpp')
    u = read(root/'src/runtime_ui.cpp')
    r = read(root/'src/rt64_renderer.cpp')

    retired_ui = [
        'Preserve authored cutscene FOV',
        'N64 three-point texture filtering',
        '2D scaling policy',
        'N64 colour dithering',
        'Clean VI output',
        'Deband smooth gradients',
        'Z-fighting reduction',
    ]
    for token in retired_ui:
        require(token not in u, f'Retired control still visible: {token}')

    retired_keys = [
        'n64_dithering=', 'three_point_filtering=', 'texture_scaling_2d=',
        'preserve_cutscene_fov=', 'vi_filter=', 'texture_deband=',
        'texture_deband_strength=', 'z_fighting=',
    ]
    for token in retired_keys:
        require(token not in u, f'Retired setting still persisted: {token}')

    fov = function_body(g, 'float rocket::graphics::effective_fov_radians')
    require('preserve_cutscene_fov' not in fov and 'cutscene_active()' not in fov,
            'FOV still depends on heuristic cutscene detection')

    require('std::lround(normalized.draw_distance_multiplier)' in g,
            'Draw-distance normalization is not integer stepped')
    require('1, 6' in g, 'Draw-distance normalization is not clamped to 1..6')

    world_pos = u.find('ImGui::TextUnformatted("Draw distance")')
    require(world_pos >= 0, 'Draw Distance UI missing')
    world_chunk = u[world_pos:world_pos+1500]
    require('ImGui::SliderInt("##draw-distance"' in world_chunk,
            'Draw Distance is not an integer slider')
    require('&draw_distance_step, 1, 6, "%dx"' in world_chunk,
            'Draw Distance slider is not exactly 1x..6x')
    require('Six exact steps: 1x, 2x, 3x, 4x, 5x or 6x.' in world_chunk,
            'Draw Distance exact-step explanation missing')

    cull = function_body(w, 'extern "C" void rocket_widescreen_frustum_begin')
    require('position_address = static_cast<std::uint32_t>(context->r5)' in cull,
            'Object-local culling does not read the actual position pointer')
    require('authored_radius = std::bit_cast<float>' in cull and 'context->r6' in cull,
            'Object-local culling does not operate on the actual cullRadius argument')
    require('context->r6 = static_cast<gpr>' in cull,
            'Object-local culling does not publish the relaxed cull radius')
    require('WriteVec3(' not in cull,
            'v12 culling still rewrites shared camera frustum planes')
    require('target_diagonal_half' in cull and 'sphere_angle' in cull,
            'Object-local expanded-FOV cone test is missing')
    require('required_radius = std::max(required_radius, plane_distance)' in cull,
            'Object-local authored-plane relaxation is missing')
    require('kAngularHysteresis' in cull and 'distance * 0.003F' in cull,
            'Culling hysteresis guard is missing')

    publish = function_body(r, 'void publish_rt64_rocket_controls')
    for token in [
        'setRocketZFightToleranceScale(1.0F)',
        'setRocketViFilterMode(0U)',
        'setRocketDebandStrength(0.0F)',
    ]:
        require(token in publish, f'Retired renderer control is not pinned neutral: {token}')

    require('application_->userConfig.threePointFiltering = true;' in r,
            'Three-point filtering stable default is not pinned')
    require('application_->userConfig.upscale2D = RT64::UserConfiguration::Upscale2D::ScaledOnly;' in r,
            '2D scaling stable default is not pinned')

    dither = function_body(u, 'bool rocket::ui::n64_dithering_enabled()')
    require('return true;' in dither,
            'Retired N64 dithering control is not pinned to its established default')

    print('[OK] Rocket-R Graphics v12 stable culling + UI cleanup verification PASS.')


if __name__ == '__main__':
    main()
