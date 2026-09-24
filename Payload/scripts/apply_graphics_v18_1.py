#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil

DRAW_DISTANCE_REPLACEMENT = r'''extern "C" void rocket_graphics_frustum_begin(std::uint8_t* rdram,
                                                 recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // func_8003ACD4 has already consumed the temporary presentation camera.
    // Restore the guest camera before object tests so graphics-only changes do
    // not leak back into authored simulation/camera state.
    RestoreOwnedCamera(rdram);
    const Settings s = rocket::graphics::settings();

    // Rocket's Vec3f parameter decays to a pointer. frustum_test therefore uses:
    //   r4 = camera pointer
    //   r5 = position pointer
    //   r6 = cullRadius float bits
    //   r7 = renderDistance float bits
    //   sp+0x10 = arg4 pointer
    //   sp+0x14 = alphaOut pointer
    // Keep Draw Distance on the real r7 argument. Do not read stack + 0x14.
    const std::uint32_t bits = static_cast<std::uint32_t>(context->r7);
    const float authored_distance = std::bit_cast<float>(bits);
    if (!std::isfinite(authored_distance) || authored_distance <= 0.0F) return;

    constexpr float kInfiniteRenderDistance = 3.402823466e+38F;
    if (authored_distance >= 3.0e+38F) {
        // Retail Rocket already uses FLT_MAX to mean no distance cull.
        context->r7 = static_cast<gpr>(
            std::bit_cast<std::uint32_t>(kInfiniteRenderDistance));
        return;
    }

    // v12's UI cleanup removed the old Maximum Detail toggle. v17 accidentally
    // referenced that retired Settings member, which made current Rocket-R fail
    // to compile. The supported control is Draw Distance, so use only it here.
    const float multiplier = s.draw_distance_multiplier;
    if (!std::isfinite(multiplier) || multiplier <= 0.0F) return;

    float adjusted = authored_distance * multiplier;
    if (!std::isfinite(adjusted) || adjusted <= 0.0F) return;
    adjusted = std::min(adjusted, 1.0e+20F);

    context->r7 = static_cast<gpr>(
        std::bit_cast<std::uint32_t>(adjusted));
}'''

