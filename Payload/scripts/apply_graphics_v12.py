#!/usr/bin/env python3
from pathlib import Path
import argparse
import re
import ast


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
            if c == "/" and n == "/": state = "line"; i += 2; continue
            if c == "/" and n == "*": state = "block"; i += 2; continue
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
            if c == "*" and n == "/": state = "code"; i += 2; continue
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


EFFECTIVE_FOV = r'''float rocket::graphics::effective_fov_radians(float authored_fov_radians) {
    if (!std::isfinite(authored_fov_radians) || authored_fov_radians <= 0.05F ||
        authored_fov_radians >= kPi - 0.05F) {
        return authored_fov_radians;
    }
    const Settings s = settings();
    if (std::fabs(s.fov_offset_degrees) <= 0.001F) {
        return authored_fov_radians;
    }
    const float adjusted_deg = std::clamp(
        authored_fov_radians * kRadToDeg + s.fov_offset_degrees,
        25.0F, 120.0F);
    return adjusted_deg * kDegToRad;
}'''


CULLING_REPLACEMENT = r'''extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;
    if (!g_expand_enabled.load(std::memory_order_acquire)) return;

    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);
    const std::uint32_t position_address = static_cast<std::uint32_t>(context->r5);
    if (camera_address < kRdramStart || camera_address > kRdramEnd - kCameraBytes ||
        position_address < kRdramStart || position_address > kRdramEnd - 12U) {
        return;
    }

    const float authored_radius = std::bit_cast<float>(
        static_cast<std::uint32_t>(context->r6));
    if (!std::isfinite(authored_radius) || authored_radius < 0.0F) return;

    const gpr camera = GuestAddress(camera_address);
    const gpr position_ptr = GuestAddress(position_address);
    const float authored_aspect = ReadFloat(rdram, camera, kAspectOffset);
    const float authored_fov_y = ReadFloat(rdram, camera, kFovYRadiansOffset);
    const float effective_fov_y =
        rocket::graphics::effective_fov_radians(authored_fov_y);
    if (!std::isfinite(authored_aspect) || authored_aspect <= 0.1F ||
        !std::isfinite(authored_fov_y) || authored_fov_y <= 0.01F ||
        authored_fov_y >= kPi - 0.01F ||
        !std::isfinite(effective_fov_y) || effective_fov_y <= 0.01F ||
        effective_fov_y >= kPi - 0.01F) {
        return;
    }

    const float selected_aspect =
        rocket::graphics::selected_aspect(authored_aspect);
    if (!std::isfinite(selected_aspect) || selected_aspect <= 0.1F) return;

    const float target_aspect = std::max(authored_aspect, selected_aspect);
    const float target_fov_y = std::max(authored_fov_y, effective_fov_y);
    const bool aspect_expanded =
        target_aspect > authored_aspect * 1.0001F;
    const bool fov_expanded =
        target_fov_y > authored_fov_y + 0.0001F;
    if (!aspect_expanded && !fov_expanded) return;

    const Vec3 eye = ReadVec3(rdram, camera, 0);
    const Vec3 position = ReadVec3(rdram, position_ptr, 0);
    if (!Finite(eye) || !Finite(position)) return;
    const Vec3 camera_offset{
        position.x - eye.x,
        position.y - eye.y,
        position.z - eye.z,
    };
    const float distance = Length(camera_offset);
    if (!std::isfinite(distance) || distance <= kMinimumVectorLength) return;

    Vec3 forward{};
    if (!Normalize(ReadVec3(rdram, camera, kForwardOffset), forward)) return;

    // The first row of Rocket's camera basis is the depth axis. Orient it so
    // the visible half-space points away from all four authored outward plane
    // normals. This keeps the test stable even if a camera convention changes.
    float orientation_score = 0.0F;
    std::array<Vec3, 4> planes{};
    for (std::size_t index = 0; index < planes.size(); ++index) {
        planes[index] = ReadVec3(
            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);
        if (!Finite(planes[index])) return;
        Vec3 unit_plane{};
        if (!Normalize(planes[index], unit_plane)) return;
        orientation_score += Dot(unit_plane, forward);
    }
    if (orientation_score > 0.0F) {
        forward = Scale(forward, -1.0F);
    }

    const float depth = Dot(camera_offset, forward);
    if (!std::isfinite(depth) || depth <= -authored_radius) return;

    // Do NOT rewrite Rocket's shared camera planes. The old v10/v11 approach
    // did that globally and the result could vary with camera orientation at
    // large FOVs. Instead, identify objects whose bounding sphere intersects
    // the expanded presentation cone and relax only that object's cull radius
    // for this one frustum_test call.
    constexpr float kObjectCullGuard = 1.10F;
    constexpr float kAngularHysteresis = 0.034906585F; // 2 degrees.
    float vertical_tangent = std::tan(target_fov_y * 0.5F);
    if (!std::isfinite(vertical_tangent) || vertical_tangent <= 0.0F) return;
    vertical_tangent *= kObjectCullGuard;
    const float horizontal_tangent = vertical_tangent * target_aspect;
    const float diagonal_tangent = std::sqrt(
        vertical_tangent * vertical_tangent +
        horizontal_tangent * horizontal_tangent);
    if (!std::isfinite(diagonal_tangent)) return;

    const float target_diagonal_half = std::atan(diagonal_tangent);
    const float cos_angle = std::clamp(depth / distance, -1.0F, 1.0F);
    const float center_angle = std::acos(cos_angle);
    const float sphere_ratio = std::clamp(authored_radius / distance, 0.0F, 1.0F);
    const float sphere_angle = std::asin(sphere_ratio);
    if (center_angle > target_diagonal_half + sphere_angle + kAngularHysteresis) {
        return;
    }

    float required_radius = authored_radius;
    for (const Vec3& plane : planes) {
        const float plane_distance = Dot(camera_offset, plane);
        if (!std::isfinite(plane_distance)) return;
        required_radius = std::max(required_radius, plane_distance);
    }
    if (required_radius <= authored_radius) return;

    // Small distance-scaled hysteresis keeps an object from oscillating exactly
    // on the authored plane while the camera/interpolated presentation moves.
    const float margin = std::max(0.5F, distance * 0.003F);
    const float adjusted_radius = required_radius + margin;
    if (!std::isfinite(adjusted_radius) || adjusted_radius <= 0.0F ||
        adjusted_radius >= 1.0e+20F) {
        return;
    }

    context->r6 = static_cast<gpr>(
        std::bit_cast<std::uint32_t>(adjusted_radius));

    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[culling] object-local aspect/FOV guard active; camera planes are "
            "left untouched (aspect %.3f->%.3f, vertical FOV %.1f->%.1f deg)\n",
            authored_aspect, target_aspect,
            authored_fov_y * (180.0F / kPi),
            target_fov_y * (180.0F / kPi));
    }
}'''


