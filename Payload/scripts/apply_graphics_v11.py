#!/usr/bin/env python3
from pathlib import Path
import argparse


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    return text, nl, bom


def write_text(path: Path, text: str, nl: str, bom: bool):
    text = text.replace("\r\n", "\n")
    if nl == "\r\n":
        text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"Function marker not found: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace not found: {marker}")
    depth = 0
    state = "code"
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "/" and n == "/":
                state = "line"; i += 2; continue
            if c == "/" and n == "*":
                state = "block"; i += 2; continue
            if c == '"': state = "string"
            elif c == "'": state = "char"
            elif c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return start, i + 1
        elif state == "line":
            if c == "\n": state = "code"
        elif state == "block":
            if c == "*" and n == "/":
                state = "code"; i += 2; continue
        elif state == "string":
            if c == "\\": i += 2; continue
            if c == '"': state = "code"
        elif state == "char":
            if c == "\\": i += 2; continue
            if c == "'": state = "code"
        i += 1
    raise RuntimeError(f"Unterminated function: {marker}")


def replace_function(text: str, marker: str, replacement: str):
    start, end = function_span(text, marker)
    return text[:start] + replacement + text[end:]


def remove_line_containing(text: str, token: str):
    return ''.join(line for line in text.splitlines(keepends=True) if token not in line)


def remove_enum_block(text: str, enum_name: str):
    marker = f"enum class {enum_name}"
    pos = text.find(marker)
    if pos < 0:
        return text
    brace = text.find("{", pos)
    end = text.find("};", brace)
    if brace < 0 or end < 0:
        raise RuntimeError(f"Malformed enum block: {enum_name}")
    end += 2
    while end < len(text) and text[end] in "\r\n":
        end += 1
    return text[:pos] + text[end:]


def patch_header(text: str):
    text = remove_enum_block(text, "HudAspectMode")
    for token in [
        "bool original_aspect_cutscenes",
        "float screen_shake_strength",
        "bool maximum_detail",
        "bool widescreen_culling",
        "float fog_distance_multiplier",
        "HudAspectMode hud_aspect",
        "float hud_scale_percent",
        "float hud_safe_margin_percent",
    ]:
        text = remove_line_containing(text, token)
    return text


SELECTED_ASPECT = r'''float rocket::graphics::selected_aspect(float authored_aspect) {
    const Settings s = settings();
    switch (s.aspect) {
    case AspectPreset::FitWindow:
        return std::max(authored_aspect, window_aspect());
    case AspectPreset::Ratio16x9:
        return std::max(authored_aspect, 16.0F / 9.0F);
    case AspectPreset::Ratio16x10:
        return std::max(authored_aspect, 16.0F / 10.0F);
    case AspectPreset::Ratio21x9:
        return std::max(authored_aspect, 21.0F / 9.0F);
    case AspectPreset::Custom:
        return std::max(authored_aspect, s.custom_aspect);
    case AspectPreset::Original4x3:
    default:
        return authored_aspect;
    }
}'''

WIDESCREEN_ACTIVE = r'''bool rocket::graphics::widescreen_active(float authored_aspect) {
    return selected_aspect(authored_aspect) > authored_aspect * 1.0001F;
}'''

SET_WINDOW_ASPECT = r'''void rocket::graphics::set_window_aspect(float aspect) {
    if (!std::isfinite(aspect) || aspect < 0.5F || aspect > 6.0F) return;
    const float old = g_window_aspect.exchange(aspect, std::memory_order_acq_rel);
    if (!std::isfinite(old) || std::fabs(old - aspect) > 0.0001F) {
        // Fit-window aspect changes can also change the automatic HUD policy.
        // Republish renderer-owned presentation state on the next frame.
        request_renderer_refresh();
    }
}'''