CULLING_REPLACEMENT = r'''extern "C" void rocket_widescreen_frustum_begin(std::uint8_t* rdram,
                                                   recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // Rocket's Vec3f parameter decays to float*, so the retail o32 ABI is:
    //   r4 = camera, r5 = position pointer, r6 = cullRadius, r7 = renderDistance.
    // v18 keeps that proven ABI and changes only the target-viewport test.
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
    if (target_aspect <= authored_aspect * 1.0001F &&
        target_fov_y <= authored_fov_y + 0.0001F) {
        return;
    }

    const Vec3 eye = ReadVec3(rdram, camera, 0);
    const Vec3 position = ReadVec3(rdram, position_ptr, 0);
    if (!Finite(eye) || !Finite(position)) return;
    const Vec3 camera_offset{
        position.x - eye.x,
        position.y - eye.y,
        position.z - eye.z,
    };
    const float distance = Length(camera_offset);
    if (!std::isfinite(distance)) return;

    // Read Rocket's REAL 4x4 view matrix. func_8003ACD4 sends camera+0x30
    // directly to the renderer, so testing in this space cannot become detached
    // from camera yaw/pitch/roll the way inferred plane-pair bases can.
    constexpr int kViewMatrixOffset = 0x30;
    std::array<float, 16> view_matrix{};
    for (std::size_t i = 0; i < view_matrix.size(); ++i) {
        view_matrix[i] = ReadFloat(
            rdram, camera, kViewMatrixOffset + static_cast<int>(i * 4U));
        if (!std::isfinite(view_matrix[i])) return;
    }

    // Rocket uses row-vector matrices: translation is the final row. Transform
    // the world-space object centre into exactly the same view space used by RSP.
    const Vec3 view_position{
        position.x * view_matrix[0] + position.y * view_matrix[4] +
            position.z * view_matrix[8] + view_matrix[12],
        position.x * view_matrix[1] + position.y * view_matrix[5] +
            position.z * view_matrix[9] + view_matrix[13],
        position.x * view_matrix[2] + position.y * view_matrix[6] +
            position.z * view_matrix[10] + view_matrix[14],
    };
    if (!Finite(view_position)) return;

    std::array<Vec3, 4> authored_planes{};
    std::array<Vec3, 4> unit_planes{};
    for (std::size_t index = 0; index < authored_planes.size(); ++index) {
        authored_planes[index] = ReadVec3(
            rdram, camera, kSidePlaneOffset + static_cast<int>(index) * 12);
        if (!Finite(authored_planes[index]) ||
            !Normalize(authored_planes[index], unit_planes[index])) {
            return;
        }
    }

    // The third view-matrix column is the world direction that changes view Z.
    // Determine its forward sign from Rocket's authored side planes rather than
    // assuming OpenGL/N64 handedness or a particular camera mode.
    Vec3 view_z_axis{view_matrix[2], view_matrix[6], view_matrix[10]};
    if (!Normalize(view_z_axis, view_z_axis)) return;
    float plus_score = 0.0F;
    float minus_score = 0.0F;
    for (const Vec3& plane : unit_planes) {
        plus_score += std::max(Dot(view_z_axis, plane), 0.0F);
        minus_score += std::max(Dot(Scale(view_z_axis, -1.0F), plane), 0.0F);
    }
    const float view_z_sign = plus_score <= minus_score ? 1.0F : -1.0F;
    const float depth = view_position.z * view_z_sign;
    if (!std::isfinite(depth)) return;

    // Use a true rectangular viewport test in renderer view space. The guard is
    // in tangent space, so left/right AND top/bottom expand with the real FOV.
    constexpr float kTargetFrustumGuard = 1.20F;
    float vertical_tangent = std::tan(target_fov_y * 0.5F);
    if (!std::isfinite(vertical_tangent) || vertical_tangent <= 0.0F) return;
    vertical_tangent *= kTargetFrustumGuard;
    const float horizontal_tangent = vertical_tangent * target_aspect;
    if (!std::isfinite(horizontal_tangent) || horizontal_tangent <= 0.0F) return;

    const float edge_slack = std::max(0.75F, distance * 0.006F);
    const float sphere_guard = authored_radius + edge_slack;

    // A sphere that intersects the near camera plane must never be side-culled;
    // for normal points require a positive forward depth and test both axes.
    bool target_visible = false;
    if (depth >= -sphere_guard) {
        const float projected_depth = std::max(depth, 0.0F);
        const float half_width = projected_depth * horizontal_tangent;
        const float half_height = projected_depth * vertical_tangent;
        target_visible =
            std::fabs(view_position.x) <= half_width + sphere_guard &&
            std::fabs(view_position.y) <= half_height + sphere_guard;
    }
    if (!target_visible) return;

    // Target-visible objects are allowed through Rocket's original four side
    // planes by relaxing only this call's sphere radius. Distance/fade remains
    // in r7 and is handled independently by the Draw Distance setting.
    float required_radius = authored_radius;
    for (const Vec3& authored_plane : authored_planes) {
        const float plane_distance = Dot(camera_offset, authored_plane);
        if (!std::isfinite(plane_distance)) return;
        required_radius = std::max(required_radius, plane_distance);
    }
    if (required_radius <= authored_radius) return;

    const float adjusted_radius = required_radius + edge_slack;
    if (!std::isfinite(adjusted_radius) || adjusted_radius <= 0.0F ||
        adjusted_radius >= 1.0e+20F) {
        return;
    }
    context->r6 = static_cast<gpr>(
        std::bit_cast<std::uint32_t>(adjusted_radius));

    if (!g_logged_expansion.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[culling] v18 renderer-view-matrix viewport guard active; "
            "position=r5 pointer, cullRadius=r6, renderDistance=r7 untouched "
            "(aspect %.3f->%.3f, vertical FOV %.1f->%.1f deg, 20%% guard)\n",
            authored_aspect, target_aspect,
            authored_fov_y * (180.0F / kPi),
            target_fov_y * (180.0F / kPi));
    }
}'''

QUEUE_STRUCT = r'''struct ExpandedRenderEntry {
    gpr r4 = 0;
    gpr r5 = 0;
    gpr r6 = 0;
    gpr r7 = 0;
    std::uint32_t stack10 = 0U;
    std::uint32_t stack14 = 0U;
};
'''

QUEUE_GLOBALS = r'''std::vector<ExpandedRenderEntry> g_expanded_render_entries;
std::size_t g_expanded_render_cursor = kGuestRenderQueueCapacity;
std::size_t g_expanded_render_batch_end = kGuestRenderQueueCapacity;
std::atomic<bool> g_render_queue_replay_active{false};
std::atomic<bool> g_render_queue_replay_loading{false};
std::atomic<bool> g_logged_render_queue_expansion{false};
'''