PUBLISH_CONTROLS = r'''void publish_rt64_rocket_controls(const rocket::graphics::Settings& settings) {
    // Retired Image controls are pinned to stable/neutral values instead of
    // being kept as hidden configuration switches.
    RT64::setRocketFogDistanceMultiplier(1.0F);
    RT64::setRocketZFightToleranceScale(1.0F);
    RT64::setRocketViFilterMode(0U);
    RT64::setRocketDebandStrength(0.0F);
    RT64::setRocketPostProcess(
        static_cast<std::uint32_t>(settings.post_process),
        settings.post_process_strength / 100.0F);

    const bool hud_widescreen =
        rocket::graphics::widescreen_active(4.0F / 3.0F);
    RT64::setRocketHudConfiguration(hud_widescreen ? 1U : 0U, 1.0F, 0.0F);
}'''


IMAGE_SECTION = r'''    } else if (g_graphics_section == 1) {
        ImGui::TextUnformatted("Anti-aliasing");
        if (ImGui::BeginCombo("##msaa", MsaaName(config.msaa_option))) {
            const std::array<ultramodern::renderer::Antialiasing, 4> values{
                ultramodern::renderer::Antialiasing::None,
                ultramodern::renderer::Antialiasing::MSAA2X,
                ultramodern::renderer::Antialiasing::MSAA4X,
                ultramodern::renderer::Antialiasing::MSAA8X};
            for (auto value : values) {
                if (ImGui::Selectable(MsaaName(value), config.msaa_option == value)) {
                    config.msaa_option = value; config_changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextUnformatted("Output scaling filter");
        if (ImGui::BeginCombo("##texture-filter", TextureFilteringName(extra.texture_filtering))) {
            for (auto value : {rocket::graphics::TextureFiltering::Nearest,
                               rocket::graphics::TextureFiltering::Linear,
                               rocket::graphics::TextureFiltering::AntiAliasedPixelScaling}) {
                if (ImGui::Selectable(TextureFilteringName(value), extra.texture_filtering == value)) {
                    extra.texture_filtering = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TextUnformatted("Anisotropic filtering");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderInt("##anisotropy", &extra.anisotropy, 1, 16, "%dx")) extra_changed = true;
        if (in_game) ImGui::TextDisabled("Anisotropy is baked into RT64 sampler objects and applies on the next game start.");

        ImGui::TextUnformatted("Texture mip LOD bias");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##mip-bias", &extra.mip_lod_bias, -2.0F, 2.0F, "%+.2f")) extra_changed = true;
        ImGui::TextDisabled("Negative values sharpen mip selection; positive values favour lower-detail mips.");

        ImGui::TextUnformatted("Post-process shader");
        if (ImGui::BeginCombo("##post-process", PostProcessName(extra.post_process))) {
            for (auto value : {rocket::graphics::PostProcessMode::Off,
                               rocket::graphics::PostProcessMode::Scanlines,
                               rocket::graphics::PostProcessMode::CRT,
                               rocket::graphics::PostProcessMode::Custom}) {
                if (ImGui::Selectable(PostProcessName(value), extra.post_process == value)) {
                    extra.post_process = value; extra_changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (extra.post_process != rocket::graphics::PostProcessMode::Off) {
            ImGui::SetNextItemWidth(control_width);
            if (ImGui::SliderFloat("##post-strength", &extra.post_process_strength, 0.0F, 100.0F, "%.0f%%")) extra_changed = true;
        }
        if (extra.post_process == rocket::graphics::PostProcessMode::Custom) {
            const auto stems = CustomShaderStems();
            const char* preview = extra.custom_shader.empty() ? "No shader selected" : extra.custom_shader.c_str();
            if (ImGui::BeginCombo("##custom-shader", preview)) {
                for (const auto& stem : stems) {
                    if (ImGui::Selectable(stem.c_str(), extra.custom_shader == stem)) {
                        extra.custom_shader = stem; extra_changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (stems.empty()) {
                ImGui::TextDisabled("Place matching <name>.dxil and/or <name>.spv binaries in the Rocket-R config/shaders folder.");
            }
            ImGui::TextDisabled("Custom shader binaries are loaded when RT64 starts; restart the game after changing the selection.");
        }
'''