CAMERA_BEGIN = r'''extern "C" void rocket_graphics_camera_begin(std::uint8_t* rdram,
                                                recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    RestoreOwnedCamera(rdram);

    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);
    if (camera_address < kRdramStart ||
        camera_address > kRdramEnd - kCameraBytes) return;
    const gpr camera = GuestAddress(camera_address);

    const Settings s = rocket::graphics::settings();
    const float authored_fov = ReadFloat(rdram, camera, kFovRadiansOffset);
    const float authored_far = ReadFloat(rdram, camera, kFarOffset);
    if (!std::isfinite(authored_fov) || authored_fov <= 0.05F ||
        authored_fov >= kPi - 0.05F || !std::isfinite(authored_far)) return;

    std::array<float, 16> raw_matrix{};
    for (int index = 0; index < 16; ++index) {
        raw_matrix[index] = ReadFloat(rdram, camera,
                                      kViewMatrixOffset + index * 4);
        if (!std::isfinite(raw_matrix[index])) return;
    }

    const float authored_fov_deg = authored_fov * kRadToDeg;
    const bool cinematic = DetectCutscene(camera_address, authored_fov_deg,
                                          raw_matrix);
    g_cutscene_active.store(cinematic, std::memory_order_release);

    g_camera_owned.active = true;
    g_camera_owned.address = camera_address;
    g_camera_owned.saved_fov = authored_fov;
    g_camera_owned.written_fov = authored_fov;
    g_camera_owned.saved_far = authored_far;
    g_camera_owned.written_far = authored_far;

    const float effective_fov =
        rocket::graphics::effective_fov_radians(authored_fov);
    if (!SameBits(effective_fov, authored_fov)) {
        g_camera_owned.written_fov = effective_fov;
        WriteFloat(rdram, camera, kFovRadiansOffset,
                   g_camera_owned.written_fov);
    }

    if (s.draw_distance_multiplier > 1.0001F) {
        // Camera far is stored in sixteenth-world units and multiplied by 16
        // immediately before guPerspective. Draw Distance may extend the far
        // clip, but it never reduces the authored clip plane.
        g_camera_owned.written_far = std::min(
            authored_far * s.draw_distance_multiplier, 32767.0F);
        WriteFloat(rdram, camera, kFarOffset, g_camera_owned.written_far);
    }

    // Screen-shake adjustment is intentionally no longer a user setting.
    // 100% is Rocket's authored camera motion. Calling the history helper at
    // 100% updates cutscene/FOV history without modifying the view matrix.
    ApplyShakeReduction(rdram, camera, camera_address, 100.0F, raw_matrix);
}'''

FRUSTUM_BEGIN = r'''extern "C" void rocket_graphics_frustum_begin(std::uint8_t* rdram,
                                                 recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // func_8003ACD4 has already consumed the temporary camera values into the
    // graphics task by the time object frustum tests begin. Restore the guest
    // camera immediately so presentation-only FOV/far changes cannot leak into
    // later simulation/camera logic during the same authored frame.
    RestoreOwnedCamera(rdram);
    const Settings s = rocket::graphics::settings();

    // Rocket's Vec3f parameter decays to a pointer, so renderDistance is the
    // fourth o32 argument (a3/r7). Keep the v10 ABI repair intact.
    const std::uint32_t bits = static_cast<std::uint32_t>(context->r7);
    const float authored_distance = std::bit_cast<float>(bits);
    if (!std::isfinite(authored_distance) || authored_distance <= 0.0F) return;

    constexpr float kInfiniteRenderDistance = 3.402823466e+38F;
    if (authored_distance >= 3.0e+38F) {
        context->r7 = static_cast<gpr>(
            std::bit_cast<std::uint32_t>(kInfiniteRenderDistance));
        return;
    }

    float adjusted = authored_distance * s.draw_distance_multiplier;
    if (!std::isfinite(adjusted) || adjusted <= 0.0F) return;
    adjusted = std::min(adjusted, 1.0e+20F);
    context->r7 = static_cast<gpr>(std::bit_cast<std::uint32_t>(adjusted));
}'''


def patch_graphics_cpp(text: str):
    for token in [
        "normalized.screen_shake_strength =",
        "normalized.fog_distance_multiplier =",
        "normalized.hud_scale_percent =",
        "normalized.hud_safe_margin_percent =",
        "s.maximum_detail = true;",
        "s.widescreen_culling = true;",
    ]:
        text = remove_line_containing(text, token)
    text = replace_function(text, "void rocket::graphics::set_window_aspect", SET_WINDOW_ASPECT)
    text = replace_function(text, "float rocket::graphics::selected_aspect", SELECTED_ASPECT)
    text = replace_function(text, "bool rocket::graphics::widescreen_active", WIDESCREEN_ACTIVE)
    text = replace_function(text, 'extern "C" void rocket_graphics_camera_begin', CAMERA_BEGIN)
    text = replace_function(text, 'extern "C" void rocket_graphics_frustum_begin', FRUSTUM_BEGIN)
    return text