QUEUE_API = r'''extern "C" void rocket_render_queue_begin(std::uint8_t*, recomp_context*) {
    std::scoped_lock lock(g_mutex);
    g_expanded_render_entries.clear();
    if (g_expanded_render_entries.capacity() < 1024U) {
        g_expanded_render_entries.reserve(1024U);
    }
    g_expanded_render_cursor = kGuestRenderQueueCapacity;
    g_expanded_render_batch_end = kGuestRenderQueueCapacity;
    g_render_queue_replay_loading.store(false, std::memory_order_release);
    g_render_queue_replay_active.store(false, std::memory_order_release);
}

extern "C" void rocket_render_queue_capture(std::uint8_t* rdram,
                                                recomp_context* context) {
    if (rdram == nullptr || context == nullptr ||
        g_render_queue_replay_loading.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint8_t alpha = static_cast<std::uint8_t>(
        MEM_W(0x14, context->r29) & 0xFF);
    if (alpha == 0U) return;

    ExpandedRenderEntry entry{};
    entry.r4 = context->r4;
    entry.r5 = context->r5;
    entry.r6 = context->r6;
    entry.r7 = context->r7;
    entry.stack10 = static_cast<std::uint32_t>(MEM_W(0x10, context->r29));
    entry.stack14 = static_cast<std::uint32_t>(MEM_W(0x14, context->r29));

    std::scoped_lock lock(g_mutex);
    g_expanded_render_entries.push_back(entry);
}

extern "C" int rocket_render_queue_replay_active(void) {
    return g_render_queue_replay_active.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int rocket_render_queue_begin_batch(std::uint8_t* rdram,
                                                  recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return 0;
    std::scoped_lock lock(g_mutex);
    if (g_expanded_render_cursor >= g_expanded_render_entries.size()) {
        g_render_queue_replay_loading.store(false, std::memory_order_release);
        g_render_queue_replay_active.store(false, std::memory_order_release);
        return 0;
    }
    g_expanded_render_batch_end = std::min(
        g_expanded_render_cursor + kGuestRenderQueueCapacity,
        g_expanded_render_entries.size());
    MEM_W(0, RdramAddress(kGuestRenderQueueEndPointerAddress)) =
        static_cast<std::int32_t>(kGuestRenderQueueBase);
    g_render_queue_replay_active.store(true, std::memory_order_release);
    g_render_queue_replay_loading.store(true, std::memory_order_release);
    if (!g_logged_render_queue_expansion.exchange(true, std::memory_order_relaxed)) {
        const std::size_t total = g_expanded_render_entries.size();
        const std::size_t passes =
            (total + kGuestRenderQueueCapacity - 1U) / kGuestRenderQueueCapacity;
        std::fprintf(stderr,
            "[render-queue] EXPANDED: %zu visible submissions exceeded Rocket's "
            "retail %zu-entry list; replaying safely in %zu retail-sized passes.\n",
            total, kGuestRenderQueueCapacity, passes);
    }
    return 1;
}

extern "C" int rocket_render_queue_next(std::uint8_t* rdram,
                                           recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return 0;
    std::scoped_lock lock(g_mutex);
    if (!g_render_queue_replay_loading.load(std::memory_order_acquire) ||
        g_expanded_render_cursor >= g_expanded_render_batch_end) {
        return 0;
    }
    const ExpandedRenderEntry& entry =
        g_expanded_render_entries[g_expanded_render_cursor++];
    context->r4 = entry.r4;
    context->r5 = entry.r5;
    context->r6 = entry.r6;
    context->r7 = entry.r7;
    MEM_W(0x10, context->r29) = static_cast<std::int32_t>(entry.stack10);
    MEM_W(0x14, context->r29) = static_cast<std::int32_t>(entry.stack14);
    return 1;
}

extern "C" void rocket_render_queue_finish_batch(std::uint8_t*, recomp_context*) {
    g_render_queue_replay_loading.store(false, std::memory_order_release);
}

extern "C" void rocket_render_queue_replay_end(void) {
    g_render_queue_replay_loading.store(false, std::memory_order_release);
    g_render_queue_replay_active.store(false, std::memory_order_release);
}
'''