WORLD_SECTION = r'''    } else if (g_graphics_section == 2) {
        ImGui::TextUnformatted("Field of view offset");
        ImGui::SetNextItemWidth(control_width);
        if (ImGui::SliderFloat("##fov-offset", &extra.fov_offset_degrees, -20.0F, 40.0F, "%+.1f deg")) extra_changed = true;

        ImGui::TextUnformatted("Draw distance");
        ImGui::SetNextItemWidth(control_width);
        int draw_distance_step = std::clamp(
            static_cast<int>(std::lround(extra.draw_distance_multiplier)), 1, 6);
        if (ImGui::SliderInt("##draw-distance", &draw_distance_step, 1, 6, "%dx")) {
            extra.draw_distance_multiplier = static_cast<float>(draw_distance_step);
            extra_changed = true;
        }
        ImGui::TextDisabled("Six exact steps: 1x, 2x, 3x, 4x, 5x or 6x.");
        ImGui::TextDisabled("Automatic FOV/aspect culling protection is object-local and does not rewrite Rocket's camera planes.");
'''


def patch_graphics_cpp(text: str):
    text = replace_function(text, "float rocket::graphics::effective_fov_radians", EFFECTIVE_FOV)
    old = "normalized.draw_distance_multiplier = std::clamp(normalized.draw_distance_multiplier, 0.5F, 8.0F);"
    new = "normalized.draw_distance_multiplier = static_cast<float>(std::clamp(static_cast<int>(std::lround(normalized.draw_distance_multiplier)), 1, 6));"
    if old in text:
        text = text.replace(old, new)
    elif new not in text:
        raise RuntimeError("Draw-distance normalization marker not found")

    # Hidden/retired controls are forced to deterministic values so old INI or
    # preset data cannot continue to affect rendering invisibly.
    marker = new
    fixed = '''\n    normalized.preserve_cutscene_fov = false;\n    normalized.three_point_filtering = true;\n    normalized.texture_scaling_2d = TextureScaling2D::ScaledOnly;\n    normalized.vi_filter = ViFilterMode::Authentic;\n    normalized.texture_deband = false;\n    normalized.texture_deband_strength = 0.0F;\n    normalized.z_fighting = ZFightingMode::Original;'''
    if "normalized.preserve_cutscene_fov = false;" not in text:
        text = text.replace(marker, marker + fixed)

    text = text.replace("s.draw_distance_multiplier = 1.5F;", "s.draw_distance_multiplier = 2.0F;")
    return text