UPDATE_WINDOW_ASPECT = r'''void rocket::widescreen::update_window_aspect(SDL_Window* window) {
    if (window != nullptr) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (width > 0 && height > 0) {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            if (std::isfinite(aspect) && aspect > 0.0F) {
                g_window_aspect.store(aspect, std::memory_order_release);
                rocket::graphics::set_window_aspect(aspect);
            }
        }
    }

    // No user toggle: the safety guard is always armed. The actual frustum
    // hook is still a no-op unless aspect/FOV expansion would expose geometry
    // beyond Rocket's authored planes.
    g_expand_enabled.store(true, std::memory_order_release);
}'''


def patch_widescreen_cpp(text: str):
    return replace_function(text, "void rocket::widescreen::update_window_aspect", UPDATE_WINDOW_ASPECT)


PUBLISH_CONTROLS = r'''void publish_rt64_rocket_controls(const rocket::graphics::Settings& settings) {
    // Keep the Rocket-specific fog multiplier neutral at all times.
    RT64::setRocketFogDistanceMultiplier(1.0F);
    RT64::setRocketZFightToleranceScale(z_fight_scale(settings.z_fighting));
    RT64::setRocketViFilterMode(static_cast<std::uint32_t>(settings.vi_filter));
    RT64::setRocketDebandStrength(settings.texture_deband
        ? settings.texture_deband_strength / 100.0F : 0.0F);
    RT64::setRocketPostProcess(
        static_cast<std::uint32_t>(settings.post_process),
        settings.post_process_strength / 100.0F);

    // HUD policy is automatic and deterministic:
    //   Original 4:3 -> authored HUD coordinates.
    //   Any wider presentation -> centered 16:9 safe area.
    const bool hud_widescreen =
        rocket::graphics::widescreen_active(4.0F / 3.0F);
    RT64::setRocketHudConfiguration(hud_widescreen ? 1U : 0U, 1.0F, 0.0F);
}'''

APPLY_EXTRA = r'''void rocket::renderer::RT64Context::apply_extra_graphics(bool force) {
    if (!application_) return;

    const std::uint64_t revision = rocket::graphics::revision();
    const auto extra = rocket::graphics::settings();
    if (!force && revision == graphics_revision_) {
        return;
    }

    const auto old_user = application_->userConfig;
    application_->userConfig.filtering = to_rt64(extra.texture_filtering);
    application_->userConfig.threePointFiltering = extra.three_point_filtering;
    application_->userConfig.upscale2D = to_rt64(extra.texture_scaling_2d);
    application_->userConfig.hardwareResolve = to_rt64(extra.hardware_resolve);

    // Aspect framing never switches because of heuristic cutscene detection.
    // The selected aspect remains authoritative for the entire presentation.
    const auto config = ultramodern::renderer::get_graphics_config();
    application_->userConfig.aspectRatio = to_rt64(config.ar_option);
    application_->userConfig.aspectTarget =
        rocket::graphics::selected_aspect(4.0F / 3.0F);

    RT64::setDefaultSamplerMipLODBias(extra.mip_lod_bias);
    publish_rt64_rocket_controls(extra);
    if (application_->shaderLibrary != nullptr) {
        // Preserve RT64's driver-specific Automatic decision. Only explicit
        // On/Off choices override the setup-time compatibility result.
        if (extra.hardware_resolve == rocket::graphics::HardwareResolve::On) {
            application_->shaderLibrary->usesHardwareResolve = true;
        } else if (extra.hardware_resolve ==
                   rocket::graphics::HardwareResolve::Off) {
            application_->shaderLibrary->usesHardwareResolve = false;
        }
    }
    if (application_->swapChain != nullptr) {
        application_->swapChain->setVsyncEnabled(extra.vsync);
    }

    const bool framebuffer_change =
        old_user.aspectRatio != application_->userConfig.aspectRatio ||
        std::fabs(old_user.aspectTarget - application_->userConfig.aspectTarget) > 0.0001 ||
        old_user.upscale2D != application_->userConfig.upscale2D;
    const bool user_change =
        framebuffer_change ||
        old_user.filtering != application_->userConfig.filtering ||
        old_user.threePointFiltering != application_->userConfig.threePointFiltering ||
        old_user.hardwareResolve != application_->userConfig.hardwareResolve;
    if (user_change && application_->sharedQueueResources != nullptr) {
        application_->updateUserConfig(framebuffer_change);
    }

    if (force || revision != graphics_revision_) {
        if (extra.anisotropy != startup_anisotropy_ ||
            extra.display_buffering != startup_buffering_ ||
            extra.custom_shader != startup_custom_shader_) {
            if (!restart_notice_logged_) {
                std::fprintf(stderr,
                    "[rt64][graphics] one or more sampler/buffering/custom-shader "
                    "changes are queued for the next game start\n");
                restart_notice_logged_ = true;
            }
        } else {
            restart_notice_logged_ = false;
        }
    }

    graphics_revision_ = revision;
}'''