SELF_CHECK_BLOCK = r'''# v18.1: renderer-view-matrix culling + direct add_render_entry capture + safe multi-pass render queue + overlay fonts.
_v181_root = __import__('pathlib').Path(__file__).resolve().parents[1]
_v181_culling = (_v181_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
_v181_presentation = (_v181_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
_v181_ui = (_v181_root / 'src' / 'runtime_ui.cpp').read_text(encoding='utf-8-sig')
_v181_oneclick = (_v181_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
_v181_culling_required = all(token in _v181_culling for token in (
    'position pointer', 'position_address = static_cast<std::uint32_t>(context->r5)',
    'static_cast<std::uint32_t>(context->r6)', 'selected_aspect(authored_aspect)',
    'effective_fov_radians(authored_fov_y)', 'kViewMatrixOffset = 0x30',
    'view_z_axis', 'view_z_sign', 'kTargetFrustumGuard = 1.20F',
    'v18 renderer-view-matrix viewport guard active',
))
_v181_queue_required = all(token in _v181_presentation for token in (
    'ExpandedRenderEntry', 'rocket_render_queue_capture', 'rocket_render_queue_begin_batch',
    'rocket_render_queue_next', 'g_render_queue_replay_loading',
    'kGuestRenderQueueBase = 0x800ADB00U', '[render-queue] EXPANDED',
)) and 'patch_render_queue_generated.py' in _v181_oneclick
_v181_font_required = all(token in _v181_ui for token in (
    'ConfigureLauncherFonts();', 'v18.1: RT64 Inspector owns a fresh ImGui context',
    'g_launcher_context_active = false;',
))
_v181_forbidden = any(token in _v181_culling for token in (
    'requested_horizontal_half', 'target_unit_planes',
    'std::bit_cast<float>(static_cast<std::uint32_t>(context->r5))',
    'kNoSideCullRadiusBits = 0x7F7FFFFFU',
))
if not (_v181_culling_required and _v181_queue_required and _v181_font_required and not _v181_forbidden):
    raise SystemExit('SOURCE SELF-CHECK FAILED: FIXED34/v18.1 render visibility / queue / overlay-font policy missing')
'''

FRAME_BEGIN = r'''extern "C" void rocket_presentation_frame_begin(std::uint8_t*,
                                                  recomp_context*) {
    std::scoped_lock lock(g_mutex);
    ++g_frame;
    g_entries.clear();
    g_key_ordinals.clear();
    MaybeTraceSummary();
}'''


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
                if depth == 0: return start, i + 1
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


def patch_graphics(path: Path):
    text, nl, bom = read_text(path)
    start, end = function_span(text, 'extern "C" void rocket_graphics_frustum_begin')
    text = text[:start] + DRAW_DISTANCE_REPLACEMENT + text[end:]
    write_text(path, text, nl, bom)


def patch_culling(path: Path):
    text, nl, bom = read_text(path)
    start, end = function_span(text, 'extern "C" void rocket_widescreen_frustum_begin')
    text = text[:start] + CULLING_REPLACEMENT + text[end:]
    write_text(path, text, nl, bom)


def patch_presentation(path: Path):
    text, nl, bom = read_text(path)
    const_anchor = "constexpr std::size_t kGuestRenderQueueCapacity = 256U;"
    if const_anchor not in text:
        raise RuntimeError("v17 render queue capacity marker missing")
    if "kGuestRenderQueueBase = 0x800ADB00U" not in text:
        text = text.replace(const_anchor, const_anchor + "\nconstexpr std::uint32_t kGuestRenderQueueBase = 0x800ADB00U;\nconstexpr std::uint32_t kGuestRenderQueueEndPointerAddress = 0x800AF300U;", 1)
    if "struct ExpandedRenderEntry {" not in text:
        anchor = "struct RecordedEntry {"
        if anchor not in text: raise RuntimeError("RecordedEntry anchor missing")
        text = text.replace(anchor, QUEUE_STRUCT + "\n" + anchor, 1)
    if "g_expanded_render_entries" not in text:
        anchor = "std::vector<RecordedEntry> g_entries;"
        if anchor not in text: raise RuntimeError("g_entries anchor missing")
        text = text.replace(anchor, anchor + "\n" + QUEUE_GLOBALS.rstrip(), 1)
    text = text.replace("std::atomic<bool> g_logged_render_queue_pressure{false};\n", "", 1)
    text = text.replace("std::atomic<bool> g_logged_render_queue_saturation{false};\n", "", 1)
    old_capture = """    ExpandedRenderEntry expanded{};
    expanded.r4 = context->r4;
    expanded.r5 = context->r5;
    expanded.r6 = context->r6;
    expanded.r7 = context->r7;
    expanded.stack10 = static_cast<std::uint32_t>(MEM_W(0x10, context->r29));
    expanded.stack14 = static_cast<std::uint32_t>(MEM_W(0x14, context->r29));
"""
    text = text.replace(old_capture, "", 1)
    text = text.replace("    if (entry.alpha != 0U) {\n        g_expanded_render_entries.push_back(expanded);\n    }\n", "", 1)
    fstart, fend = function_span(text, 'extern "C" void rocket_presentation_frame_begin')
    text = text[:fstart] + FRAME_BEGIN + text[fend:]
    rstart, rend = function_span(text, 'extern "C" void rocket_presentation_render_entry')
    body = text[rstart:rend]
    null_anchor = "    if (rdram == nullptr || context == nullptr) return;\n"
    replay_guard = "    if (g_render_queue_replay_loading.load(std::memory_order_acquire)) return;\n"
    if replay_guard not in body:
        if null_anchor not in body: raise RuntimeError("render_entry null anchor missing")
        body = body.replace(null_anchor, null_anchor + replay_guard, 1)
    text = text[:rstart] + body + text[rend:]
    api_start = text.find('extern "C" void rocket_render_queue_begin(')
    if api_start >= 0:
        _, api_end = function_span(text, 'extern "C" void rocket_render_queue_replay_end')
        text = text[:api_start] + QUEUE_API + "\n" + text[api_end:]
    else:
        anchor='extern "C" void rocket_presentation_frame_begin'
        if anchor not in text: raise RuntimeError("frame begin API anchor missing")
        text=text.replace(anchor,QUEUE_API+"\n"+anchor,1)
    write_text(path,text,nl,bom)