def patch_widescreen_cpp(text: str):
    text = text.replace("constexpr float kEdgeGuard = 1.05F;", "constexpr float kEdgeGuard = 1.10F;")
    text = replace_function(text, 'extern "C" void rocket_widescreen_frustum_begin', CULLING_REPLACEMENT)
    return text


def replace_branch(text: str, start_marker: str, end_marker: str, replacement: str):
    start = text.find(start_marker)
    end = text.find(end_marker, start + len(start_marker))
    if start < 0 or end < 0:
        raise RuntimeError(f"UI branch markers not found: {start_marker} / {end_marker}")
    return text[:start] + replacement + text[end:]


def patch_runtime_ui(text: str):
    # Remove obsolete persistence. Existing INI keys will be ignored and will
    # disappear on the next settings save.
    for token in [
        'out << "n64_dithering="',
        'out << "three_point_filtering="',
        'out << "texture_scaling_2d="',
        'out << "preserve_cutscene_fov="',
        'out << "vi_filter="',
        'out << "texture_deband="',
        'out << "texture_deband_strength="',
        'out << "z_fighting="',
        'key == "n64_dithering"',
        'key == "three_point_filtering"',
        'key == "texture_scaling_2d"',
        'key == "preserve_cutscene_fov"',
        'key == "vi_filter"',
        'key == "texture_deband"',
        'key == "texture_deband_strength"',
        'key == "z_fighting"',
        'bool n64_dithering = true;',
        'std::atomic<bool> g_n64_dithering_enabled{true};',
        'g_n64_dithering_enabled.store(n64_dithering',
        'g_n64_dithering_enabled.store(true',
    ]:
        text = remove_line_containing(text, token)

    text = replace_branch(
        text,
        '    } else if (g_graphics_section == 1) {',
        '    } else if (g_graphics_section == 2) {',
        IMAGE_SECTION)
    text = replace_branch(
        text,
        '    } else if (g_graphics_section == 2) {',
        '    } else {',
        WORLD_SECTION)

    # Keep the v7 API for compatibility, but it is no longer user-configurable.
    if "bool rocket::ui::n64_dithering_enabled()" in text:
        text = replace_function(
            text,
            "bool rocket::ui::n64_dithering_enabled()",
            "bool rocket::ui::n64_dithering_enabled() {\n    return true;\n}")
    return text


def patch_renderer(text: str):
    text = replace_function(text, "void publish_rt64_rocket_controls", PUBLISH_CONTROLS)
    text = text.replace(
        "application_->userConfig.threePointFiltering = extra.three_point_filtering;",
        "application_->userConfig.threePointFiltering = true;")
    text = text.replace(
        "application_->userConfig.upscale2D = to_rt64(extra.texture_scaling_2d);",
        "application_->userConfig.upscale2D = RT64::UserConfiguration::Upscale2D::ScaledOnly;")
    return text


def patch_verifier_v9(text: str):
    # Remove retired UI/model expectations while preserving all remaining checks.
    for token in [
        "'N64 three-point texture filtering',",
        "'2D scaling policy',",
        "'Clean VI output',",
        "'Deband smooth gradients',",
        "'Z-fighting reduction',",
        "'Preserve authored cutscene FOV',",
        "'preserve_cutscene_fov',",
    ]:
        text = text.replace(token, "")
    text = text.replace("'kEdgeGuard = 1.05F'", "'kEdgeGuard = 1.10F'")
    text = text.replace('draw-distance hook must run before widescreen frustum widening',
                        'draw-distance hook must run before object-local FOV culling')
    return text


