#!/usr/bin/env python3
from pathlib import Path
import argparse
import re

FRUSTUM_REPLACEMENT = 'extern "C" void rocket_graphics_frustum_begin(std::uint8_t* rdram,\n                                                 recomp_context* context) {\n    if (rdram == nullptr || context == nullptr) return;\n\n    // func_8003ACD4 has already consumed the temporary camera values into the\n    // graphics task by the time object frustum tests begin. Restore the guest\n    // camera immediately so presentation-only FOV/far/shake changes cannot leak\n    // into later simulation/camera logic during the same authored frame.\n    RestoreOwnedCamera(rdram);\n    const Settings s = rocket::graphics::settings();\n\n    // IMPORTANT ABI DETAIL:\n    // Rocket\'s Vec3f is typedef float Vec3f[3]. In a C function parameter,\n    // `Vec3f position` therefore decays to `float* position`; it is NOT a\n    // 12-byte by-value aggregate. The real o32 argument layout for\n    //\n    //   frustum_test(camera, position, cullRadius, renderDistance, arg4, alphaOut)\n    //\n    // is:\n    //   a0/r4 = camera\n    //   a1/r5 = position pointer\n    //   a2/r6 = cullRadius float bits\n    //   a3/r7 = renderDistance float bits\n    //   sp+0x10 = arg4 pointer\n    //   sp+0x14 = alphaOut pointer\n    //\n    // v9 incorrectly treated sp+0x14 as renderDistance, so the setting never\n    // reached Rocket\'s actual distance-cull/fade calculation.\n    const std::uint32_t bits = static_cast<std::uint32_t>(context->r7);\n    const float authored_distance = std::bit_cast<float>(bits);\n    if (!std::isfinite(authored_distance) || authored_distance <= 0.0F) return;\n\n    constexpr float kInfiniteRenderDistance = 3.402823466e+38F;\n    if (authored_distance >= 3.0e+38F) {\n        // Rocket already uses FLT_MAX as its explicit no-distance-cull sentinel.\n        // Preserve that exact behaviour.\n        context->r7 = static_cast<gpr>(\n            std::bit_cast<std::uint32_t>(kInfiniteRenderDistance));\n        return;\n    }\n\n    float adjusted = s.maximum_detail\n        ? kInfiniteRenderDistance\n        : authored_distance * s.draw_distance_multiplier;\n\n    if (!s.maximum_detail) {\n        if (!std::isfinite(adjusted) || adjusted <= 0.0F) return;\n        // Keep ordinary scaled distances finite while allowing a very large\n        // range. Maximum visibility deliberately uses FLT_MAX instead.\n        adjusted = std::min(adjusted, 1.0e+20F);\n    }\n\n    context->r7 = static_cast<gpr>(\n        std::bit_cast<std::uint32_t>(adjusted));\n}'
ASPECT_REPLACEMENT = 'void rocket::widescreen::update_window_aspect(SDL_Window* window) {\n    if (window != nullptr) {\n        int width = 0;\n        int height = 0;\n        SDL_GetWindowSize(window, &width, &height);\n        if (width > 0 && height > 0) {\n            const float aspect = static_cast<float>(width) / static_cast<float>(height);\n            if (std::isfinite(aspect) && aspect > 0.0F) {\n                g_window_aspect.store(aspect, std::memory_order_release);\n                rocket::graphics::set_window_aspect(aspect);\n            }\n        }\n    }\n\n    // The guard now protects both aspect expansion and positive gameplay FOV\n    // expansion. It must therefore remain enabled even at original 4:3 when a\n    // wider vertical FOV exposes geometry above/below the authored view.\n    g_expand_enabled.store(\n        rocket::graphics::settings().widescreen_culling,\n        std::memory_order_release);\n}'
CULLING_REPLACEMENT = 'extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,\n                                                   recomp_context* context) {\n    if (rdram == nullptr || context == nullptr) return;\n\n    // A prior call leaves the camera widened only while no guest code has\n    // replaced those exact plane values. Restore that owned state before this\n    // call so expansion is idempotent and a live switch back is clean.\n    RestorePreviousFrustumIfOwned(rdram);\n\n    if (!g_expand_enabled.load(std::memory_order_acquire)) return;\n\n    const std::uint32_t camera_address = static_cast<std::uint32_t>(context->r4);\n    if (camera_address < kRdramStart || camera_address > kRdramEnd - kCameraBytes) {\n        return;\n    }\n    const gpr camera = GuestAddress(camera_address);\n\n    const float authored_aspect = ReadFloat(rdram, camera, kAspectOffset);\n    const float authored_fov_y = ReadFloat(rdram, camera, kFovYRadiansOffset);\n    const float effective_fov_y =\n        rocket::graphics::effective_fov_radians(authored_fov_y);\n\n    if (!std::isfinite(authored_aspect) || authored_aspect <= 0.1F ||\n        !std::isfinite(authored_fov_y) || authored_fov_y <= 0.01F ||\n        authored_fov_y >= kPi - 0.01F ||\n        !std::isfinite(effective_fov_y) || effective_fov_y <= 0.01F ||\n        effective_fov_y >= kPi - 0.01F) {\n        return;\n    }\n\n    const float selected_aspect =\n        rocket::graphics::selected_aspect(authored_aspect);\n    if (!std::isfinite(selected_aspect) || selected_aspect <= 0.1F) return;\n\n    // Never make Rocket\'s authored culling tighter. Positive FOV offsets and\n    // wider output aspects may expand the visible volume; negative offsets or\n    // narrower targets keep the authored planes.\n    const float target_aspect = std::max(authored_aspect, selected_aspect);\n    const float target_fov_y = std::max(authored_fov_y, effective_fov_y);\n    const bool aspect_expanded =\n        target_aspect > authored_aspect * 1.0001F;\n    const bool fov_expanded =\n        target_fov_y > authored_fov_y + 0.0001F;\n    if (!aspect_expanded && !fov_expanded) return;\n\n    Vec3 forward{};\n    if (!Normalize(ReadVec3(rdram, camera, kForwardOffset), forward)) return;\n\n    std::array<Vec3, 4> planes{};\n    std::array<Vec3, 4> unit_planes{};\n    std::array<float, 4> plane_lengths{};\n    std::array<float, 4> forward_scores{};\n    std::array<std::size_t, 4> order{0U, 1U, 2U, 3U};\n\n    for (std::size_t index = 0; index < planes.size(); ++index) {\n        planes[index] = ReadVec3(\n            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);\n        if (!Normalize(planes[index], unit_planes[index], &plane_lengths[index])) {\n            return;\n        }\n\n        // For Rocket\'s perspective frustum, the left/right pair has the larger\n        // absolute forward component because the authored aspect is wider than\n        // 1:1. Sorting lets us identify horizontal vs vertical planes without\n        // depending on their memory order.\n        forward_scores[index] =\n            std::fabs(Dot(unit_planes[index], forward));\n    }\n\n    std::sort(order.begin(), order.end(),\n              [&](std::size_t left, std::size_t right) {\n                  return forward_scores[left] > forward_scores[right];\n              });\n\n    g_frustum_scope.saved = planes;\n    g_frustum_scope.widened = planes;\n    g_frustum_scope.camera_address = camera_address;\n\n    // The visible vertical FOV is Rocket-R\'s effective FOV. Apply the edge\n    // guard in tangent space so the safety margin scales naturally as FOV\n    // increases instead of being a fixed angular fudge.\n    const float target_vertical_half = target_fov_y * 0.5F;\n    float target_vertical_tangent = std::tan(target_vertical_half);\n    if (!std::isfinite(target_vertical_tangent) ||\n        target_vertical_tangent <= 0.0F) {\n        return;\n    }\n    target_vertical_tangent *= kEdgeGuard;\n\n    const float desired_vertical_half_fov =\n        std::atan(target_vertical_tangent);\n    const float desired_horizontal_half_fov =\n        std::atan(target_vertical_tangent * target_aspect);\n\n    // order[0..1] = left/right, order[2..3] = top/bottom. Expand all four.\n    // For every plane, compare against its current authored angle and only\n    // widen; this fail-safe prevents a malformed setting from narrowing retail\n    // visibility or clipping cutscene-authored geometry.\n    for (std::size_t pair_index = 0; pair_index < order.size(); ++pair_index) {\n        const std::size_t index = order[pair_index];\n        const Vec3 current = unit_planes[index];\n        const float forward_component = Dot(current, forward);\n        const float current_forward_abs =\n            std::clamp(std::fabs(forward_component), 0.0F, 1.0F);\n        const float current_half_fov = std::asin(current_forward_abs);\n        const float requested_half_fov = pair_index < 2U\n            ? desired_horizontal_half_fov\n            : desired_vertical_half_fov;\n        const float widened_half_fov =\n            std::max(current_half_fov, requested_half_fov);\n\n        // If this plane already encloses the requested guarded FOV, keep it\n        // bit-for-bit. That avoids needless guest-memory churn.\n        if (widened_half_fov <= current_half_fov + 0.00001F) continue;\n\n        const Vec3 transverse_raw =\n            Add(current, Scale(forward, -forward_component));\n        Vec3 transverse{};\n        if (!Normalize(transverse_raw, transverse)) continue;\n\n        const float desired_forward = std::sin(widened_half_fov);\n        const float desired_transverse = std::cos(widened_half_fov);\n        const float signed_forward =\n            std::copysign(desired_forward, forward_component);\n\n        const Vec3 widened_unit =\n            Add(Scale(transverse, desired_transverse),\n                Scale(forward, signed_forward));\n        const Vec3 widened = Scale(widened_unit, plane_lengths[index]);\n\n        WriteVec3(rdram, camera,\n                  kSidePlaneOffset + static_cast<int>(index) * 12,\n                  widened);\n        g_frustum_scope.widened[index] = widened;\n    }\n\n    g_frustum_scope.active = true;\n\n    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {\n        std::fprintf(\n            stderr,\n            "[culling] CPU frustum guard now covers left/right/top/bottom "\n            "(aspect %.3f->%.3f, vertical FOV %.1f->%.1f deg, +5%% guard)\\n",\n            authored_aspect, target_aspect,\n            authored_fov_y * (180.0F / kPi),\n            target_fov_y * (180.0F / kPi));\n    }\n}'


