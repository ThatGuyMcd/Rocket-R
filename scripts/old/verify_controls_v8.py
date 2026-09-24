#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def read(root: Path, relative: str) -> str:
    path = root / relative
    require(path.is_file(), f"missing {relative}")
    return path.read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()

    cmake = read(root, "CMakeLists.txt")
    platform_h = read(root, "src/platform.hpp")
    platform = read(root, "src/platform.cpp")
    ui_h = read(root, "src/runtime_ui.hpp")
    ui = read(root, "src/runtime_ui.cpp")
    input_h = read(root, "src/runtime_input.hpp")
    input_cpp = read(root, "src/runtime_input.cpp")

    require(cmake.count("src/runtime_input.cpp") == 1,
            "CMake must build runtime_input.cpp exactly once")
    require('enum class Action' in input_h and 'BindingSlot' in input_h and
            'ShortcutAction' in input_h,
            "runtime_input.hpp is missing the single-player binding model")
    require(input_cpp.count('"stick_up"') == 1 and
            '"c_right"' in input_cpp and 'Action::Count' in input_cpp,
            "runtime_input.cpp is missing the complete N64 control set")
    for marker in (
        "KeyboardPrimary", "KeyboardSecondary", "ControllerPrimary",
        "ControllerSecondary", "reset_bindings", "reset_shortcuts",
        "stick_deadzone", "stick_anti_deadzone", "stick_sensitivity",
        "stick_curve", "trigger_threshold", "background_input_enabled",
    ):
        require(marker in input_h and marker in input_cpp,
                f"input system missing {marker}")

    require('g_trigger_threshold{0.36624F}' in input_cpp,
            "default Z-trigger threshold no longer matches Rocket's prior 12,000 raw threshold")
    require('float threshold = 0.6516F' in input_cpp,
            "digital axis threshold no longer preserves Rocket's prior C-button stick feel")
    require('g_stick_deadzone{21.363F}' in input_cpp,
            "stick deadzone no longer preserves Rocket's prior 7,000 raw default")
    require('UpdateShortcutRequests(pad, keys, allow_shortcuts);' in input_cpp,
            "shortcut capture suppression is missing")

    for marker in (
        "ControllerChoice", "controller_choices", "preferred_controller_key",
        "set_preferred_controller_key", "rumble_strength", "test_rumble",
    ):
        require(marker in platform_h and marker in platform,
                f"platform controller integration missing {marker}")
    require('#include "runtime_input.hpp"' in platform,
            "platform.cpp is not wired to runtime_input")
    require('rocket::input::poll(' in platform and
            '!rocket::ui::input_capture_active()' in platform,
            "platform input sampling does not use the new binding engine safely")
    require('void keyboard_input(' not in platform and
            'void controller_input(' not in platform,
            "legacy hard-coded input paths are still present")

    for marker in (
        '"DEVICE", "N64 BINDINGS", "STICK", "SHORTCUTS"',
        'Every N64 control can have two keyboard inputs and two gamepad inputs.',
        'RESTORE DEFAULT N64 BINDINGS', 'RESTORE DEFAULT STICK FEEL',
        'RESTORE DEFAULT SHORTCUTS', 'RESCAN CONTROLLERS', 'TEST RUMBLE',
        'Player 1 gamepad', 'HandleInputCaptureEvent', 'DrawCapturePopup',
    ):
        require(marker in ui, f"Controls page missing marker: {marker}")
    require('controller_device=' in ui and 'input.' in ui and
            'stick_deadzone=' in ui and 'shortcut.overlay.keyboard=' in ui,
            "control settings are not persisted")
    require('bool input_capture_active();' in ui_h and
            'bool rocket::ui::input_capture_active()' in ui,
            "UI capture state is not exposed to the input sampler")
    require(ui.count('HandleInputCaptureEvent(&event)') >= 1 and
            ui.count('HandleInputCaptureEvent(event)') >= 1,
            "binding capture must work in both launcher and in-game overlay")

    print("[OK] Rocket-R single-player DKR-style Controls v8 verification PASS.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}")
        raise SystemExit(1)
