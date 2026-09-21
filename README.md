# Rocket: Robot on Wheels — Recompiled (Rocket-R)

A local-first static recompilation project for the **Nintendo 64 US release of Rocket: Robot on Wheels** (`NSUE`). The project intentionally follows the same core engineering pattern as DKR-R: a matching decompilation ELF supplies symbols, N64Recomp translates the original MIPS CPU program, RSPRecomp translates the game's audio microcode, N64ModernRuntime supplies the N64 host environment, and RT64 renders the original F3DEX2 graphics tasks natively.

## One-click multi-platform build

Double-click:

```text
ONE-CLICK-BUILD.cmd
```

The builder now starts with a **multi-select platform question**. Pick any combination of:

- **Windows x64** — `Rocket-R-<version>-Windows-x64.zip`
- **Linux x86-64 / x64** — `Rocket-R-<version>-Linux-x86_64.AppImage`
- **Linux ARM64 / aarch64** — `Rocket-R-<version>-Linux-aarch64.AppImage`
- **Android ARM64 / arm64-v8a** — `Rocket-R-<version>-Android-arm64-v8a.apk`
- **All four**

The ROM-derived work is performed **once**. The matching NSUE ELF, CPU recompilation and RSP recompilation are then reused by every selected platform, so the four packages contain the same Rocket game translation and runtime feature set. Platform-specific stages only compile/package that shared source for the target OS/architecture.

The script checks or offers to install its Windows, WSL, Linux-container and Android prerequisites as needed. It then prepares exact pinned dependencies, validates your own unmodified Rocket USA ROM, builds RocketRet's matching `NSUE.elf`, runs N64Recomp/RSPRecomp once, and produces only the platform packages you selected. Every distributable is scanned to reject ROM data.

Linux x86-64 and ARM64 are packaged as type-2 AppImages in architecture-native Ubuntu 22.04 Docker userspaces. ARM64 uses QEMU/binfmt when the Windows/WSL host is x86-64. Android is built as an ARM64-only SDL2/RT64 Vulkan APK with a small Java launcher that imports the user's ROM through Android's document picker into app-private storage; the ROM is never placed in the APK.

If Windows/WSL installation needs a reboot or Ubuntu's first-run username setup, the builder stops cleanly and tells you exactly what to do. Rerunning safely regenerates protected dependency/generated work areas.

### Supported ROM

Initial target only:

- Rocket: Robot on Wheels (USA)
- Product/game code: `NSUE`
- ROM size: 12 MiB
- canonical SHA-1: `622D71A44DA0B81EA68092CAC9198C66154A4F4A`

The ROM is **not** part of this repository and is never included in release packages.

## Launcher, overlay and runtime controls

Rocket-R now opens a native launcher before the game starts. You can browse for or drag-and-drop your own `.z64`, `.n64` or `.v64` Rocket US ROM, choose RT64 graphics settings, adjust master volume and review the controls. A ROM passed with `--rom` is preselected and still goes through the launcher so the same frontend owns every launch path.

While the game is running:

- F1 or Escape — open/close the Rocket-R overlay
- F11 or Alt+Enter — toggle fullscreen
- WASD — analogue stick
- X or Space — A
- Z or Left Ctrl — B
- Shift — Z trigger
- Enter — Start
- Arrow keys — D-pad
- Q / E — L / R
- I / J / K / L — C-Up / C-Left / C-Down / C-Right

Gamepads are detected automatically. The launcher and overlay also feed SDL gamepad buttons into ImGui navigation, so the D-pad and face buttons can be used throughout the frontend. The runtime advertises a 4 Kbit EEPROM and Rumble Pak to the original game.

## Linux helpers

The normal path is still `ONE-CLICK-BUILD.cmd`, which generates the shared Rocket CPU/RSP translation first and then invokes the Linux helpers for any selected AppImage targets. After that shared generation exists, the helpers can also be run manually:

```bash
./Setup-Linux.sh
./Build-Linux.sh --arch x86_64
./Build-Linux.sh --arch aarch64
```

Both outputs are ROM-free AppImages under `dist/`. The ARM64 AppImage contains a genuine aarch64 executable, not an x86 binary with an ARM filename.

## Important project status

**FIXED34 deliberately rolls the runtime/interpolation stack back to the user-qualified FIXED27 baseline.** The FIXED28-31 transform-identity/dynamic-vertex experiments have been removed because they introduced substantial interpolation artefacts and a gameplay crash. The original FIXED27 RT64 interpolation patches, immutable graphics-task snapshots, safe-area presentation crop and widescreen CPU-frustum fix are retained. The only runtime-source differences from FIXED27 are compile-time platform bridges for Linux/Android; Windows and Linux continue through the same FIXED27 interpolation code path.

The Windows pipeline produces a complete native `Rocket-R.exe`. User testing confirmed the FIXED25 audio/video/full-viewport path and FIXED26 high-refresh interpolation path were smooth and correct, and FIXED27 kept those paths intact while adding **aspect-aware CPU frustum expansion in Expand to window mode**, preventing Rocket's original 4:3 object culling from visibly popping models at the wider left/right edges. The Graphics page now offers **Original 30 FPS**, **Match display**, and a **Custom 30-500 FPS** target. Rocket itself still authors one new game frame every two NTSC retraces (30 Hz); interpolation creates presentation frames between those authored frames and does not speed up the simulation, input polling or audio clock.

