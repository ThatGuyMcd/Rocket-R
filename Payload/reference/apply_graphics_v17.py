#!/usr/bin/env python3
from pathlib import Path
import argparse

FRUSTUM_REPLACEMENT = r'''extern "C" void rocket_graphics_frustum_begin(std::uint8_t* rdram,
                                                 recomp_context* context) {
    if (rdram == nullptr || context == nullptr) return;

    // func_8003ACD4 has already consumed the temporary camera values into the
    // graphics task by the time object frustum tests begin. Restore the guest
    // camera immediately so presentation-only FOV/far/shake changes cannot leak
    // into later simulation/camera logic during the same authored frame.
    RestoreOwnedCamera(rdram);
    const Settings s = rocket::graphics::settings();

    // Rocket's Vec3f parameter decays to a pointer, so frustum_test uses the
    // normal four-register o32 layout:
    //   r4 = camera pointer
    //   r5 = position pointer
    //   r6 = cullRadius float bits
    //   r7 = renderDistance float bits
    //   sp+0x10 = arg4 pointer
    //   sp+0x14 = alphaOut pointer
    //
    // Several later graphics patch layers accidentally restored the old v9
    // stack-based read. Keep renderDistance in r7 so Draw Distance and Maximum
    // Detail feed Rocket's real distance cull/fade calculation.
    const std::uint32_t bits = static_cast<std::uint32_t>(context->r7);
    const float authored_distance = std::bit_cast<float>(bits);
    if (!std::isfinite(authored_distance) || authored_distance <= 0.0F) return;

    constexpr float kInfiniteRenderDistance = 3.402823466e+38F;
    if (authored_distance >= 3.0e+38F) {
        // Rocket already uses FLT_MAX as its explicit no-distance-cull sentinel.
        context->r7 = static_cast<gpr>(
            std::bit_cast<std::uint32_t>(kInfiniteRenderDistance));
        return;
    }

    float adjusted = s.maximum_detail
        ? kInfiniteRenderDistance
        : authored_distance * s.draw_distance_multiplier;

    if (!s.maximum_detail) {
        if (!std::isfinite(adjusted) || adjusted <= 0.0F) return;
        adjusted = std::min(adjusted, 1.0e+20F);
    }

    context->r7 = static_cast<gpr>(
        std::bit_cast<std::uint32_t>(adjusted));
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


def install_queue_diagnostic(text: str):
    if "[render-queue] SATURATION" in text:
        return text

    const_anchor = "constexpr std::size_t kMaximumPendingTasks = 8U;"
    if const_anchor not in text:
        raise RuntimeError("presentation_identity.cpp queue constant anchor not found")
    text = text.replace(
        const_anchor,
        const_anchor + "\nconstexpr std::size_t kGuestRenderQueueCapacity = 256U;",
        1)

    atomic_anchor = "std::atomic<std::uint64_t> g_trace_entries{0U};"
    if atomic_anchor not in text:
        raise RuntimeError("presentation_identity.cpp trace atomic anchor not found")
    text = text.replace(
        atomic_anchor,
        "std::atomic<bool> g_logged_render_queue_pressure{false};\n"
        "std::atomic<bool> g_logged_render_queue_saturation{false};\n" + atomic_anchor,
        1)

    frame_marker = 'extern "C" void rocket_presentation_frame_begin'
    start, end = function_span(text, frame_marker)
    body = text[start:end]
    lock_anchor = "    std::scoped_lock lock(g_mutex);\n"
    if lock_anchor not in body:
        raise RuntimeError("rocket_presentation_frame_begin lock anchor not found")

    diagnostic = r'''    const std::size_t visible_attempts = static_cast<std::size_t>(
        std::count_if(g_entries.begin(), g_entries.end(),
                      [](const RecordedEntry& entry) { return entry.alpha != 0U; }));
    if (visible_attempts >= 240U &&
        !g_logged_render_queue_pressure.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[render-queue] PRESSURE: previous authored frame attempted %zu/%zu visible entries. "
            "Dense widescreen views are close to Rocket's original render-list ceiling.\n",
            visible_attempts, kGuestRenderQueueCapacity);
    }
    if (visible_attempts > kGuestRenderQueueCapacity &&
        !g_logged_render_queue_saturation.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
            "[render-queue] SATURATION: previous authored frame attempted %zu visible entries, "
            "but Rocket's guest render list physically holds only %zu. At least %zu entries were "
            "silently rejected by add_render_entry; camera-direction popping can result.\n",
            visible_attempts, kGuestRenderQueueCapacity,
            visible_attempts - kGuestRenderQueueCapacity);
    }
'''
    body = body.replace(lock_anchor, lock_anchor + diagnostic, 1)
    return text[:start] + body + text[end:]


def patch(root: Path):
    graphics = root / "src" / "graphics_enhancements.cpp"
    presentation = root / "src" / "presentation_identity.cpp"
    culling = root / "src" / "widescreen_culling.cpp"
    for path in (graphics, presentation, culling):
        if not path.is_file():
            raise RuntimeError(f"Required Rocket-R source file missing: {path}")

    gt, gnl, gbom = read_text(graphics)
    gs, ge = function_span(gt, 'extern "C" void rocket_graphics_frustum_begin')
    gt = gt[:gs] + FRUSTUM_REPLACEMENT + gt[ge:]
    write_text(graphics, gt, gnl, gbom)

    pt, pnl, pbom = read_text(presentation)
    pt = install_queue_diagnostic(pt)
    write_text(presentation, pt, pnl, pbom)

    # Fail closed if the v16 viewport-aware culling layer is not present. v17
    # deliberately does not rewrite that now-improved frustum implementation.
    ct = culling.read_text(encoding="utf-8-sig")
    if ("v16 viewport-locked FOV/aspect guard active" not in ct or
        "position_address = static_cast<std::uint32_t>(context->r5)" not in ct or
        "context->r6 = static_cast<gpr>" not in ct):
        raise RuntimeError("Graphics v16 viewport/FOV culling baseline was not found; refusing to stack v17 onto an unknown culling implementation")

    # Structural assertions.
    gf = graphics.read_text(encoding="utf-8-sig")
    s, e = function_span(gf, 'extern "C" void rocket_graphics_frustum_begin')
    frustum = gf[s:e]
    if "static_cast<std::uint32_t>(context->r7)" not in frustum or "context->r7 =" not in frustum:
        raise RuntimeError("r7 renderDistance repair was not installed")
    if "MEM_W(0x14" in frustum:
        raise RuntimeError("retired stack-based renderDistance bug remains")

    pf = presentation.read_text(encoding="utf-8-sig")
    for token in (
        "kGuestRenderQueueCapacity = 256U",
        "visible_attempts",
        "[render-queue] PRESSURE",
        "[render-queue] SATURATION",
        "visible_attempts - kGuestRenderQueueCapacity",
    ):
        if token not in pf:
            raise RuntimeError("render-queue diagnostic token missing: " + token)

    print("[OK] Restored frustum_test renderDistance to the real a3/r7 ABI.")
    print("[OK] sp+0x14 is again treated only as alphaOut, never renderDistance.")
    print("[OK] Kept Graphics v16 viewport/FOV side-culling logic unchanged.")
    print("[OK] Added non-invasive 240/256 render-queue pressure diagnostic.")
    print("[OK] Added one-shot >256 saturation diagnostic with dropped-entry count.")
    print("[INFO] v17 does NOT enlarge or overwrite Rocket's fixed guest render array.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    patch(Path(args.root).resolve())


if __name__ == "__main__":
    main()