def patch_rt64_cpp(text: str):
    text = replace_function(text, "void publish_rt64_rocket_controls", PUBLISH_CONTROLS)
    text = replace_function(text, "void rocket::renderer::RT64Context::apply_extra_graphics", APPLY_EXTRA)
    text = remove_line_containing(text, "cutscene_aspect_active_ = false;")
    return text


def patch_rt64_hpp(text: str):
    return remove_line_containing(text, "bool cutscene_aspect_active_")


def remove_ui_function(text: str, marker: str):
    if marker not in text:
        return text
    start, end = function_span(text, marker)
    while end < len(text) and text[end] in "\r\n": end += 1
    return text[:start] + text[end:]


def patch_runtime_ui(text: str):
    for token in [
        'out << "cutscene_original_aspect="',
        'out << "screen_shake_strength="',
        'out << "maximum_detail="',
        'out << "widescreen_culling="',
        'out << "fog_distance="',
        'out << "hud_aspect="',
        'out << "hud_scale="',
        'out << "hud_safe_margin="',
        'key == "cutscene_original_aspect"',
        'key == "screen_shake_strength"',
        'key == "maximum_detail"',
        'key == "widescreen_culling"',
        'key == "fog_distance"',
        'key == "hud_aspect"',
        'key == "hud_scale"',
        'key == "hud_safe_margin"',
    ]:
        text = remove_line_containing(text, token)

    text = remove_ui_function(text, "const char* HudAspectName")

    old_buttons = ('    GraphicsSectionButton("WORLD / CAMERA", 2); ImGui::SameLine();\n'
                   '    GraphicsSectionButton("HUD", 3); ImGui::SameLine();\n'
                   '    GraphicsSectionButton("DIAGNOSTICS", 4);')
    new_buttons = ('    GraphicsSectionButton("WORLD / CAMERA", 2); ImGui::SameLine();\n'
                   '    GraphicsSectionButton("DIAGNOSTICS", 3);')
    if old_buttons in text:
        text = text.replace(old_buttons, new_buttons)
    elif 'GraphicsSectionButton("HUD", 3)' in text:
        raise RuntimeError("Graphics section button layout did not match expected v10 baseline")

    marker = '        if (extra.aspect == rocket::graphics::AspectPreset::Custom) {'
    pos = text.find(marker)
    if pos < 0:
        raise RuntimeError("Aspect custom UI marker not found")
    b = text.find('{', pos)
    depth = 0
    end = None
    for i in range(b, len(text)):
        if text[i] == '{': depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end is None:
        raise RuntimeError("Custom aspect UI block malformed")
    if "HUD framing is automatic" not in text:
        text = text[:end] + ('\n        ImGui::TextDisabled("HUD framing is automatic: original in 4:3; "\n'
                            '                            "centered 16:9 safe area in widescreen modes.");') + text[end:]

    world_start = text.find('    } else if (g_graphics_section == 2) {')
    hud_start = text.find('    } else if (g_graphics_section == 3) {', world_start)
    if world_start < 0:
        raise RuntimeError("World graphics section marker not found")

    # If HUD was already removed by a prior run, the v11 world markers prove the
    # transform is already installed; avoid searching for the old HUD block.
    if hud_start < 0 and "Aspect/FOV culling protection is automatic" in text:
        return text
    if hud_start < 0:
        raise RuntimeError("HUD graphics section marker not found")

    world_block = r'''    } else if (g_graphics_section == 2) {
        ImGui::TextUnformatted("Field of view offset");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##fov-offset", &extra.fov_offset_degrees, -20.0F, 40.0F, "%+.1f deg")) extra_changed = true;
        if (ImGui::Checkbox("Preserve authored cutscene FOV", &extra.preserve_cutscene_fov)) extra_changed = true;
        ImGui::TextDisabled("When enabled, detected authored FOV changes are preserved. Aspect framing itself never changes automatically.");

        ImGui::TextUnformatted("Draw distance");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##draw-distance", &extra.draw_distance_multiplier, 0.5F, 8.0F, "%.2fx")) extra_changed = true;
        ImGui::TextDisabled("Scales Rocket's real object render-distance argument before its original cull/fade calculation.");
        ImGui::TextDisabled("Aspect/FOV culling protection is automatic and covers left/right/top/bottom with the built-in safety guard.");
'''
    text = text[:world_start] + world_block + text[hud_start:]

    hud_start = text.find('    } else if (g_graphics_section == 3) {')
    diag_else = text.find('    } else {', hud_start + 1)
    if hud_start < 0 or diag_else < 0:
        raise RuntimeError("HUD/Diagnostics branch markers not found")
    text = text[:hud_start] + text[diag_else:]
    return text


