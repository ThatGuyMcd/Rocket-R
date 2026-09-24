#!/usr/bin/env python3
from pathlib import Path
import argparse
import re
import sys


def fail(message):
    print(f"[ERROR] {message}")
    raise SystemExit(1)


def function_body(text, marker):
    start=text.find(marker)
    if start < 0: fail(f"Missing function: {marker}")
    brace=text.find("{",start)
    if brace < 0: fail(f"Missing opening brace: {marker}")
    depth=0
    for i in range(brace,len(text)):
        if text[i]=="{": depth+=1
        elif text[i]=="}":
            depth-=1
            if depth==0: return text[start:i+1]
    fail(f"Unterminated function: {marker}")


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--root",required=True)
    args=ap.parse_args()
    root=Path(args.root).resolve()
    graphics=(root/"src/graphics_enhancements.cpp").read_text(encoding="utf-8-sig")
    culling=(root/"src/widescreen_culling.cpp").read_text(encoding="utf-8-sig")
    ui=(root/"src/runtime_ui.cpp").read_text(encoding="utf-8-sig")

    frustum=function_body(graphics,'extern "C" void rocket_graphics_frustum_begin')
    required_frustum=[
        "context->r7",
        "authored_distance",
        "draw_distance_multiplier",
        "kInfiniteRenderDistance",
    ]
    for token in required_frustum:
        if token not in frustum: fail(f"Draw-distance hook missing token: {token}")
    if "MEM_W(0x14" in frustum:
        fail("Old v9 stack offset 0x14 renderDistance bug is still present")
    if "context->r29" in frustum:
        fail("Draw-distance hook still depends on the old mistaken stack ABI")

    aspect=function_body(culling,'void rocket::widescreen::update_window_aspect')
    guard=function_body(culling,'extern "C" void rocket_widescreen_frustum_begin')
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
            fail(f"v13 exact target-frustum culling guard missing token: {token}")
    if "WriteVec3(" in guard:
        fail("v13 culling must not rewrite shared camera frustum planes")

    for token in [
        "Draw distance",
        "Six exact steps: 1x, 2x, 3x, 4x, 5x or 6x.",
        "object-local",
    ]:
        if token not in ui: fail(f"Graphics UI missing v12 behavior marker: {token}")

    policy=root/"runtime-recomp/rocket.us.recomp-policy.json"
    if policy.is_file():
        pt=policy.read_text(encoding="utf-8-sig")
        gi=pt.find("rocket_graphics_frustum_begin")
        wi=pt.find("rocket_widescreen_frustum_begin")
        if gi < 0 or wi < 0: fail("Required frustum hooks missing from recomp policy")
        if gi > wi: fail("Hook order is wrong: graphics distance hook must precede culling guard")

    print("[OK] Rocket-R Graphics v10/v13 view-distance + exact object-local FOV culling verification PASS.")

if __name__=="__main__":
    main()
