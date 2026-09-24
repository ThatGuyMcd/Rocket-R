#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

PATCH_REL = "patches/rt64/0009-rocket-configurable-n64-dithering.patch"


def fail(message: str) -> None:
    raise SystemExit("ERROR: " + message)


def require(cond: bool, message: str) -> None:
    if not cond:
        fail(message)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ns = ap.parse_args()
    root = Path(ns.root).resolve()

    for rel in (
        "src/runtime_ui.cpp", "src/runtime_ui.hpp", "src/rt64_renderer.cpp",
        "scripts/self_check.py", "scripts/OneClickBuild.ps1", PATCH_REL,
        "patches/manifest.json",
    ):
        require((root / rel).is_file(), f"missing {rel}")

    ui = (root / "src/runtime_ui.cpp").read_text(encoding="utf-8")
    ui_h = (root / "src/runtime_ui.hpp").read_text(encoding="utf-8")
    renderer = (root / "src/rt64_renderer.cpp").read_text(encoding="utf-8")
    patch = (root / PATCH_REL).read_text(encoding="utf-8")
    self_check = (root / "scripts/self_check.py").read_text(encoding="utf-8")

    for marker in (
        "std::atomic<bool> g_n64_dithering_enabled{true};",
        'out << "n64_dithering="',
        'key == "n64_dithering"',
        'ImGui::Checkbox("N64 colour dithering"',
        "bool rocket::ui::n64_dithering_enabled()",
    ):
        require(marker in ui, f"runtime_ui.cpp missing {marker!r}")
    require("bool n64_dithering_enabled();" in ui_h, "runtime_ui.hpp missing dithering getter")
    require("application_->state->setN64DitheringEnabled(" in renderer,
            "rt64_renderer.cpp missing RT64 dithering state handoff")

    for marker in (
        "bool n64DitheringEnabled = true;",
        "void setN64DitheringEnabled(bool enabled);",
        "if (!extended.n64DitheringEnabled)",
        "G_AD_DISABLE | G_CD_DISABLE",
        "void State::setN64DitheringEnabled(bool enabled)",
    ):
        require(marker in patch, f"RT64 dithering patch missing {marker!r}")

    manifest = json.loads((root / "patches/manifest.json").read_text(encoding="utf-8"))
    rt64 = next((d for d in manifest.get("dependencies", []) if d.get("name") == "RT64"), None)
    require(rt64 is not None, "manifest has no RT64 dependency")
    records = [p for p in rt64.get("patches", []) if p.get("path") == PATCH_REL]
    require(len(records) == 1, "manifest must contain dithering patch exactly once")
    digest = hashlib.sha256((root / PATCH_REL).read_bytes()).hexdigest()
    require(records[0].get("sha256") == digest, "manifest SHA mismatch for dithering patch")

    # The repair must preserve whatever builder revision the user's tree already
    # uses. We only require that the current self-check agrees with the new RT64
    # manifest size; no FIXEDxx revision is imposed here.
    count_match = re.search(r'len\(rt64_manifest\.get\("patches", \[\]\)\) == (\d+)', self_check)
    if count_match:
        require(int(count_match.group(1)) == len(rt64.get("patches", [])),
                "current self-check RT64 patch count does not match manifest")
    require('0009-rocket-configurable-n64-dithering.patch' in self_check or count_match is not None,
            "self-check was not updated for the new RT64 patch")

    print("[OK] Rocket-R N64 dithering v7.2 repair verification PASS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