def patch_verifier_v11(text: str):
    if not text:
        return text
    text = text.replace(
        "    require('Preserve authored cutscene FOV' in u, 'Preserve authored cutscene FOV should remain available')\n",
        "    require('Preserve authored cutscene FOV' not in u, 'Retired cutscene-FOV option is still visible')\n")
    text = text.replace(
        "    require('Aspect/FOV culling protection is automatic' in u, 'Automatic culling explanation missing')\n",
        "    require('object-local' in u, 'Automatic object-local culling explanation missing')\n")
    old = """    guard = function_body(w, 'extern \"C\" void rocket_widescreen_frustum_begin')
    for token in ['desired_vertical_half_fov', 'desired_horizontal_half_fov', 'pair_index < order.size()', 'kEdgeGuard']:
        require(token in guard, f'Four-plane FOV culling guard lost: {token}')
"""
    new = """    guard = function_body(w, 'extern \"C\" void rocket_widescreen_frustum_begin')
    for token in ['position_address', 'context->r6', 'target_diagonal_half', 'required_radius', 'kObjectCullGuard']:
        require(token in guard, f'Object-local FOV culling guard lost: {token}')
    require('WriteVec3(' not in guard, 'v12 culling still rewrites shared camera frustum planes')
"""
    text = text.replace(old, new)
    text = text.replace('simplified settings + automatic HUD/culling verification PASS.',
                        'simplified settings + automatic HUD/object-local culling verification PASS.')
    return text


def patch_verifier_v10(text: str):
    if not text:
        return text
    start = text.find('    aspect=function_body(culling')
    end = text.find('    policy=root/"runtime-recomp/rocket.us.recomp-policy.json"', start)
    if start >= 0 and end >= 0:
        repl = """    aspect=function_body(culling,'void rocket::widescreen::update_window_aspect')
    guard=function_body(culling,'extern \"C\" void rocket_widescreen_frustum_begin')
    for token in [
        "effective_fov_radians",
        "selected_aspect",
        "target_fov_y",
        "target_aspect",
        "position_address",
        "context->r6",
        "target_diagonal_half",
        "required_radius",
        "kObjectCullGuard",
    ]:
        if token not in aspect + guard:
            fail(f"v12 object-local culling guard missing token: {token}")
    if "WriteVec3(" in guard:
        fail("v12 culling must not rewrite shared camera frustum planes")

    for token in [
        "Draw distance",
        "Six exact steps: 1x, 2x, 3x, 4x, 5x or 6x.",
        "object-local",
    ]:
        if token not in ui: fail(f"Graphics UI missing v12 behavior marker: {token}")

"""
        text = text[:start] + repl + text[end:]
    text = text.replace('Graphics v10 view-distance + four-plane FOV culling verification PASS.',
                        'Graphics v10/v12 view-distance + object-local FOV culling verification PASS.')
    return text



STALE_FIXED34_CULLING_MESSAGE = (
    "FIXED34 must widen only Rocket's horizontal CPU frustum in Expand mode "
    "with a guarded, idempotent window-aspect bridge"
)

NEW_FIXED34_CULLING_MESSAGE = (
    "FIXED34/v12 must use object-local FOV/aspect culling, preserve the "
    "window-aspect bridge, and avoid rewriting shared camera frustum planes"
)


def _statement_for_string(tree: ast.AST, message: str):
    parents = {}
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            parents[child] = parent

    matches = [
        node for node in ast.walk(tree)
        if isinstance(node, ast.Constant) and node.value == message
    ]
    if len(matches) != 1:
        return None

    node = matches[0]
    stmt = None
    while node in parents:
        node = parents[node]
        if isinstance(node, ast.stmt):
            stmt = node
            break

    # Handle the common:
    #   if not condition:
    #       fail("message")
    # form as one logical assertion rather than replacing only its body.
    if stmt is not None:
        parent = parents.get(stmt)
        if isinstance(parent, ast.If) and len(parent.body) == 1 and parent.body[0] is stmt:
            stmt = parent
    return stmt


