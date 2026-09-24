#!/usr/bin/env python3
from pathlib import Path
import argparse


def fail(msg):
    print("[ERROR] " + msg)
    raise SystemExit(1)


def function_body(text: str, marker: str):
    start = text.find(marker)
    if start < 0: fail("Missing function: " + marker)
    brace = text.find("{", start)
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
                if depth == 0: return text[start:i + 1]
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
    fail("Unterminated function: " + marker)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = Path(ap.parse_args().root).resolve()

    graphics = root / "src" / "graphics_enhancements.cpp"
    presentation = root / "src" / "presentation_identity.cpp"
    culling = root / "src" / "widescreen_culling.cpp"
    for p in (graphics, presentation, culling):
        if not p.is_file(): fail("Missing source file: " + str(p))

    gf = graphics.read_text(encoding="utf-8-sig")
    body = function_body(gf, 'extern "C" void rocket_graphics_frustum_begin')
    for token in (
        "static_cast<std::uint32_t>(context->r7)",
        "context->r7 = static_cast<gpr>",
        "kInfiniteRenderDistance",
    ):
        if token not in body: fail("renderDistance ABI token missing: " + token)
    if "MEM_W(0x14" in body: fail("old sp+0x14 renderDistance bug remains")

    pf = presentation.read_text(encoding="utf-8-sig")
    for token in (
        "kGuestRenderQueueCapacity = 256U",
        "std::count_if(g_entries.begin(), g_entries.end()",
        "entry.alpha != 0U",
        "[render-queue] PRESSURE",
        "[render-queue] SATURATION",
    ):
        if token not in pf: fail("render queue diagnostic missing: " + token)

    cf = culling.read_text(encoding="utf-8-sig")
    cb = function_body(cf, 'extern "C" void rocket_widescreen_frustum_begin')
    for token in (
        "v16 viewport-locked FOV/aspect guard active",
        "position_address = static_cast<std::uint32_t>(context->r5)",
        "context->r6 = static_cast<gpr>",
        "requested_horizontal_half",
        "requested_vertical_half",
    ):
        if token not in cb: fail("v16 culling baseline missing: " + token)

    print("[OK] Rocket-R Graphics v17 renderDistance ABI + render-queue diagnostic verification PASS.")


if __name__ == "__main__":
    main()