def patch_runtime_ui(path: Path):
    text, nl, bom = read_text(path)
    marker = "v18.1: RT64 Inspector owns a fresh ImGui context"
    if marker not in text:
        anchor = '    application.presentQueue->inspector->setIniPath(g_config_directory / "rocket-r-ui.ini");\n'
        if anchor not in text: raise RuntimeError("runtime_ui.cpp Inspector setIniPath anchor missing")
        text=text.replace(anchor,anchor +
            "    // v18.1: RT64 Inspector owns a fresh ImGui context after the startup\n"
            "    // launcher context is destroyed. Load the same Comic Sans policy into\n"
            "    // this context so the in-game Launcher Overlay keeps its typography.\n"
            "    g_launcher_context_active = true;\n"
            "    ConfigureLauncherFonts();\n",1)
    dstart,dend=function_span(text,'void rocket::ui::detach(RT64::Application& application)')
    body=text[dstart:dend]
    cleanup=("    g_launcher_context_active = false;\n"
             "    g_launcher_body_font = nullptr;\n"
             "    g_launcher_heading_font = nullptr;\n")
    if cleanup not in body:
        close=body.rfind("\n}")
        if close < 0: raise RuntimeError("runtime_ui.cpp detach end missing")
        body=body[:close]+"\n"+cleanup+body[close:]
    text=text[:dstart]+body+text[dend:]
    write_text(path,text,nl,bom)

def patch_oneclick(path: Path):
    text, nl, bom = read_text(path)
    token = "scripts\\patch_render_queue_generated.py"
    if token in text:
        return
    anchor = '''    if ($recompExit -ne 0) {
        throw "N64Recomp CPU generation failed (exit $recompExit). See $RecompLog for the exact function/instruction."
    }
'''
    if anchor not in text:
        raise RuntimeError("FIXED34 N64Recomp generation anchor not found")
    insertion = anchor + '''
    # Graphics v18.1: N64Recomp regenerates these files every build, so apply the
    # safe multi-pass render-queue wrapper immediately after CPU generation.
    Invoke-Python @((Join-Path $Root 'scripts\\patch_render_queue_generated.py'),'--root',$Root)
'''
    text = text.replace(anchor, insertion, 1)
    write_text(path, text, nl, bom)


def patch_self_check(path: Path):
    text, nl, bom = read_text(path)
    if '# v18.1: renderer-view-matrix culling + direct add_render_entry capture + safe multi-pass render queue + overlay fonts.' in text:
        return
    start = text.find('# v16: correct pointer ABI + viewport-locked four-plane FOV/aspect guard.')
    if start < 0:
        raise RuntimeError('Expected v16 self-check block not found')
    end = text.find('\nprint(', start)
    if end < 0:
        # The v16 block is expected to be the last structural assertion block.
        end = len(text)
    text = text[:start] + SELF_CHECK_BLOCK.rstrip() + '\n' + text[end:]
    write_text(path, text, nl, bom)


