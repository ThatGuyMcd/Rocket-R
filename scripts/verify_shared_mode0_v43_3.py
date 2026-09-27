#!/usr/bin/env python3
from pathlib import Path
import argparse
import json
import sys
from collections import Counter

FORBIDDEN = (
    "patch_render_capacity_v32_generated.py",
    "patch_interpolation_v35_generated.py",
    "patch_interpolation_v36_generated.py",
    "patch_tinker_token_v41_generated.py",
)

def need(ok, msg):
    if not ok:
        raise RuntimeError(msg)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    a = ap.parse_args()
    root = Path(a.root).resolve()

    policy = json.loads(
        (root / "runtime-recomp/rocket.us.recomp-policy.json").read_text(
            encoding="utf-8-sig"))
    source = (root / "src/presentation_identity.cpp").read_text(
        encoding="utf-8-sig")
    one = (root / "scripts/OneClickBuild.ps1").read_text(
        encoding="utf-8-sig")

    hooks = list(policy.get("functionHooks", []))
    keys = [
        (str(h.get("function", "")),
         str(h.get("beforeVram", "")).lower())
        for h in hooks
    ]
    counts = Counter(keys)
    duplicates = sorted(k for k, n in counts.items() if n > 1)
    need(not duplicates,
         "N64Recomp policy contains duplicate function+VRAM hooks: "
         + repr(duplicates))

    def hook(function, vram):
        hits = [
            h for h in hooks
            if str(h.get("function", "")) == function
            and str(h.get("beforeVram", "")).lower() == vram.lower()
        ]
        need(len(hits) == 1,
             f"Expected exactly one hook at {function}/{vram}, "
             f"found {len(hits)}")
        return hits[0]

    begin = hook("func_8001ECEC", "0x8001ECEC")
    begin_text = str(begin.get("text", ""))
    need("ROCKET-R POLICY V42 V36 MODEL BEGIN" in begin_text,
         "merged entry hook lost v36 model-range begin")
    need("ROCKET-R POLICY V43 MODE0 BEGIN" in begin_text,
         "merged entry hook lost v43 shared-mode0 begin")

    end = hook("func_8001ECEC", "0x8001F120")
    end_text = str(end.get("text", ""))
    need("ROCKET-R POLICY V42 V36 MODEL END" in end_text,
         "merged return hook lost v36 model-range end")
    need("ROCKET-R POLICY V43 MODE0 END" in end_text,
         "merged return hook lost v43 shared-mode0 end")

    matrix = hook("func_8001ECEC", "0x8001EFD0")
    need("ROCKET-R POLICY V42 TINKER MATRIX" in str(matrix.get("text", "")),
         "shared mode-0 matrix hook at 0x8001EFD0 is missing")

    for token in (
        "ROCKET-R INTERPOLATION V43 SHARED MODE0 DIRECT MATRICES",
        "rocket_presentation_shared_mode0_begin",
        "rocket_presentation_shared_mode0_end",
        "g_shared_mode0_v43.claimed_matrix",
        "Physical(object) != g_shared_mode0_v43.model",
        "SupportsRigidInterpolation(rdram, matrix)",
        "g_tinker_token_draw_owner",
    ):
        need(token in source, "v43 source marker missing: " + token)

    start = source.find(
        'extern "C" void rocket_presentation_tinker_token_matrix(')
    need(start >= 0, "shared mode-0 matrix function missing")
    region = source[start:start + 12000]
    need("const std::uint32_t index" not in region,
         "shared mode-0 identity still depends on per-Submodel index")
    need("claimed_matrix != 0U" in region,
         "duplicate shared-matrix suppression missing")
    need("g_specific_samples.push_back(sample);" in region,
         "shared mode-0 sample is not submitted")

    for name in FORBIDDEN:
        need(name not in one,
             "OneClick schedules forbidden generated-C patcher: " + name)

    need("ROCKET-R V42 GOLDEN RULE" in one,
         "v42 golden-rule OneClick migration missing")

    print("[OK] Rocket-R interpolation v43.3 policy verification PASS.")
    print("[OK] func_8001ECEC entry/return each have ONE merged v36+v43 hook.")
    print("[OK] No duplicate function+VRAM hook exists anywhere in policy.")
    print("[OK] RecompiledFuncs remains generated/read-only.")

if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print("v43.3 verification failed:", exc, file=sys.stderr)
        raise SystemExit(1)