The bootstrap/runtime architecture is unchanged: N64ModernRuntime keeps Rocket's retail `0x80000400` for initial ROM placement, generated code enters `game_init` at `0x80000E64` through the recreated retail caller stack (`0x803FFFF0`), SDL/window ownership stays on the main thread, game start is deferred until RT64's first safe VI presentation, and queued graphics tasks use immutable RDRAM snapshots. FIXED27 retains FIXED26's source-frame cadence pin to the known 30 Hz producer whenever interpolation is enabled and carries DKR-R's exact early-present/interpolation framebuffer approval patches so Rocket's alternating color buffers cannot race the asynchronous present queue. The build environment still cannot include the retail ROM or execute the final Windows/WSL/GPU runtime here. User testing has already qualified the interpolation path; for FIXED27 the focused runtime check is **Expand to window at 16:9 and then ultrawide**, watching the old 4:3 side boundaries for any remaining model/segment pop. Original 4:3 remains the regression baseline. If it faults, Windows writes a text crash report and minidump under `%APPDATA%\Rocket-R\logs` (or the directory selected with `--config`).

See `docs/STATUS.md` and `docs/ARCHITECTURE.md`.

## Protected work areas

Do not hand-edit:

- `extern/`
- `runtime-recomp/RecompiledFuncs/`
- `runtime-recomp/RecompiledRSP/`
- `generated/*.generated.hpp`

Changes to translated game behaviour belong in `runtime-recomp/rocket.us.recomp-policy.json`. Dependency changes belong in `dependencies.lock.json` / `patches/manifest.json`.

## Privacy / publishing

The project is intentionally local-only. The ZIP does not contain a Git remote for this project, and the build scripts never create or push a GitHub repository. External dependency repositories naturally have their upstream remotes inside ignored `extern/` checkouts.

## FIXED26 high-refresh presentation: DKR-R-style RT64 interpolation
FIXED26 adds an opt-in frame-rate control without changing Rocket's game clock. The retail scheduler counts 60 Hz VI retraces and submits a new graphics task every two retraces, so the authored presentation rate is 30 Hz. When Match Display or Custom is selected, Rocket-R explicitly reports that 30 Hz source cadence to RT64 and asks RT64 to synthesize the extra presentation frames between authored workloads.

The RT64 patch set now includes DKR-R's proven early-present interpolation fixes: per-workload framebuffer ownership, exact target preapproval before asynchronous rendering, creation of a preapproved target in the workload that first produces it, and an interpolated-presentation counter used for runtime confirmation. Rocket keeps `PresentEarly`, triple buffering, immutable graphics-task snapshots, the FIXED24 independent AI clock and the FIXED25 safe-area/VI crop. **Original 30 FPS remains available at all times and disables interpolation.**

This first Rocket-specific interpolation pass deliberately relies on RT64's standard F3DEX2 transform matching. DKR-R needs extra game-specific identity rules because it translates Rare's custom F3DDKR microcode; Rocket uses standard F3DEX2, so those DKR-only bridge rules are not copied blindly. If a particular Rocket camera cut, particle, HUD element or animation ghosts at high refresh, that should be fixed with a narrow Rocket-specific interpolation identity/exclusion rather than changing simulation timing.

## FIXED27 widescreen visibility: expand Rocket's CPU object frustum
FIXED26 proved that RT64 can render and interpolate the wider Expand-to-window view cleanly, but Rocket still rejected objects using the retail 4:3 CPU frustum before those objects reached RT64. FIXED27 hooks the verified `frustum_test` entry at `0x8003C764` and widens only the camera's two horizontal side planes when **Expand to window** is active. The vertical planes, object culling radii, render-distance/fade logic, simulation and Original 4:3 path are unchanged.

The hook derives the active horizontal FOV from Rocket's own camera fields (`fovY` at `+0xA0`, authored aspect at `+0xA4`) and the current SDL window aspect. It identifies the left/right planes geometrically rather than assuming their array order, then adds the same small **5% edge guard** philosophy used by DKR-R so objects do not flicker exactly on a widened screen edge. A defensive ownership check restores the prior plane values before the next frustum call when they have not already been refreshed by the game, so repeated calls are idempotent and live aspect changes cannot accumulate widening.


## FIXED34 multi-platform packaging + FIXED27 interpolation rollback
FIXED34 keeps the user-qualified FIXED27 gameplay/interpolation code untouched while fixing two platform build blockers: the Android Gradle toolchain uses CMake 3.22.1, so the Rocket-R root now advertises the same compatible minimum; and the Ubuntu 22.04 AppImage container installs `tomli` so the source self-check works on Python 3.10. The launcher also uses a larger UI and requests Comic Sans MS from the host system, with an enlarged built-in fallback when that font is not installed.

FIXED34 includes the FIXED33 RT64 file-dialog EOF correction and adds the Android/Linux build repairs above. No renderer, interpolation, culling, audio or gameplay code has changed from the restored FIXED27 baseline.

FIXED34 is intentionally **not** another interpolation experiment. It removes the FIXED28-31 identity/vertex-interpolation sidecar and returns gameplay rendering to FIXED27, which was the smoothest/most stable user-tested Rocket interpolation revision. Platform support is layered around that baseline.

The One Click Builder can emit four packages from one static-recomp generation: Windows x64 ZIP, Linux x86-64 AppImage, Linux ARM64 AppImage and Android ARM64 APK. Linux packages are built in native-architecture Ubuntu containers; Android cross-compiles with the pinned NDK and SDL2 2.26.3. RT64 receives the Android `ANativeWindow`, has Android-native refresh/window handling, and its build-host shader embedding tools remain host executables during NDK cross-compilation.