def verify(root: Path):
    p = (root / "src" / "presentation_identity.cpp").read_text(encoding="utf-8-sig")
    w = (root / "src" / "widescreen_culling.cpp").read_text(encoding="utf-8-sig")
    o = (root / "scripts" / "OneClickBuild.ps1").read_text(encoding="utf-8-sig")
    sc = (root / "scripts" / "self_check.py").read_text(encoding="utf-8-sig")
    g = (root / "src" / "graphics_enhancements.cpp").read_text(encoding="utf-8-sig")
    u = (root / "src" / "runtime_ui.cpp").read_text(encoding="utf-8-sig")
    for token in (
        "ExpandedRenderEntry", "rocket_render_queue_capture", "rocket_render_queue_begin_batch",
        "rocket_render_queue_next", "g_render_queue_replay_loading",
        "kGuestRenderQueueBase = 0x800ADB00U",
        "[render-queue] EXPANDED",
    ):
        if token not in p: raise RuntimeError("presentation v18 token missing: " + token)
    for token in (
        "kViewMatrixOffset = 0x30", "view_z_axis", "view_z_sign",
        "v18 renderer-view-matrix viewport guard active",
    ):
        if token not in w: raise RuntimeError("culling v18 token missing: " + token)
    if "rocket's guest render list physically holds only" in p:
        raise RuntimeError("retired v17 saturation-only diagnostic remains")
    if "v18.1: RT64 Inspector owns a fresh ImGui context" not in u or "ConfigureLauncherFonts();" not in u:
        raise RuntimeError("in-game Comic Sans Inspector-context fix missing")
    if "scripts\\patch_render_queue_generated.py" not in o:
        raise RuntimeError("OneClickBuild persistent generated-code patch hook missing")
    if '# v18.1: renderer-view-matrix culling + direct add_render_entry capture + safe multi-pass render queue + overlay fonts.' not in sc:
        raise RuntimeError("v18.1 source self-check block missing")
    gs, ge = function_span(g, 'extern "C" void rocket_graphics_frustum_begin')
    gf = g[gs:ge]
    if "static_cast<std::uint32_t>(context->r7)" not in gf or "maximum_detail" in gf:
        raise RuntimeError("v17.1 Draw Distance/r7 baseline is not present")


def patch(root: Path, payload: Path | None = None):
    required = [
        root / "src" / "presentation_identity.cpp",
        root / "src" / "runtime_ui.cpp",
        root / "src" / "widescreen_culling.cpp",
        root / "src" / "graphics_enhancements.cpp",
        root / "scripts" / "OneClickBuild.ps1",
        root / "scripts" / "self_check.py",
    ]
    for path in required:
        if not path.is_file(): raise RuntimeError(f"Required file missing: {path}")

    # Fail closed on expected v16/v17.1 stack.
    if "v16 viewport-locked FOV/aspect guard active" not in required[2].read_text(encoding="utf-8-sig") and \
       "v18 renderer-view-matrix viewport guard active" not in required[2].read_text(encoding="utf-8-sig"):
        raise RuntimeError("Expected v16 culling baseline not found")
    gt = required[3].read_text(encoding="utf-8-sig")
    function_span(gt, 'extern "C" void rocket_graphics_frustum_begin')

    patch_graphics(required[3])
    patch_culling(required[2])
    patch_presentation(required[0])
    patch_runtime_ui(required[1])
    patch_oneclick(required[4])
    patch_self_check(required[5])

    if payload is not None:
        src = payload / "patch_render_queue_generated.py"
        if not src.is_file(): raise RuntimeError("payload generated-code patcher missing")
        shutil.copy2(src, root / "scripts" / "patch_render_queue_generated.py")

    verify(root)
    print("[OK] Kept Draw Distance on the correct r7 ABI and removed retired Maximum Detail dependency.")
    print("[OK] Replaced inferred plane-pair viewport test with Rocket's real camera+0x30 view matrix.")
    print("[OK] Kept correct frustum_test ABI: r5 position pointer, r6 radius, r7 distance.")
    print("[OK] Added direct generated add_render_entry capture for every non-zero-alpha render submission.")
    print("[OK] Installed persistent safe 256-entry multi-pass queue replay after every N64Recomp generation.")
    print("[OK] No guest BSS array was enlarged; every replay batch remains within the retail 256-entry buffer.")


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--payload")
    args=ap.parse_args()
    patch(Path(args.root).resolve(), Path(args.payload).resolve() if args.payload else None)

if __name__ == "__main__": main()
