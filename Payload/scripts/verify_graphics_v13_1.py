#!/usr/bin/env python3
from pathlib import Path
import argparse


def fail(msg):
    print("[ERROR] " + msg)
    raise SystemExit(1)


def function_body(text: str, marker: str):
    start = text.find(marker)
    if start < 0:
        fail(f"Missing function: {marker}")
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
    fail(f"Unterminated function: {marker}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    root = Path(ap.parse_args().root).resolve()

    culling_path = root / "src" / "widescreen_culling.cpp"
    if not culling_path.is_file():
        fail("src/widescreen_culling.cpp is missing")

    text = culling_path.read_text(encoding="utf-8-sig")
    body = function_body(
        text, 'extern "C" void rocket_widescreen_frustum_begin')

    for token in [
        "position_address = static_cast<std::uint32_t>(context->r5)",
        "authored_radius = std::bit_cast<float>",
        "kPairings",
        "outward_sum",
        "target_planes",
        "best_pair_score",
        "requested_horizontal_half",
        "requested_vertical_half",
        "kTargetFrustumGuard = 1.15F",
        "plane_distance > authored_radius + edge_slack",
        "required_radius = std::max(required_radius, plane_distance)",
        "context->r6 = static_cast<gpr>",
    ]:
        if token not in body:
            fail(f"Exact v13.1 target-frustum culling token missing: {token}")

    for token in [
        "target_diagonal_half",
        "sphere_angle",
        "kAngularHysteresis",
        "kForwardOffset), forward",
        "WriteVec3(rdram, camera",
    ]:
        if token in body:
            fail(f"Retired direction-sensitive culling code remains: {token}")

    if "context->r7" in body:
        fail("Culling guard must not modify draw-distance/renderDistance r7")

    self_check = (root / "scripts" / "self_check.py").read_text(
        encoding="utf-8-sig")
    if "FIXED34/v13.1 must use exact object-local target-frustum culling" not in self_check:
        fail("self_check.py was not migrated to v13 culling semantics")

    print("[OK] Rocket-R Graphics v13.1 exact target-frustum culling verification PASS.")


if __name__ == "__main__":
    main()
