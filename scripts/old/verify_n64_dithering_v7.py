#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

PATCH_REL = "patches/rt64/0009-rocket-configurable-n64-dithering.patch"


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
        "src/runtime_ui.cpp",
        "src/runtime_ui.hpp",
        "src/rt64_renderer.cpp",
        PATCH_REL,
        "patches/manifest.json",
    ]
    for rel in required:
        if not (root / rel).is_file():
            fail(f"missing {rel}")

    ui = (root / "src/runtime_ui.cpp").read_text(encoding="utf-8")
    ui_h = (root / "src/runtime_ui.hpp").read_text(encoding="utf-8")
    renderer = (root / "src/rt64_renderer.cpp").read_text(encoding="utf-8")
    patch = (root / PATCH_REL).read_text(encoding="utf-8")

    for marker in (
        "std::atomic<bool> g_n64_dithering_enabled{true};",
        'out << "n64_dithering="',
        'key == "n64_dithering"',
        'ImGui::Checkbox("N64 colour dithering"',
        "Rocket enables a 4x4 Bayer colour-dither pattern",
        "bool rocket::ui::n64_dithering_enabled()",
    ):
        require(ui, marker, "runtime_ui.cpp")

    require(ui_h, "bool n64_dithering_enabled();", "runtime_ui.hpp")
    require(renderer, "application_->state->setN64DitheringEnabled(", "rt64_renderer.cpp")
    require(renderer, "rocket::ui::n64_dithering_enabled()", "rt64_renderer.cpp")

    for marker in (
        "bool n64DitheringEnabled = true;",
        "void setN64DitheringEnabled(bool enabled);",
        "if (!extended.n64DitheringEnabled)",
        "G_AD_DISABLE | G_CD_DISABLE",
        "void State::setN64DitheringEnabled(bool enabled)",
        "updateDrawStatusAttribute(DrawAttribute::OtherMode);",
    ):
        require(patch, marker, PATCH_REL)

    manifest = json.loads((root / "patches/manifest.json").read_text(encoding="utf-8"))
    rt64 = next((dep for dep in manifest.get("dependencies", []) if dep.get("name") == "RT64"), None)
    if rt64 is None:
        fail("RT64 dependency missing from manifest")
    item = next((entry for entry in rt64.get("patches", []) if entry.get("path") == PATCH_REL), None)
    if item is None:
        fail(f"manifest missing {PATCH_REL}")
    actual = hashlib.sha256((root / PATCH_REL).read_bytes()).hexdigest()
    if item.get("sha256") != actual:
        fail(f"manifest SHA mismatch for {PATCH_REL}: {item.get('sha256')} != {actual}")

    print("[OK] Rocket-R N64 dithering launcher control v7 verification PASS.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