def read_text(path: Path):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    newline = "\r\n" if "\r\n" in text else "\n"
    return text, newline, bom


def write_text(path: Path, text: str, newline: str, bom: bool):
    text = text.replace("\r\n", "\n")
    if newline == "\r\n":
        text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def function_span(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"Required function marker not found: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace not found for: {marker}")

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
            if c == '"':
                state = "string"; i += 1; continue
            if c == "'":
                state = "char"; i += 1; continue
            if c == "{":
                depth += 1
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


def replace_disabled_after(text: str, anchor: str, replacement_line: str):
    anchor_pos = text.find(anchor)
    if anchor_pos < 0:
        raise RuntimeError(f"Graphics UI anchor not found: {anchor}")
    line_end = text.find("\n", anchor_pos)
    if line_end < 0:
        raise RuntimeError(f"Graphics UI anchor has no following line: {anchor}")
    next_pos = text.find("ImGui::TextDisabled(", line_end)
    if next_pos < 0 or next_pos - line_end > 700:
        raise RuntimeError(f"Graphics UI explanatory text not found after: {anchor}")
    start = text.rfind("\n", 0, next_pos) + 1
    end = text.find("\n", next_pos)
    if end < 0: end = len(text)
    indent = text[start:next_pos]
    return text[:start] + indent + replacement_line + text[end:]