def patch_verifier_v9(text: str):
    for token in [
        "'HudAspectMode',",
        "'screen_shake_strength',",
        "'maximum_detail',",
        "'fog_distance_multiplier',",
        "'ApplyShakeReduction'",
    ]:
        text = text.replace(token, "")
    start = text.find("    ui_tokens=[")
    end = text.find("    for token in ui_tokens:", start)
    if start >= 0 and end >= 0:
        repl = '''    ui_tokens=[\n        'Graphics API','Window mode','VSync','Aspect ratio','Resolution','Frame rate','Display buffering',\n        'Framebuffer precision','Hardware resolve','Anti-aliasing','Output scaling filter',\n        'N64 three-point texture filtering','2D scaling policy','Anisotropic filtering','Texture mip LOD bias',\n        'Clean VI output','Deband smooth gradients','Z-fighting reduction','Post-process shader',\n        'Field of view offset','Preserve authored cutscene FOV','Draw distance',\n        'Performance overlay','Interpolation coverage overlay','RESET GRAPHICS TO ORIGINAL'\n    ]\n'''
        text = text[:start] + repl + text[end:]
    return text


def patch_verifier_v10(text: str):
    if not text:
        return text
    text = text.replace('        "s.maximum_detail",\n', '')
    text = text.replace('        "settings().widescreen_culling",\n', '')
    old = '''    for token in [\n        "Maximum visibility (disable distance culling)",\n        "Widescreen culling fix",\n        "left/right/top/bottom automatically",\n        "real object render-distance argument",\n    ]:\n        if token not in ui: fail(f"Graphics UI missing v10 behavior marker: {token}")\n'''
    new = '''    for token in [\n        "Draw distance",\n        "Aspect/FOV culling protection is automatic",\n        "left/right/top/bottom",\n        "real object render-distance argument",\n    ]:\n        if token not in ui: fail(f"Graphics UI missing v10/v11 behavior marker: {token}")\n'''
    text = text.replace(old, new)
    return text


def patch(root: Path):
    paths = {
        'h': root/'src/graphics_enhancements.hpp',
        'g': root/'src/graphics_enhancements.cpp',
        'w': root/'src/widescreen_culling.cpp',
        'u': root/'src/runtime_ui.cpp',
        'r': root/'src/rt64_renderer.cpp',
        'rh': root/'src/rt64_renderer.hpp',
    }
    for p in paths.values():
        if not p.is_file():
            raise RuntimeError(f"Required source file not found: {p}")

    funcs = {'h': patch_header, 'g': patch_graphics_cpp, 'w': patch_widescreen_cpp,
             'u': patch_runtime_ui, 'r': patch_rt64_cpp, 'rh': patch_rt64_hpp}
    for key, func in funcs.items():
        text, nl, bom = read_text(paths[key])
        write_text(paths[key], func(text), nl, bom)

    v9 = root/'scripts/verify_graphics_v9.py'
    if v9.is_file():
        text, nl, bom = read_text(v9)
        write_text(v9, patch_verifier_v9(text), nl, bom)
    v10 = root/'scripts/verify_view_distance_culling_v10.py'
    if v10.is_file():
        text, nl, bom = read_text(v10)
        write_text(v10, patch_verifier_v10(text), nl, bom)

    print("[OK] Removed heuristic 4:3 cutscene framing control and behavior.")
    print("[OK] Removed Screen shake, Maximum visibility, Widescreen culling, Fog and HUD settings from UI/persistence.")
    print("[OK] Culling guard is automatic and remains four-plane/FOV-aware.")
    print("[OK] HUD is automatic: authored 4:3 in Original mode, centered 16:9-safe presentation in widescreen.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    patch(Path(args.root).resolve())

if __name__ == '__main__':
    main()
