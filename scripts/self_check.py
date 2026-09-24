#!/usr/bin/env python3
"""Validate the checked-in Rocket-R source/bootstrap metadata without needing a ROM."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.10 in Ubuntu 22.04 containers
    import tomli as tomllib

HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z._-]+)?$")
ROM_SUFFIXES = {".z64", ".v64", ".n64"}
ROM_MAGICS = {bytes.fromhex("80371240"), bytes.fromhex("37804012"), bytes.fromhex("40123780")}
IGNORED_TOP_LEVEL = {"build", "dist", "extern", ".git"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_patch_syntax(path: Path) -> None:
    git = shutil.which("git")
    require(git is not None, "Git is required to validate dependency patches")
    result = subprocess.run(
        [git, "apply", "--stat", str(path)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    detail = (result.stdout or "").strip()
    require(result.returncode == 0,
            f"malformed/corrupt dependency patch: {path}\n{detail}")

def scan_checked_source(root: Path) -> None:
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if relative.parts and relative.parts[0] in IGNORED_TOP_LEVEL:
            continue
        if "__pycache__" in relative.parts:
            continue
        require(path.suffix.lower() not in ROM_SUFFIXES,
                f"ROM-like file must not be checked into source: {relative}")
        try:
            with path.open("rb") as stream:
                require(stream.read(4) not in ROM_MAGICS,
                        f"N64 ROM header detected in checked source: {relative}")
        except OSError as exc:
            raise ValueError(f"could not inspect {relative}: {exc}") from exc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()

    required = [
        "VERSION", "CMakeLists.txt", "dependencies.lock.json", "patches/manifest.json",
        "runtime-recomp/rocket.us.recomp-policy.json", "runtime-recomp/rsp/n_aspMain.us.toml",
        "scripts/bootstrap_dependencies.py", "scripts/generate_recomp_config.py",
        "scripts/validate_rom.py", "scripts/scan_release.py", "scripts/OneClickBuild.ps1",
        "scripts/Build-Android.ps1", "scripts/package_appimage.sh",
        "scripts/build_rocket_decomp.sh", "Build-Linux.sh",
        "packaging/android/build.gradle", "packaging/android/settings.gradle",
        "packaging/android/app-build.gradle", "packaging/android/app/jni/CMakeLists.txt",
        "packaging/android/app/src/main/AndroidManifest.xml",
        "packaging/android/app/src/main/java/com/rocketret/rocketr/MainActivity.java",
        "packaging/android/app/src/main/java/com/rocketret/rocketr/RocketActivity.java",
        "packaging/linux/rocket-r.desktop", "packaging/linux/rocket-r.svg",
        "src/main.cpp", "src/platform.cpp", "src/runtime_ui.cpp", "src/runtime_ui.hpp",
        "src/rt64_renderer.cpp", "src/vi_presentation_policy.hpp", "src/renderer_snapshot.cpp", "src/renderer_snapshot.hpp",
        "src/game_registration.cpp", "src/crash_handler.cpp", "src/crash_handler.hpp",
    ]
    for name in required:
        require((root / name).is_file(), f"missing required source file: {name}")

    version = (root / "VERSION").read_text(encoding="utf-8-sig").strip()
    require(bool(VERSION_RE.fullmatch(version)), f"invalid VERSION value: {version!r}")

    lock = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))
    require(lock.get("schemaVersion") == 1, "unsupported dependencies.lock.json schema")
    deps = lock.get("dependencies")
    require(isinstance(deps, list) and deps, "dependency lock has no dependencies")
    names: set[str] = set()
    destinations: set[str] = set()
    for dep in deps:
        require(isinstance(dep, dict), "dependency entry is not an object")
        name = str(dep.get("name", ""))
        destination = str(dep.get("destination", ""))
        commit = str(dep.get("commit", ""))
        repository = str(dep.get("repository", ""))
        require(name and name not in names, f"duplicate/empty dependency name: {name!r}")
        require(destination and destination not in destinations,
                f"duplicate/empty dependency destination: {destination!r}")
        require(bool(HEX40.fullmatch(commit)), f"{name}: commit is not a full 40-digit SHA")
        require(repository.startswith("https://github.com/") and repository.endswith(".git"),
                f"{name}: repository must be an explicit GitHub clone URL")
        names.add(name)
        destinations.add(destination)

    manifest = json.loads((root / "patches/manifest.json").read_text(encoding="utf-8"))
    require(manifest.get("schemaVersion") == 1, "unsupported patch manifest schema")
    for dep in manifest.get("dependencies", []):
        require(str(dep.get("expectedCommit", "")) in {str(x["commit"]) for x in deps},
                f"patch manifest commit for {dep.get('name')} is not dependency-pinned")
        for patch in dep.get("patches", []):
            patch_path = root / str(patch.get("path", ""))
            require(patch_path.is_file(), f"missing patch: {patch_path.relative_to(root)}")
            expected = str(patch.get("sha256", "")).lower()
            require(bool(re.fullmatch(r"[0-9a-f]{64}", expected)),
                    f"patch has invalid/missing SHA-256: {patch_path.relative_to(root)}")
            require(sha256(patch_path) == expected,
                    f"patch SHA-256 mismatch: {patch_path.relative_to(root)}")
            validate_patch_syntax(patch_path)

    policy = json.loads((root / "runtime-recomp/rocket.us.recomp-policy.json").read_text(encoding="utf-8"))
    require(policy.get("schemaVersion") == 1, "unsupported Rocket recomp policy schema")
    for field in ("functionSizes", "manualFunctions", "functionHooks", "instructionPatches",
                  "stubs", "renamed", "ignored"):
        require(isinstance(policy.get(field), list), f"policy field {field} must be an array")

    expected_zero_size_boundaries = {
        "bzero": "0x000000A0",
        "osWritebackDCache": "0x00000080",
        "osInvalDCache": "0x000000B0",
        "osWritebackDCacheAll": "0x00000030",
        "osSetIntMask": "0x000000A0",
        "__osDisableInt": "0x00000020",
        "__osRestoreInt": "0x00000020",
        "__osExceptionPreamble": "0x00000010",
        "__osException": "0x00000564",
        "send_mesg": "0x000000F0",
        "__osEnqueueAndYield": "0x00000108",
        "__osEnqueueThread": "0x00000048",
        "__osPopThread": "0x00000010",
        "__osDispatchThread": "0x0000017C",
        "__osCleanupThread": "0x00000010",
        "__osGetSR": "0x00000010",
        "__osSetSR": "0x00000010",
        "__osSetFpcCsr": "0x00000010",
        "osInvalICache": "0x00000080",
        "osMapTLBRdb": "0x00000060",
        "osGetCount": "0x00000010",
        "bcopy": "0x00000320",
        "__osProbeTLB": "0x000000C0",
        "__osSetCompare": "0x00000010",
    }
    actual_sizes = {str(e.get("name")): str(e.get("size")) for e in policy["functionSizes"] if isinstance(e, dict)}
    require(all(actual_sizes.get(name) == size for name, size in expected_zero_size_boundaries.items()),
            "Rocket recomp policy must retain the 24 verified zero-size libultra boundaries")
    require("EntryPoint" not in actual_sizes and len(expected_zero_size_boundaries) == 24,
            "FIXED34 must not statically execute/size Rocket's raw retail EntryPoint bootstrap")


    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    include_marker = "target_include_directories(RocketR PRIVATE"
    require(include_marker in cmake_text, "CMakeLists.txt is missing RocketR include configuration")
    rocket_include_block = cmake_text.split(include_marker, 1)[1].split(")", 1)[0]
    require('"${ROCKET_SOURCE_ROOT}"' not in rocket_include_block,
            "RocketR must not add the repository root to its include path; VERSION shadows C++20 <version> on Windows")
    require('"${ROCKET_SOURCE_ROOT}/generated"' in rocket_include_block,
            "RocketR must include only the generated header directory instead of the repository root")
    require('set(ROCKET_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")' in cmake_text,
            "FIXED34 must keep a CMAKE_CURRENT_SOURCE_DIR Rocket root so Android can add the runtime as a subdirectory")
    require(cmake_text.startswith("cmake_minimum_required(VERSION 3.22.1)"),
            "FIXED34 root CMake minimum must remain compatible with the pinned Android SDK CMake 3.22.1")
    require('/clang:-msse4.1' in cmake_text,
            "RocketR clang-cl builds must enable SSE4.1 for librecomp generated RSP vector intrinsics")
    require('src/runtime_ui.cpp' in cmake_text and 'imgui_impl_sdlrenderer2.cpp' in cmake_text,
            "RocketR must build the SDL/ImGui launcher and overlay frontend")
    require('src/renderer_snapshot.cpp' in cmake_text and 'src/crash_handler.cpp' in cmake_text and
            'src/widescreen_culling.cpp' in cmake_text,
            "RocketR must build the graphics snapshot bridge, widescreen culling bridge and crash diagnostics")
    require('comdlg32 dbghelp' in cmake_text,
            "Windows RocketR must link native ROM browsing and minidump support")
    require('generated/bootstrap.generated.hpp' in cmake_text,
            "CMakeLists.txt must require the generated Rocket bootstrap address header")
    game_registration = (root / "src/game_registration.cpp").read_text(encoding="utf-8")
    require('#include "rom_identity.generated.hpp"' in game_registration and
            '#include "generated/rom_identity.generated.hpp"' not in game_registration,
            "game_registration.cpp must include rom_identity.generated.hpp through the isolated generated include directory")
    require('#include "bootstrap.generated.hpp"' in game_registration,
            "game_registration.cpp must consume the generated Rocket bootstrap addresses")
    require("std::memset(rdram + bss_start" in game_registration and
            "kBootstrapBssStart" in game_registration and "kBootstrapBssEnd" in game_registration and
            "context->r29 = sign_extended_address(rocket::generated::kInitialStackPointer)" in game_registration and
            "entry.entrypoint_address = sign_extended_address(generated::kRetailLoadAddress)" in game_registration and
            "entry.entrypoint = RunRocketEntrypoint" in game_registration and
            "entry.on_init_callback = InitialiseEntrypointContext" in game_registration,
            "FIXED34 must reproduce Rocket's bootstrap BSS clear/stack while keeping ROM DMA at 0x80000400 and invoking game_init through a wrapper")

    builder = (root / "scripts/OneClickBuild.ps1").read_text(encoding="utf-8-sig")
    require('Banner "Rocket-R ${Version}: local static recompilation builder"' in builder,
            "OneClickBuild.ps1 must delimit Version before a literal colon for Windows PowerShell 5.1")
    require("$BuilderRevision = 'FIXED34'" in builder,
            "OneClickBuild.ps1 must identify this multi-platform FIXED27-baseline source as FIXED34")
    require(".Replace([char]0,'')" not in builder and '.Replace([char]0,"")' not in builder,
            "OneClickBuild.ps1 contains the PowerShell 5.1 Replace(char,char) empty-string trap")
    require("ROCKET_R_BUILDER_WINPATH" in builder and "$bridgeName + '/p'" in builder,
            "OneClickBuild.ps1 is missing the WSLENV /p Windows-to-WSL path bridge")
    require(builder.find("$WslRoot = Resolve-WslPath $Root") < builder.find("Banner '3/9 - Pinned source dependencies'"),
            "OneClickBuild.ps1 must validate WSL path translation before dependency/ROM stages")
    require("python3-dev libyaml-dev pkg-config" in builder and "libc6-i386" in builder,
            "OneClickBuild.ps1 must install the Rocket/Splat/legacy-compiler prerequisites")
    require("function Ensure-RocketDecompHelper" in builder and "[System.IO.File]::WriteAllText" in builder,
            "OneClickBuild.ps1 must embed and self-repair its Rocket decomp helper")
    require("build_rocket_decomp.sh" in builder and "rocket-decomp-$Stamp.log" in builder,
            "OneClickBuild.ps1 must use the separately logged Rocket decomp helper")
    require("--exec', '/bin/bash'" in builder and "@wslStage5Args" in builder,
            "OneClickBuild.ps1 must launch the stage-5 helper directly through wsl.exe --exec")
    require("$decompCmd =" not in builder,
            "OneClickBuild.ps1 must not compose the stage-5 helper into a bash -lc command string")
    require("N64RecompCLI" in builder and "Configuring N64Recomp with native Ninja + MSVC" in builder,
            "OneClickBuild.ps1 must build the N64Recomp CLI executable with native Ninja + MSVC")
    require("function Get-NativeCMake" in builder and "function Test-CMakeMinimumVersion" in builder,
            "OneClickBuild.ps1 must discover a native Windows CMake 3.24+ without relying on the VS project generator")
    require("function Test-SuspiciousUnixToolPath" in builder and "devkitPro" in builder and "Get-Command cmake.exe -All" in builder,
            "OneClickBuild.ps1 must reject MSYS/devkitPro tool shadowing")
    require("function Get-NativeNinja" in builder and "CMAKE_MAKE_PROGRAM=$NativeNinja" in builder,
            "OneClickBuild.ps1 must bind native Ninja explicitly for Windows CMake stages")
    require("-DCMAKE_C_COMPILER=$MsvcCl" in builder and "-DCMAKE_CXX_COMPILER=$MsvcCl" in builder,
            "OneClickBuild.ps1 must bind N64Recomp/hash builds to the imported MSVC compiler")
    require("function Get-ClangCl" in builder and "-DCMAKE_C_COMPILER=$ClangCl" in builder and "-DCMAKE_CXX_COMPILER=$ClangCl" in builder,
            "OneClickBuild.ps1 must bind the Rocket runtime build directly to clang-cl")
    require("'Visual Studio 17 2022'" not in builder and 'Test-CMakeSupportsVisualStudio' not in builder,
            "OneClickBuild.ps1 must not gate the build on the Visual Studio CMake generator")
    require("n64recomp-tools-$Stamp.log" in builder and "rocket-runtime-windows-$Stamp.log" in builder,
            "OneClickBuild.ps1 must preserve detailed native CMake logs")
    require("dependency-bootstrap-$Stamp.log" in builder and "Invoke-Python $bootstrapArgs $DependencyLog" in builder,
            "FIXED34 must preserve a dedicated dependency-bootstrap log")
    bootstrap = (root / "scripts/bootstrap_dependencies.py").read_text(encoding="utf-8")
    require("using locally cached pinned commit" in bootstrap and "rev-parse" in bootstrap and "--no-tags" in bootstrap,
            "FIXED34 dependency bootstrap must prefer locally cached pinned commits and fetch only missing objects")
    require('git_args("apply", "--check"' in bootstrap and "Patch no longer applies cleanly" in bootstrap,
            "FIXED34 dependency bootstrap must report the exact failing patch before aborting")
    require("Resolve-RocketBootstrap" in builder and "game_init" in builder and "0x803FFFF0" in builder and
            "0x800AFD70" not in builder and "rocket-entry-disasm-$Stamp.log" in builder,
            "OneClickBuild.ps1 must keep retail ROM placement separate from game_init and derive the bootstrap caller stack directly from EntryPoint")
    require("static-recomp-$Stamp.log" in builder and "--dump-context" in builder,
            "OneClickBuild.ps1 must preserve stage-7 N64Recomp/RSPRecomp diagnostics and validate the ELF context")
    require("static-recomp policy: game_init callable entry + 24 verified zero-size assembly routines" in builder,
            "OneClickBuild.ps1 must report the FIXED34 split load/call entry policy")
    require("FIXED27 gameplay/interpolation baseline restored exactly" in builder and
            "retail 30 Hz simulation + RT64 presentation interpolation + safe-area crop + widescreen CPU frustum" in builder,
            "OneClickBuild.ps1 must report the FIXED34 rollback/runtime policy")

    n64_patch = (root / "patches/n64recomp/0001-fix-high-vram-entrypoint-comparison.patch").read_text(encoding="utf-8")
    require("value == entrypoint_address" in n64_patch and "configured_entry_size" in n64_patch and "else if (size == 0)" in n64_patch,
            "N64Recomp patch must apply configured entry sizes before renaming, preserve real sizes, and compare KSEG0 addresses safely")
    require("!found_entrypoint_func" in n64_patch and "rom_address == 0x1000" in n64_patch,
            "N64Recomp patch must retain deterministic configured entrypoint selection")

    modern_manifest = next((d for d in manifest.get("dependencies", []) if d.get("name") == "N64ModernRuntime"), None)
    require(modern_manifest is not None and len(modern_manifest.get("patches", [])) == 12,
            "FIXED34 must carry the FIXED27 N64ModernRuntime stability set plus the Android thread portability patch")
    modern_patch_text = "\n".join(
        (root / patch["path"]).read_text(encoding="utf-8")
        for patch in modern_manifest["patches"])
    for marker in ("mq == NULLPTR", "notify_external_message_waiters", "rdram_snapshot",
                   "screen_update_action_pending", "std::this_thread::yield",
                   "get_audio_status", "audio_callbacks.get_status"):
        require(marker in modern_patch_text,
                f"N64ModernRuntime stability patch set is missing marker: {marker}")

    rt64_manifest = next((d for d in manifest.get("dependencies", []) if d.get("name") == "RT64"), None)
    require(rt64_manifest is not None and len(rt64_manifest.get("patches", [])) == 14,
            "Rocket-R self-check RT64 patch count must match the current manifest, including the DKR-R semantic identity patch")
    rt64_patch_text = "\n".join(
        (root / patch["path"]).read_text(encoding="utf-8")
        for patch in rt64_manifest["patches"])
    for marker in ("skipBuffering", "totalInterpolatedPresentations", "colorImageAddresses",
                   "earlyPresentApproved", "RT64_ROCKET_SAFE_AREA_CROP", "18.0f / 320.0f",
                   "14.0f / 240.0f", "ActiveWidth", "ActiveHeight"):
        require(marker in rt64_patch_text,
                f"Rocket RT64 interpolation/presentation patch set is missing marker: {marker}")

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    require("target_compile_definitions(rt64 PRIVATE RT64_ROCKET_SAFE_AREA_CROP=1)" in cmake,
            "FIXED34 must enable the Rocket-only RT64 safe-area crop only on the RT64 target")

    main_cpp = (root / "src/main.cpp").read_text(encoding="utf-8")
    platform_cpp = (root / "src/platform.cpp").read_text(encoding="utf-8")
    renderer_cpp = (root / "src/rt64_renderer.cpp").read_text(encoding="utf-8")
    renderer_hpp = (root / "src/rt64_renderer.hpp").read_text(encoding="utf-8")
    vi_policy = (root / "src/vi_presentation_policy.hpp").read_text(encoding="utf-8")
    ui_cpp = (root / "src/runtime_ui.cpp").read_text(encoding="utf-8")
    widescreen_cpp = (root / "src/widescreen_culling.cpp").read_text(encoding="utf-8")
    widescreen_hpp = (root / "src/widescreen_culling.hpp").read_text(encoding="utf-8")
    require("audio_callbacks.get_status = rocket::platform::audio_status" in main_cpp,
            "FIXED34 must expose Rocket's emulated two-slot AI status through N64ModernRuntime")
    require("g_ai_fifo_frames" in platform_cpp and "AI_STATUS_FIFO_FULL" in platform_cpp and
            "advance_virtual_ai_locked" in platform_cpp and "AudioClock = std::chrono::steady_clock" in platform_cpp and
            "elapsed_seconds * static_cast<double>(g_audio_frequency)" in platform_cpp and
            "desired.samples = 256" in platform_cpp and
            "guest AI clock is independent" in platform_cpp and
            "SDL_GetQueuedAudioSize" in platform_cpp and
            "g_audio_total_submitted_frames" not in platform_cpp and
            "g_audio_accounted_consumed_frames" not in platform_cpp and
            "open_audio_locked(g_audio_frequency)" not in platform_cpp,
            "FIXED34 must pace Rocket's two-slot N64 AI FIFO from a continuous wall clock, keep SDL host buffering independent, and use a sub-DMA callback quantum")
    require("audio_profile=2" in ui_cpp and "volume = 0.65F" in ui_cpp and
            "normalized default" in ui_cpp,
            "FIXED34 must preserve the normalized 65% default/migration for legacy audio settings")
    require("config.window_handle = window_handle" in main_cpp and "std::thread runtime_thread" in main_cpp,
            "FIXED34 must hand the main-thread SDL window to N64ModernRuntime and run recomp::start on a worker")
    require("gfx_callbacks.create_window" not in main_cpp and "events_callbacks.vi_callback = nullptr" in main_cpp,
            "FIXED34 must not let N64ModernRuntime own the SDL window or use the old VI callback start path")
    require("SDL_PumpEvents();" not in platform_cpp and "pump_runtime_events" in platform_cpp,
            "FIXED34 input callbacks must consume a main-thread SDL snapshot instead of pumping SDL from guest threads")
    require("void send_dl(const OSTask* task, std::uint8_t* rdram_snapshot) override" in renderer_hpp and
            "SnapshotScope snapshot" in renderer_cpp and "rocket::start_game_once()" in renderer_cpp,
            "FIXED34 renderer must consume immutable graphics-task snapshots and start Rocket on the first safe VI present")
    require("kCanonicalViWidth = 320U" in vi_policy and "kCanonicalViHeight = 240U" in vi_policy and
            "inferred_vi_height" in vi_policy and "canonicalise_rocket_v_region" in vi_policy and
            '#include "vi_presentation_policy.hpp"' in renderer_cpp and
            "CanonicalViPresentationScope" in renderer_cpp and
            "CanonicalViPresentationScope vi_scope(*application_)" in renderer_cpp and
            "presentation.removeBlackBorders = true" in renderer_cpp,
            "FIXED34 must normalize RT64's inferred VI guard rows while preserving the 320x240 authored presentation")
    require("kAuthoredPresentationRate = 30" in renderer_cpp and
            "setRefreshRate(kAuthoredPresentationRate)" in renderer_cpp and
            "RT64::UserConfiguration::RefreshRate::Manual" in renderer_cpp and
            "totalInterpolatedPresentations" in renderer_cpp and
            "presentation_mutex_" in renderer_hpp,
            "FIXED34 must pin Rocket's authored 30 Hz source cadence and enable live RT64 interpolation targets without altering simulation timing")
    require('"refresh_rate="' in ui_cpp and '"refresh_manual="' in ui_cpp and
            '"Original 30 FPS"' in ui_cpp and '"Match display"' in ui_cpp and
            "rr_manual_value" in ui_cpp and "30, 500" in ui_cpp,
            "FIXED34 must expose and persist Original/Match Display/Custom frame interpolation controls")
    frustum_hooks = [entry for entry in policy["functionHooks"]
                     if isinstance(entry, dict) and entry.get("function") == "frustum_test"]
    require(len(frustum_hooks) == 1 and
            frustum_hooks[0].get("beforeVram") == "0x8003C764" and
            "rocket_widescreen_frustum_begin" in str(frustum_hooks[0].get("text", "")),
            "FIXED34 must hook Rocket's verified frustum_test entry exactly once")
    # v12.1: FIXED34's old horizontal-plane assertion is obsolete.
    _v121_culling_path = (__import__('pathlib').Path(__file__).resolve().parents[1] / 'src' / 'widescreen_culling.cpp')
    _v121_culling = _v121_culling_path.read_text(encoding='utf-8-sig')
    _v121_required = all(token in _v121_culling for token in (
        'void rocket::widescreen::update_window_aspect(SDL_Window* window)',
        'position_address = static_cast<std::uint32_t>(context->r5)',
        'authored_radius = std::bit_cast<float>',
        'target_diagonal_half',
        'required_radius = std::max(required_radius, plane_distance)',
        'context->r6 = static_cast<gpr>',
        'kObjectCullGuard',
        'kAngularHysteresis',
    ))
    _v121_retired_shared_rewrite = (
        'pair_index < order.size()' in _v121_culling or
        '[culling] CPU frustum guard now covers left/right/top/bottom' in _v121_culling
    )
    # v13.1: exact object-local target-frustum culling; global integrity check is scoped to genuine retired cone markers.
    _v13_culling_path = (__import__('pathlib').Path(__file__).resolve().parents[1] / 'src' / 'widescreen_culling.cpp')
    _v13_culling = _v13_culling_path.read_text(encoding='utf-8-sig')
    _v13_required = all(token in _v13_culling for token in (
        'kPairings',
        'outward_sum',
        'target_planes',
        'requested_horizontal_half',
        'requested_vertical_half',
        'kTargetFrustumGuard = 1.15F',
        'plane_distance > authored_radius + edge_slack',
        'required_radius = std::max(required_radius, plane_distance)',
        'context->r6 = static_cast<gpr>',
    ))
    _v13_forbidden = any(token in _v13_culling for token in (
        'target_diagonal_half',
        'sphere_angle',
        'kForwardOffset), forward',
    ))
    # v14: expanded presentation bypasses Rocket's stale CPU side-plane rejection.
    _v14_path = (__import__('pathlib').Path(__file__).resolve().parents[1] / 'src' / 'widescreen_culling.cpp')
    _v14 = _v14_path.read_text(encoding='utf-8-sig')
    _v14_required = all(token in _v14 for token in (
        'kNoSideCullRadiusBits = 0x7F7FFFFFU',
        'rocket::graphics::widescreen_active(4.0F / 3.0F)',
        'settings.fov_offset_degrees > 0.001F',
        'context->r6 = static_cast<gpr>(kNoSideCullRadiusBits)',
        'culling/fade remains active',
    ))
    _v14_forbidden = any(token in _v14 for token in (
        'target_diagonal_half', 'target_planes',
        'requested_horizontal_half', 'requested_vertical_half',
        'required_radius = std::max(required_radius, plane_distance)',
    ))
    # v26: keep the proven v21 queue recovery and retail draw-distance path, but bind
    # interpolated model/submodel presentation to the actual GameObject owner instead of
    # post-hoc nearest-neighbour RenderEntry matching. The retired v23 mask bypass must be off.
    _v26_root = __import__('pathlib').Path(__file__).resolve().parents[1]
    _v26_culling = (_v26_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    _v26_graphics = (_v26_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    _v26_presentation = (_v26_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
    _v26_header = (_v26_root / 'src' / 'presentation_identity.hpp').read_text(encoding='utf-8-sig')
    _v26_oneclick = (_v26_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    _v26_policy = json.loads((_v26_root / 'runtime-recomp' / 'rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))
    _v26_queue_path = _v26_root / 'scripts' / 'patch_render_queue_expansion_v21_generated.py'
    require(_v26_queue_path.is_file(), 'v21 generated queue patcher missing')
    _v26_queue = _v26_queue_path.read_text(encoding='utf-8-sig')
    _v26_hooks = _v26_policy.get('functionHooks', [])
    _v26_owner_hooks = [h for h in _v26_hooks if h.get('function') == 'func_8001ECEC' and str(h.get('beforeVram','')).upper() == '0X8001F084' and 'rocket_presentation_model_entry_owner' in h.get('text','')]
    _v26_required = (
        'v16 viewport-locked FOV/aspect guard active' in _v26_culling and
        'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in _v26_culling and
        'static_cast<std::uint32_t>(context->r7)' in _v26_graphics and
        's.draw_distance_multiplier' in _v26_graphics and
        'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN' in _v26_presentation and
        'rocket_render_queue_prepare_first_batch' in _v26_presentation and
        'rocket_render_queue_prepare_next_batch' in _v26_presentation and
        'PendingModelOwner' in _v26_presentation and
        'OwnerTrack' in _v26_presentation and
        'owner_key' in _v26_presentation and
        'DURABLE-OWNER-V26' in _v26_presentation and
        'pre-render-object-gate=RETAIL' in _v26_presentation and
        'rocket_presentation_model_entry_owner' in _v26_header and
        len(_v26_owner_hooks) == 1 and
        'patch_render_queue_expansion_v21_generated.py' in _v26_oneclick and
        'patch_visibility_retention_v23_generated.py' not in _v26_oneclick and
        'ROCKET_QUEUE_V21_PROCESS_BATCH' in _v26_queue
    )
    _v26_forbidden = any(token in (_v26_presentation + _v26_oneclick) for token in (
        'authored-submodel-mask=RECOVERED-V23',
        'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN',
        'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',
        'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',
        'rocket_original_func_8008B694', 'rocket_capture_func_8008B694',
        'rocket_draw_func_8008B694', '[render-queue] GLOBAL',
        'pre-render-object-gate=RECOVERED-V25', 'rocket_popdiag_object_gate_result',
        'object_gate_recovered', 'patch_prerender_object_gate_v25_generated.py',
        'ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION BEGIN',
    ))
    if not (_v26_required and not _v26_forbidden):
        raise SystemExit('SOURCE SELF-CHECK FAILED: v26 durable presentation ownership state missing')
    scan_checked_source(root)
    print(f"Rocket-R source self-check PASS ({version}).")
    print(f"Pinned dependencies: {len(deps)}; patch integrity: PASS; ROM-free source: PASS.")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"SOURCE SELF-CHECK FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
