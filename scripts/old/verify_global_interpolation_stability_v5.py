#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

PATCH_REL = "patches/rt64/0008-rocket-dkrr-semantic-presentation-identities.patch"


def fail(message: str) -> None:
    raise SystemExit("ERROR: " + message)


def require(text: str, marker: str, label: str) -> None:
    if marker not in text:
        fail(f"{label}: missing marker {marker!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ns = ap.parse_args()
    root = Path(ns.root).resolve()

    required = [
        root / "src/presentation_identity.cpp",
        root / "src/presentation_identity.hpp",
        root / PATCH_REL,
        root / "patches/manifest.json",
        root / "src/rt64_renderer.cpp",
        root / "runtime-recomp/rocket.us.recomp-policy.json",
    ]
    for path in required:
        if not path.is_file():
            fail(f"missing {path.relative_to(root)}")

    cpp = (root / "src/presentation_identity.cpp").read_text(encoding="utf-8")
    hpp = (root / "src/presentation_identity.hpp").read_text(encoding="utf-8")
    patch = (root / PATCH_REL).read_text(encoding="utf-8")

    for marker in (
        "constexpr std::uint64_t kMaximumTrackAge = 1U;",
        "constexpr float kMaximumTrackDistance = 384.0F;",
        "kAmbiguityAbsoluteMargin",
        "track_best_entry",
        "clearly_better",
        "g_trace_ambiguous_rejects",
        "g_trace_sidecar_mismatches",
        "g_active_task_fail_closed = true;",
        "binding = IgnoredBinding();",
        "sidecar mismatch:",
        "identity==0 is an explicit G_EX_ID_IGNORE binding",
    ):
        require(cpp, marker, "presentation_identity.cpp")

    for forbidden in (
        "kMaximumTrackAge = 4U",
        "kBaseTrackDistance = 768.0F",
        "predicted = Add(track.position",
    ):
        if forbidden in cpp:
            fail(f"presentation_identity.cpp still contains permissive v4.2 matcher marker {forbidden!r}")

    require(hpp, '#include "recomp.h"', "presentation_identity.hpp")
    require(hpp, "identity 0 is an explicit G_EX_ID_IGNORE", "presentation_identity.hpp")

    for marker in (
        "if (rocket_presentation_matrix_binding(physicalAddress, &semantic))",
        "const bool interpolateTransform =",
        "group.decompose = false;",
        "group.positionInterpolation = interpolateTransform",
        "group.rotationInterpolation = interpolateTransform",
        "group.scaleInterpolation = G_EX_COMPONENT_SKIP;",
        "group.skewInterpolation = G_EX_COMPONENT_SKIP;",
        "group.perspectiveInterpolation = interpolateTransform",
        "group.lookAtInterpolation = G_EX_COMPONENT_SKIP;",
        "doLookAtInterpolation",
    ):
        require(patch, marker, PATCH_REL)

    if "rocket_presentation_matrix_binding(physicalAddress, &semantic) &&" in patch:
        fail("RT64 patch still allows explicit IGNORE bindings to fall back to automatic matching")

    manifest = json.loads((root / "patches/manifest.json").read_text(encoding="utf-8"))
    rt64 = next((d for d in manifest.get("dependencies", []) if d.get("name") == "RT64"), None)
    if rt64 is None:
        fail("RT64 manifest entry missing")
    records = [p for p in rt64.get("patches", []) if p.get("path") == PATCH_REL]
    if len(records) != 1:
        fail("semantic RT64 patch manifest record missing/duplicated")
    digest = hashlib.sha256((root / PATCH_REL).read_bytes()).hexdigest()
    if records[0].get("sha256") != digest:
        fail("semantic RT64 patch manifest hash mismatch")
    if "global interpolation stability v5" not in str(records[0].get("purpose", "")).lower():
        fail("semantic RT64 manifest purpose was not updated to v5")

    renderer = (root / "src/rt64_renderer.cpp").read_text(encoding="utf-8")
    require(renderer, "TaskIdentityScope identity_scope", "src/rt64_renderer.cpp")

    policy = json.loads((root / "runtime-recomp/rocket.us.recomp-policy.json").read_text(encoding="utf-8"))
    hooks = "\n".join(str(h.get("text", "")) for h in policy.get("functionHooks", []))
    for marker in (
        "rocket_presentation_frame_begin",
        "rocket_presentation_render_entry",
        "rocket_presentation_task_submitted",
    ):
        require(hooks, marker, "rocket.us.recomp-policy.json")

    print("[OK] Rocket-R global interpolation stability v5 verification PASS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