def patch(root: Path):
    graphics = root / "src" / "graphics_enhancements.cpp"
    culling = root / "src" / "widescreen_culling.cpp"
    ui = root / "src" / "runtime_ui.cpp"
    for path in (graphics, culling, ui):
        if not path.is_file():
            raise RuntimeError(f"Required source file not found: {path}")

    gt, gnl, gbom = read_text(graphics)
    wt, wnl, wbom = read_text(culling)
    ut, unl, ubom = read_text(ui)

    gt = replace_function(
        gt,
        'extern "C" void rocket_graphics_frustum_begin',
        FRUSTUM_REPLACEMENT)
    wt = replace_function(
        wt,
        'void rocket::widescreen::update_window_aspect',
        ASPECT_REPLACEMENT)
    wt = replace_function(
        wt,
        'extern "C" void rocket_widescreen_frustum_begin',
        CULLING_REPLACEMENT)

    ut = replace_disabled_after(
        ut,
        'Maximum visibility (disable distance culling)',
        'ImGui::TextDisabled("Scales Rocket\'s real object render-distance argument before its original cull/fade calculation. Maximum visibility feeds Rocket\'s own FLT_MAX no-distance-cull sentinel.");')
    ut = replace_disabled_after(
        ut,
        'Widescreen culling fix',
        'ImGui::TextDisabled("The culling guard tracks aspect ratio and effective FOV, widening left/right/top/bottom automatically with a 5%% safety margin.");')

    write_text(graphics, gt, gnl, gbom)
    write_text(culling, wt, wnl, wbom)
    write_text(ui, ut, unl, ubom)

    # Immediate structural assertions.
    gt2 = graphics.read_text(encoding="utf-8-sig")
    wt2 = culling.read_text(encoding="utf-8-sig")
    if "context->r7" not in gt2:
        raise RuntimeError("Correct renderDistance register hook was not installed")
    fs, fe = function_span(gt2, 'extern "C" void rocket_graphics_frustum_begin')
    frustum_body = gt2[fs:fe]
    if "MEM_W(0x14" in frustum_body or "MEM_W(0x14," in frustum_body:
        raise RuntimeError("Old incorrect stack-based renderDistance access remains")
    if "pair_index < order.size()" not in wt2:
        raise RuntimeError("Four-plane culling loop was not installed")
    if "desired_vertical_half_fov" not in wt2 or "desired_horizontal_half_fov" not in wt2:
        raise RuntimeError("FOV-aware horizontal/vertical guard is incomplete")

    print("[OK] Draw distance now modifies frustum_test a3/r7 (actual renderDistance).")
    print("[OK] Maximum visibility now feeds Rocket's FLT_MAX distance-cull sentinel.")
    print("[OK] FOV/aspect culling guard now expands left/right/top/bottom planes.")
    print("[OK] Graphics UI descriptions updated to match the repaired behavior.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    patch(Path(args.root).resolve())


if __name__ == "__main__":
    main()
