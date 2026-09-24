#!/usr/bin/env python3
from pathlib import Path
import argparse


def fail(msg):
    print("[ERROR] " + msg)
    raise SystemExit(1)


def function_body(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        fail("Missing function: " + marker)
    brace = text.find("{", start)
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
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
        elif state == "line":
            if c == "\n":
                state = "code"
        elif state == "block":
            if c == "*" and n == "/":
                state = "code"; i += 2; continue
        elif state == "string":
            if c == "\\":
                i += 2; continue
            if c == '"':
                state = "code"
        elif state == "char":
            if c == "\\":
                i += 2; continue
            if c == "'":
                state = "code"
        i += 1
    fail("Unterminated function: " + marker)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = Path(ap.parse_args().root).resolve()

    culling_path = root / "src" / "widescreen_culling.cpp"
    if not culling_path.is_file():
        fail("src/widescreen_culling.cpp is missing")

    text = culling_path.read_text(encoding="utf-8-sig")
    body = function_body(text, 'extern "C" void rocket_widescreen_frustum_begin')

    required = [
        "std::bit_cast<float>(static_cast<std::uint32_t>(context->r5))",
        "std::bit_cast<float>(static_cast<std::uint32_t>(context->r6))",
        "std::bit_cast<float>(static_cast<std::uint32_t>(context->r7))",
        "MEM_W(0x10, sp)",
        "ReadVec3(rdram, camera, kForwardOffset)",
        "rocket::graphics::widescreen_active(authored_aspect)",
        "kPairings",
        "target_planes",
        "requested_horizontal_half",
        "requested_vertical_half",
        "kTargetFrustumGuard = 1.15F",
        "required_radius = std::max(required_radius, plane_distance)",
        "v15 O32 camera-locked target frustum active",
    ]
    for token in required:
        if token not in body:
            fail("v15 ABI/camera-frustum token missing: " + token)

    forbidden = [
        "position_address = static_cast<std::uint32_t>(context->r5)",
        "context->r6 = static_cast<gpr>",
        "kNoSideCullRadiusBits = 0x7F7FFFFFU",
        "target_diagonal_half",
        "sphere_angle",
        "WriteVec3(rdram, camera",
    ]
    for token in forbidden:
        if token in body:
            fail("Broken/retired culling token remains: " + token)

    if "MEM_W(0x14, sp) =" in body:
        fail("Widescreen culling hook must not modify renderDistance at sp+0x14")

    self_check = (root / "scripts" / "self_check.py").read_text(
        encoding="utf-8-sig")
    if "FIXED34/v15 must use the real MIPS o32 frustum_test ABI" not in self_check:
        fail("self_check.py was not migrated to v15 ABI semantics")

    print("[OK] Rocket-R Graphics v15 O32 ABI + camera-locked frustum verification PASS.")


if __name__ == "__main__":
    main()
