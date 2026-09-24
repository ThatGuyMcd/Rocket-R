#!/usr/bin/env python3
from pathlib import Path
import argparse

REPLACEMENT = r'''extern "C" void rocket_graphics_frustum_begin(std::uint8_t* rdram,
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


def patch(root: Path):
    graphics = root / "src" / "graphics_enhancements.cpp"
    culling = root / "src" / "widescreen_culling.cpp"
    presentation = root / "src" / "presentation_identity.cpp"
    if not graphics.is_file():
        raise RuntimeError(f"Missing Rocket-R source: {graphics}")
    if not culling.is_file():
        raise RuntimeError(f"Missing Rocket-R source: {culling}")
    if not presentation.is_file():
        raise RuntimeError(f"Missing Rocket-R source: {presentation}")

    text, nl, bom = read_text(graphics)
    start, end = function_span(text, 'extern "C" void rocket_graphics_frustum_begin')
    text = text[:start] + REPLACEMENT + text[end:]
    write_text(graphics, text, nl, bom)

    # Refuse to silently downgrade the useful v16/v17 work.
    ctext = culling.read_text(encoding="utf-8-sig")
    if "v16 viewport-locked FOV/aspect guard active" not in ctext:
        raise RuntimeError("Graphics v16 viewport/FOV culling baseline not found")
    ptext = presentation.read_text(encoding="utf-8-sig")
    if "[render-queue] SATURATION" not in ptext:
        raise RuntimeError("Graphics v17 render-queue diagnostic baseline not found")

    final = graphics.read_text(encoding="utf-8-sig")
    s, e = function_span(final, 'extern "C" void rocket_graphics_frustum_begin')
    body = final[s:e]
    for token in (
        "static_cast<std::uint32_t>(context->r7)",
        "s.draw_distance_multiplier",
        "context->r7 = static_cast<gpr>",
    ):
        if token not in body:
            raise RuntimeError("v17.1 token missing: " + token)
    if "maximum_detail" in body:
        raise RuntimeError("retired maximum_detail reference remains in frustum hook")
    if "MEM_W(0x14" in body:
        raise RuntimeError("retired stack renderDistance read remains")

    print("[OK] Removed retired Settings::maximum_detail dependency from the v17 frustum hook.")
    print("[OK] Draw Distance now uses Settings::draw_distance_multiplier only.")
    print("[OK] Kept renderDistance on the correct frustum_test r7 argument.")
    print("[OK] Kept v16 viewport/FOV culling and v17 render-queue diagnostics intact.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    patch(Path(args.root).resolve())


if __name__ == "__main__":
    main()
