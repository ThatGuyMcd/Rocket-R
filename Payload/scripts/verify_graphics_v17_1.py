#!/usr/bin/env python3
from pathlib import Path
import argparse


def fail(msg):
    print("[ERROR] " + msg)
    raise SystemExit(1)


def function_body(text, marker):
    start = text.find(marker)
    if start < 0: fail("Missing function: " + marker)
    brace = text.find("{", start)
    depth = 0
    i = brace
    while i < len(text):
        if text[i] == "{": depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0: return text[start:i+1]
        i += 1
    fail("Unterminated function: " + marker)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = Path(ap.parse_args().root).resolve()
    g = root / "src" / "graphics_enhancements.cpp"
    c = root / "src" / "widescreen_culling.cpp"
    p = root / "src" / "presentation_identity.cpp"
    for f in (g, c, p):
        if not f.is_file(): fail("Missing source: " + str(f))

    body = function_body(g.read_text(encoding="utf-8-sig"), 'extern "C" void rocket_graphics_frustum_begin')
    for token in ("context->r7", "draw_distance_multiplier", "kInfiniteRenderDistance"):
        if token not in body: fail("frustum hook missing: " + token)
    if "maximum_detail" in body: fail("retired maximum_detail is still referenced by frustum hook")
    if "MEM_W(0x14" in body: fail("renderDistance incorrectly reads stack + 0x14")

    if "v16 viewport-locked FOV/aspect guard active" not in c.read_text(encoding="utf-8-sig"):
        fail("v16 viewport/FOV culling baseline missing")
    pt = p.read_text(encoding="utf-8-sig")
    if "[render-queue] SATURATION" not in pt or "kGuestRenderQueueCapacity = 256U" not in pt:
        fail("v17 render-queue diagnostics missing")

    print("[OK] Rocket-R Graphics v17.1 compile hotfix verification PASS.")


if __name__ == "__main__":
    main()
