#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

PATCH_REL = "patches/rt64/0009-rocket-configurable-n64-dithering.patch"
VERIFY_REL = "scripts/verify_n64_dithering_v71.py"
PURPOSE = (
    "Expose Rocket-R launcher control over the original N64 Bayer colour dithering by forcing "
    "decoded RT64 draw-state RGB/alpha dither modes to disabled while the option is off, "
    "without mutating guest RDP state."
)


def fail(message: str) -> None:
    raise RuntimeError(message)


def write_text(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def insert_once(text: str, anchor: str, insertion: str, label: str, after: bool = True) -> str:
    if insertion.strip() in text:
        return text
    count = text.count(anchor)
    if count != 1:
        fail(f"{label}: expected exactly one anchor {anchor!r}, found {count}")
    pos = text.index(anchor)
    return text[: pos + (len(anchor) if after else 0)] + insertion + text[pos + (len(anchor) if after else 0) :]


def validate_base(root: Path) -> None:
    required = [
        "src/runtime_ui.cpp",
        "src/runtime_ui.hpp",
        "src/rt64_renderer.cpp",
        "src/presentation_identity.cpp",
        "patches/manifest.json",
        "scripts/self_check.py",
        "scripts/OneClickBuild.ps1",
        "scripts/verify_attachment_skybox_interpolation_v6.py",
    ]
    for rel in required:
        if not (root / rel).is_file():
            fail(f"Rocket-R source is missing {rel}")

    presentation = read(root / "src/presentation_identity.cpp")
    for marker in ("struct SharedMatrixSample", "CanonicalGfxRef", "shared-match=%llu"):
        if marker not in presentation:
            fail(f"Rocket-R v6 interpolation base missing marker {marker}")


def patch_runtime_ui_cpp(path: Path) -> None:
    text = read(path)

    if "std::atomic<bool> g_n64_dithering_enabled{true};" not in text:
        text = insert_once(
            text,
            "std::atomic<bool> g_overlay_visible{false};\n",
            "std::atomic<bool> g_n64_dithering_enabled{true};\n",
            "runtime_ui.cpp global setting",
        )

    save_line = '    out << "n64_dithering=" << (g_n64_dithering_enabled.load(std::memory_order_relaxed) ? 1 : 0) << \'\\n\';\n'
    if 'out << "n64_dithering="' not in text:
        match = re.search(r'(?m)^(\s*out << "downsample="[^\n]*\n)', text)
        if not match:
            fail("runtime_ui.cpp: could not locate downsample SaveSettings line")
        text = text[:match.end()] + save_line + text[match.end():]

    if "bool n64_dithering = true;" not in text:
        match = re.search(r'(?m)^(\s*float volume = [^\n]+\n)', text)
        if not match:
            fail("runtime_ui.cpp: could not locate volume default")
        text = text[:match.end()] + "    bool n64_dithering = true;\n" + text[match.end():]

    if 'key == "n64_dithering"' not in text:
        match = re.search(r'(?m)^(\s*else if \(key == "downsample"\)[^\n]+\n)', text)
        if not match:
            fail("runtime_ui.cpp: could not locate downsample parser")
        text = text[:match.end()] + '            else if (key == "n64_dithering") n64_dithering = std::stoi(value) != 0;\n' + text[match.end():]

    store_line = "    g_n64_dithering_enabled.store(n64_dithering, std::memory_order_relaxed);\n"
    if store_line.strip() not in text:
        anchor = (
            "    ultramodern::renderer::set_graphics_config(graphics);\n"
            "    rocket::platform::set_master_volume(volume);\n"
        )
        if text.count(anchor) != 1:
            fail("runtime_ui.cpp: could not uniquely locate LoadSettings graphics/volume handoff")
        replacement = (
            "    ultramodern::renderer::set_graphics_config(graphics);\n"
            + store_line +
            "    rocket::platform::set_master_volume(volume);\n"
        )
        text = text.replace(anchor, replacement, 1)

    if 'ImGui::Checkbox("N64 colour dithering"' not in text:
        anchor = '    ImGui::TextDisabled("F11 or Alt+Enter toggles fullscreen at any time.");\n'
        block = '''\n    bool n64_dithering = g_n64_dithering_enabled.load(std::memory_order_relaxed);\n    if (ImGui::Checkbox("N64 colour dithering", &n64_dithering)) {\n        g_n64_dithering_enabled.store(n64_dithering, std::memory_order_relaxed);\n        changed = true;\n    }\n    ImGui::TextDisabled("Rocket enables a 4x4 Bayer colour-dither pattern on the 16-bit framebuffer.");\n    ImGui::TextDisabled("It is most visible in smooth skybox gradients. Disable this for a cleaner modern image.");\n'''
        text = insert_once(text, anchor, block, "runtime_ui.cpp graphics checkbox")

    if "bool rocket::ui::n64_dithering_enabled()" not in text:
        block = '''\n\nbool rocket::ui::n64_dithering_enabled() {\n    return g_n64_dithering_enabled.load(std::memory_order_relaxed);\n}\n'''
        text = text.rstrip() + block

    write_text(path, text)


def patch_runtime_ui_hpp(path: Path) -> None:
    text = read(path)
    if "bool n64_dithering_enabled();" not in text:
        text = insert_once(
            text,
            "bool overlay_visible();\n",
            "bool n64_dithering_enabled();\n",
            "runtime_ui.hpp declaration",
        )
    write_text(path, text)


def patch_renderer(path: Path) -> None:
    text = read(path)
    marker = "application_->state->setN64DitheringEnabled("
    if marker not in text:
        anchor = "    application_->state->rsp->reset();\n"
        block = '''    // Rocket enables 4x4 Bayer colour dithering as part of its normal RDP\n    // frame setup. Let the launcher preserve that retail look or disable it\n    // globally for cleaner gradients (most noticeably the skyboxes). RT64\n    // applies this at decoded draw-state level so later display-list state\n    // changes cannot silently re-enable dithering while the option is off.\n    application_->state->setN64DitheringEnabled(\n        rocket::ui::n64_dithering_enabled());\n\n'''
        # The task decode path has one RSP reset in the Rocket renderer. Insert before it.
        if text.count(anchor) < 1:
            fail("rt64_renderer.cpp: could not locate RSP reset")
        # Prefer the reset following authored presentation refresh-rate setup when present.
        rate_anchor = "application_->state->setRefreshRate(kAuthoredPresentationRate);"
        rate_pos = text.find(rate_anchor)
        if rate_pos >= 0:
            reset_pos = text.find(anchor, rate_pos)
            if reset_pos < 0:
                fail("rt64_renderer.cpp: could not locate decode RSP reset after refresh-rate setup")
            text = text[:reset_pos] + block + text[reset_pos:]
        else:
            reset_pos = text.find(anchor)
            text = text[:reset_pos] + block + text[reset_pos:]
    write_text(path, text)


def patch_self_check(path: Path, manifest_patch_count: int) -> None:
    text = read(path)

    # Do NOT replace the user's self-check. Adjust only the RT64 manifest count
    # that changed because v7 adds one legitimate pinned RT64 patch.
    count_re = re.compile(r'(len\(rt64_manifest\.get\("patches", \[\]\)\) == )(\d+)')
    matches = list(count_re.finditer(text))
    if matches:
        if len(matches) != 1:
            fail(f"self_check.py: expected one RT64 patch-count assertion, found {len(matches)}")
        old = int(matches[0].group(2))
        # Use the real manifest count instead of assuming the source revision.
        text = count_re.sub(lambda m: m.group(1) + str(manifest_patch_count), text, count=1)
        print(f"[OK] Preserved current self-check revision; RT64 patch count {old} -> {manifest_patch_count}.")

    dither_check = 'path.endswith("0009-rocket-configurable-n64-dithering.patch")'
    if dither_check not in text and "rt64_paths =" in text:
        line_re = re.compile(r'(?m)^(\s*rt64_paths\s*=\s*[^\n]+\n)')
        match = line_re.search(text)
        if not match:
            fail("self_check.py: found rt64_paths marker but could not locate assignment")
        indent = re.match(r'\s*', match.group(1)).group(0)
        block = (
            indent + 'require(sum(path.endswith("0009-rocket-configurable-n64-dithering.patch") for path in rt64_paths) == 1,\n' +
            indent + '        "Rocket-R configurable N64 dithering patch must appear exactly once in the RT64 manifest")\n'
        )
        text = text[:match.end()] + block + text[match.end():]

    write_text(path, text)


def install_patch_file(root: Path, payload: Path) -> None:
    src = payload / PATCH_REL
    dst = root / PATCH_REL
    if not src.is_file():
        fail(f"payload missing {PATCH_REL}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def update_manifest(root: Path) -> int:
    path = root / "patches/manifest.json"
    data = json.loads(read(path))
    rt64 = next((dep for dep in data.get("dependencies", []) if dep.get("name") == "RT64"), None)
    if rt64 is None:
        fail("patch manifest has no RT64 dependency")
    patches = rt64.setdefault("patches", [])
    patches[:] = [p for p in patches if p.get("path") != PATCH_REL]
    patch_path = root / PATCH_REL
    patches.append({
        "path": PATCH_REL,
        "purpose": PURPOSE,
        "sha256": hashlib.sha256(patch_path.read_bytes()).hexdigest(),
    })
    write_text(path, json.dumps(data, indent=2) + "\n")
    return len(patches)


def update_one_click_status(path: Path) -> None:
    if not path.is_file():
        return
    text = read(path)
    lines = [line for line in text.splitlines() if "N64 colour dithering v7:" not in line]
    text = "\n".join(lines) + "\n"
    status = "    Write-Host 'N64 colour dithering v7: LAUNCHER TOGGLE (retail Bayer / disabled).' -ForegroundColor Green\n"
    anchor = re.compile(r"(?m)^(\s*Write-Host\s+'Attachment/skybox interpolation v6:[^\n]*\n)")
    match = anchor.search(text)
    if match:
        text = text[:match.end()] + status + text[match.end():]
    else:
        policy = re.compile(r"(?m)^(\s*Write-Host\s+'Rocket runtime policy:[^\n]*\n)")
        match = policy.search(text)
        if match:
            text = text[:match.end()] + status + text[match.end():]
    write_text(path, text)


def install_verifier(root: Path, payload: Path) -> None:
    src = payload / VERIFY_REL
    dst = root / VERIFY_REL
    if not src.is_file():
        fail(f"payload missing {VERIFY_REL}")
    shutil.copy2(src, dst)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--payload", required=True)
    ns = ap.parse_args()
    root = Path(ns.root).resolve()
    payload = Path(ns.payload).resolve()

    validate_base(root)
    install_patch_file(root, payload)
    patch_runtime_ui_cpp(root / "src/runtime_ui.cpp")
    patch_runtime_ui_hpp(root / "src/runtime_ui.hpp")
    patch_renderer(root / "src/rt64_renderer.cpp")
    patch_count = update_manifest(root)
    patch_self_check(root / "scripts/self_check.py", patch_count)
    update_one_click_status(root / "scripts/OneClickBuild.ps1")
    install_verifier(root, payload)

    print("[OK] Rocket-R N64 dithering v7.2 repair applied surgically to the current source revision.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        raise SystemExit(1)
