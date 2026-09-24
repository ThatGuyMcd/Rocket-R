#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


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

    source_path = root / "src/presentation_identity.cpp"
    if not source_path.is_file():
        fail("missing src/presentation_identity.cpp")
    cpp = source_path.read_text(encoding="utf-8")

    # v5 safety must remain intact.
    for marker in (
        "constexpr std::uint64_t kMaximumTrackAge = 1U;",
        "constexpr float kMaximumTrackDistance = 384.0F;",
        "track_best_entry",
        "g_active_task_fail_closed = true;",
        "binding = IgnoredBinding();",
        "sidecar mismatch:",
    ):
        require(cpp, marker, "presentation_identity.cpp")

    # v6 coverage for articulated/shared matrices and alternating dynamic GFX.
    for marker in (
        "struct SharedMatrixSample",
        "struct SharedMatrixTrack",
        "CanonicalGfxRef",
        "0x80000000U | (p - begin)",
        "BuildSharedMatrixSamples",
        "MatchSharedMatrixSamples",
        "SharedMatrixSignature",
        "shared_bindings",
        "sample.track_token, 0x20U + sample.role_mask",
        "g_shared_matrix_tracks.clear();",
        "shared-match=%llu",
    ):
        require(cpp, marker, "presentation_identity.cpp")

    # The original v5 conflict path remains available for genuinely inconsistent
    # physical slots; v6 only overrides a matrix when all same-frame references
    # agree on the exact matrix contents.
    require(cpp, "accumulator.position_conflict = true;", "presentation_identity.cpp")
    require(cpp, "accumulator.descriptors.size() < 2U", "presentation_identity.cpp")

    print("[OK] Rocket-R attachment/skybox interpolation v6 verification PASS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