def patch_self_check_v121(text: str):
    # Idempotence: once the migrated assertion exists, leave it alone.
    if NEW_FIXED34_CULLING_MESSAGE in text:
        return text

    if STALE_FIXED34_CULLING_MESSAGE not in text:
        raise RuntimeError(
            "self_check.py does not contain the expected FIXED34 culling assertion; "
            "refusing to weaken an unknown source-integrity check"
        )

    try:
        tree = ast.parse(text)
    except SyntaxError as exc:
        raise RuntimeError(f"Unable to parse self_check.py: {exc}") from exc

    stmt = _statement_for_string(tree, STALE_FIXED34_CULLING_MESSAGE)
    if stmt is None or not hasattr(stmt, "lineno") or not hasattr(stmt, "end_lineno"):
        raise RuntimeError(
            "Could not isolate the stale FIXED34 culling assertion in self_check.py"
        )

    # Only replace assertion-shaped statements. This avoids accidentally
    # deleting a larger table/list of unrelated integrity checks.
    if not isinstance(stmt, (ast.Expr, ast.Assert, ast.If)):
        raise RuntimeError(
            "The stale FIXED34 culling message is embedded in an unexpected "
            f"{type(stmt).__name__}; refusing an unsafe self-check rewrite"
        )

    lines = text.splitlines(keepends=True)
    start = stmt.lineno - 1
    end = stmt.end_lineno
    indent = re.match(r"[ \t]*", lines[start]).group(0)

    replacement_lines = [
        indent + "# v12.1: FIXED34's old horizontal-plane assertion is obsolete.\n",
        indent + "_v121_culling_path = (__import__('pathlib').Path(__file__).resolve().parents[1] / 'src' / 'widescreen_culling.cpp')\n",
        indent + "_v121_culling = _v121_culling_path.read_text(encoding='utf-8-sig')\n",
        indent + "_v121_required = all(token in _v121_culling for token in (\n",
        indent + "    'void rocket::widescreen::update_window_aspect(SDL_Window* window)',\n",
        indent + "    'position_address = static_cast<std::uint32_t>(context->r5)',\n",
        indent + "    'authored_radius = std::bit_cast<float>',\n",
        indent + "    'target_diagonal_half',\n",
        indent + "    'required_radius = std::max(required_radius, plane_distance)',\n",
        indent + "    'context->r6 = static_cast<gpr>',\n",
        indent + "    'kObjectCullGuard',\n",
        indent + "    'kAngularHysteresis',\n",
        indent + "))\n",
        indent + "_v121_retired_shared_rewrite = (\n",
        indent + "    'pair_index < order.size()' in _v121_culling or\n",
        indent + "    '[culling] CPU frustum guard now covers left/right/top/bottom' in _v121_culling\n",
        indent + ")\n",
        indent + "if not (_v121_required and not _v121_retired_shared_rewrite):\n",
        indent + "    raise SystemExit('SOURCE SELF-CHECK FAILED: " + NEW_FIXED34_CULLING_MESSAGE.replace("'", "\\'") + "')\n",
    ]

    return "".join(lines[:start] + replacement_lines + lines[end:])

def patch(root: Path):
    files = {
        'g': root/'src/graphics_enhancements.cpp',
        'w': root/'src/widescreen_culling.cpp',
        'u': root/'src/runtime_ui.cpp',
        'r': root/'src/rt64_renderer.cpp',
    }
    for p in files.values():
        if not p.is_file():
            raise RuntimeError(f"Required source file not found: {p}")

    funcs = {
        'g': patch_graphics_cpp,
        'w': patch_widescreen_cpp,
        'u': patch_runtime_ui,
        'r': patch_renderer,
    }
    for key, func in funcs.items():
        text, nl, bom = read_text(files[key])
        write_text(files[key], func(text), nl, bom)

    self_check = root/'scripts/self_check.py'
    if not self_check.is_file():
        raise RuntimeError("Required source-integrity file not found: scripts/self_check.py")
    sc_text, sc_nl, sc_bom = read_text(self_check)
    write_text(self_check, patch_self_check_v121(sc_text), sc_nl, sc_bom)
    print("[OK] Migrated stale FIXED34 horizontal-frustum self-check to v12 object-local culling.")

    for rel, func in [
        ('scripts/verify_graphics_v9.py', patch_verifier_v9),
        ('scripts/verify_view_distance_culling_v10.py', patch_verifier_v10),
        ('scripts/verify_graphics_v11.py', patch_verifier_v11),
    ]:
        p = root/rel
        if p.is_file():
            text, nl, bom = read_text(p)
            write_text(p, func(text), nl, bom)

    print("[OK] Removed Preserve authored cutscene FOV and its hidden effect.")
    print("[OK] Draw Distance is normalized to exact integer steps 1x through 6x.")
    print("[OK] Removed ineffective/redundant Image controls and Z-fighting reduction from UI/persistence.")
    print("[OK] Retired Image controls are pinned to stable renderer defaults internally.")
    print("[OK] Replaced shared-plane frustum rewriting with object-local FOV/aspect culling relaxation.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    args = ap.parse_args()
    patch(Path(args.root).resolve())


if __name__ == '__main__':
    main()
