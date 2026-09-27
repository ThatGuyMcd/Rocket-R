#!/usr/bin/env python3
from pathlib import Path
import argparse
import json
import sys

FORBIDDEN_ACTIVE_PATCHERS = (
    "patch_render_capacity_v32_generated.py",
    "patch_interpolation_v35_generated.py",
    "patch_interpolation_v36_generated.py",
    "patch_tinker_token_v41_generated.py",
)

EXPECTED_HOOKS = {
    ("func_8008B594", "0x8008b5c0"),
    ("add_render_entry", "0x8008b26c"),
    ("add_render_entry", "0x8008b29c"),
    ("func_8008B694", "0x8008b7f8"),
    ("func_8008B694", "0x8008b82c"),
    ("func_8008B694", "0x8008b830"),
    ("func_8008B694", "0x8008b834"),
    ("func_8008B694", "0x8008b850"),
    ("func_8008B694", "0x8008b92c"),
    ("func_8008B694", "0x8008b988"),
    ("func_8008B694", "0x8008b98c"),
    ("func_8008B694", "0x8008b9a4"),
    ("func_8008B694", "0x8008b9a8"),
    ("func_8001EA18", "0x8001ea18"),
    ("func_8001EA18", "0x8001ece4"),
    ("func_8001ECEC", "0x8001ecec"),
    ("func_8001ECEC", "0x8001f120"),
    ("load_translation_mtx", "0x800476cc"),
    ("load_translation_mtx", "0x800477a4"),
    ("func_8004A4F0", "0x8004a4f0"),
    ("func_8004A4F0", "0x8004ac34"),
    ("func_8003ACD4", "0x8003adf4"),
    ("func_8006BDF0", "0x8006be94"),
    ("func_8006BDF0", "0x8006be9c"),
    ("func_8001ECEC", "0x8001efd0"),
}

EXPECTED_NOPS = {
    ("func_8008B594", "0x8008b5c0"),
    ("add_render_entry", "0x8008b26c"),
    ("add_render_entry", "0x8008b29c"),
    ("func_8008B694", "0x8008b7f8"),
    ("func_8008B694", "0x8008b82c"),
    ("func_8008B694", "0x8008b830"),
    ("func_8008B694", "0x8008b834"),
    ("func_8008B694", "0x8008b850"),
    ("func_8008B694", "0x8008b92c"),
    ("func_8008B694", "0x8008b988"),
    ("func_8008B694", "0x8008b98c"),
    ("func_8008B694", "0x8008b9a4"),
    ("func_8008B694", "0x8008b9a8"),
}

GENERATED_MARKERS = (
    "ROCKET-R POLICY V42 V32 QUEUE BASE",
    "ROCKET-R POLICY V42 V32 ADD BASE",
    "ROCKET-R POLICY V42 V32 CAPACITY",
    "ROCKET-R POLICY V42 V35 SUBMODEL BEGIN",
    "ROCKET-R POLICY V42 V35 SUBMODEL END",
    "ROCKET-R POLICY V42 V36 MODEL BEGIN",
    "ROCKET-R POLICY V42 V36 MODEL END",
    "ROCKET-R POLICY V42 V36 TRANSLATION BEGIN",
    "ROCKET-R POLICY V42 V36 TRANSLATION END",
    "ROCKET-R POLICY V42 V36 EFFECT BEGIN",
    "ROCKET-R POLICY V42 V36 EFFECT END",
    "ROCKET-R POLICY V42 V36 CAMERA MATRICES",
    "ROCKET-R POLICY V42 TINKER BEGIN",
    "ROCKET-R POLICY V42 TINKER END",
    "ROCKET-R POLICY V42 TINKER MATRIX",
)

OLD_POSTPATCH_MARKERS = (
    "ROCKET-R GRAPHICS V32 SINGLE LIVE RENDER QUEUE",
    "ROCKET-R INTERPOLATION V35 SUBMODEL CAPTURE BEGIN",
    "ROCKET-R INTERPOLATION V36 MODEL RANGE BEGIN",
    "ROCKET-R INTERPOLATION V41 TINKER TOKEN DRAW SCOPE",
    "ROCKET-R INTERPOLATION V41 TINKER TOKEN MODE0 EXACT MATRIX",
)


def need(ok: bool, msg: str):
    if not ok:
        raise RuntimeError(msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--with-generated", action="store_true")
    a = ap.parse_args()
    root = Path(a.root).resolve()

    policy_path = root / "runtime-recomp" / "rocket.us.recomp-policy.json"
    one_path = root / "scripts" / "OneClickBuild.ps1"
    cpp_path = root / "src" / "presentation_identity.cpp"
    need(policy_path.is_file(), "recomp policy missing")
    need(one_path.is_file(), "OneClickBuild.ps1 missing")
    need(cpp_path.is_file(), "presentation_identity.cpp missing")

    policy = json.loads(policy_path.read_text(encoding="utf-8-sig"))
    one = one_path.read_text(encoding="utf-8-sig")
    cpp = cpp_path.read_text(encoding="utf-8-sig")

    hooks = {
        (str(x.get("function")), str(x.get("beforeVram", "")).lower())
        for x in policy.get("functionHooks", [])
    }
    nops = {
        (str(x.get("function")), str(x.get("vram", "")).lower())
        for x in policy.get("instructionPatches", [])
        if str(x.get("value", "")).lower() in ("0x00000000", "0x0", "0")
    }

    missing_hooks = sorted(EXPECTED_HOOKS - hooks)
    missing_nops = sorted(EXPECTED_NOPS - nops)
    need(not missing_hooks, "Missing v42 policy hooks: " + repr(missing_hooks))
    need(not missing_nops, "Missing v42 v32 instruction NOPs: " + repr(missing_nops))

    # Confirm the policy text contains the source-of-truth marker family.
    policy_text = policy_path.read_text(encoding="utf-8-sig")
    for marker in GENERATED_MARKERS:
        need(marker in policy_text, "Policy marker missing: " + marker)

    for token in (
        "ROCKET-R INTERPOLATION V42 TINKER TOKEN POLICY HOOKS",
        "ROCKET-R V42 RETIRED V37 CLASS-CALLBACK GUESS",
        "rocket_presentation_tinker_token_draw_begin",
        "rocket_presentation_tinker_token_draw_end",
        "rocket_presentation_tinker_token_matrix",
        "g_trace_v42_token_draws",
        "g_trace_v42_token_matrices",
        "[rocket-interpolation-v42]",
    ):
        need(token in cpp, "v42 presentation source missing: " + token)

    retired_pos = cpp.find("ROCKET-R V42 RETIRED V37 CLASS-CALLBACK GUESS")
    need(retired_pos >= 0 and "return false;" in cpp[retired_pos:retired_pos + 600],
         "Disproved v37 classifier is not fail-closed")

    # Broken v41 trace state must be completely gone.
    for token in ("g_trace_v41_draws", "g_trace_v41_matrices",
                  "[rocket-interpolation-v41]"):
        need(token not in cpp, "Broken v41 source residue remains: " + token)

    # OneClick must not schedule or even reference an active generated patcher.
    for name in FORBIDDEN_ACTIVE_PATCHERS:
        need(name not in one, "OneClick still schedules generated patcher: " + name)
        need(not (root / "scripts" / name).exists(),
             "Obsolete active generated patcher still exists: scripts/" + name)

    for token in (
        "ROCKET-R V42 GOLDEN RULE: RecompiledFuncs is disposable N64Recomp output.",
        "ROCKET-R V42 GOLDEN RULE: generated CPU C is read-only after N64Recomp.",
        "verify_recomp_policy_v42.py",
        "RecompiledFuncs policy verification: PASS (read-only; no post-generation patching).",
    ):
        need(token in one, "OneClick golden-rule wiring missing: " + token)

    # No generic generated patch script should be INVOKED after N64Recomp.
    post_start = one.find("Generating Rocket CPU recompilation...")
    post_end = one.find("Generating Rocket n_aspMain RSP recompilation...", post_start)
    need(post_start >= 0 and post_end > post_start,
         "Could not identify OneClick CPU post-generation section")
    post = one[post_start:post_end]
    need(not re_search_generated_invocation(post),
         "A generated-C patcher invocation remains after N64Recomp")

    # Existing semantic safety architecture remains.
    for token in (
        "ROCKET-R INTERPOLATION V35 SPECIFIC SUBMODEL MATRICES + RANGE-STABLE SKY",
        "ROCKET-R INTERPOLATION V36 GLOBAL MATRIX OWNERSHIP",
        "ROCKET-R INTERPOLATION V37 COLLECTIBLE RIGID ROTATION",
        "ROCKET-R INTERPOLATION V38 COLLECTIBLE BILLBOARD VERTEX COVERAGE",
        "ROCKET-R INTERPOLATION V39 COLLECTIBLE OWNER-POSITION CONTINUITY",
        "binding = IgnoredBinding();",
        "g_active_task_fail_closed = true;",
    ):
        need(token in cpp, "Existing interpolation safety marker lost: " + token)

    if a.with_generated:
        out = root / "runtime-recomp" / "RecompiledFuncs"
        need(out.is_dir(), "RecompiledFuncs missing after N64Recomp")
        files = sorted(out.glob("*.c"))
        need(files, "N64Recomp emitted no CPU .c files")
        corpus = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in files)

        for marker in GENERATED_MARKERS:
            need(marker in corpus, "N64Recomp did not emit policy marker: " + marker)
        for marker in OLD_POSTPATCH_MARKERS:
            need(marker not in corpus, "Stale post-generation patch marker found: " + marker)

        print("[OK] Generated RecompiledFuncs read-only verification PASS.")
        print("[OK] v32/v35/v36/Tinker hooks were emitted by N64Recomp policy.")
        print("[OK] No old post-generation patch markers are present.")

    print("[OK] Rocket-R v42 golden-rule recomp policy verification PASS.")
    print("[OK] RecompiledFuncs is not an active modification target.")


def re_search_generated_invocation(text: str) -> bool:
    import re
    return re.search(r"Invoke-Python[^\n]*patch_[^\n]*_generated\.py", text, re.I) is not None


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print("v42 policy verification failed:", exc, file=sys.stderr)
        raise SystemExit(1)
