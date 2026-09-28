# Rocket-R Modding Bible

## By ThatGuyMcd

Edition 1.0 • 29 September 2026 • Rocket-R 1.0.1 / API 1

Source baseline: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`. See the validation appendix for evidence and unperformed checks.

## Contents
- [01 — Right, where do we start?](#chapter-01)
- [02 — What can we actually make?](#chapter-02)
- [03 — Getting the tools ready](#chapter-03)
- [04 — Your first working mod](#chapter-04)
- [05 — Understanding the files and the package](#chapter-05)
- [06 — Settings that actually do something](#chapter-06)
- [07 — Events, hooks and original game functions](#chapter-07)
- [08 — Working with game memory without guessing](#chapter-08)
- [09 — Gameplay changes and built-in cheats](#chapter-09)
- [10 — Colours, materials and textures](#chapter-10)
- [11 — Building a menu inside the game](#chapter-11)
- [12 — Creating an RT64 texture pack](#chapter-12)
- [13 — Making mods work together](#chapter-13)
- [14 — Why is my mod doing absolutely nothing?](#chapter-14)
- [15 — Testing and releasing properly](#chapter-15)
- [16 — Reference, recipes and the questions that keep coming up](#chapter-16)
- [Release checklist](#release-checklist)
- [Validation record](#validation)
- [Sources and provenance](#sources)


---

<a id="chapter-01"></a>

# 01 — Right, where do we start?

**Start here.** This chapter explains the system before we start changing it.

Right, let's get something useful working. Start with one small change, get it building, get it loading, and make sure you can actually see it doing something. After that, build on it. Trying to add a whole menu, five gameplay features and a texture replacement at once makes it much harder to work out which bit has gone wrong.

This handbook is for **Rocket-R 1.0.1, Rocket API 1**, checked against repository commit **37b387e38a04673c1ea346fee98cb17b4ed1eba8** on **29 September 2026**. Those details matter. A feature in a newer upstream project does not automatically exist in this Rocket-R build. The dependency lock records the exact versions the host uses. [[S01]](#source-s01) [[S07]](#source-s07)

## What this book is based on

There are three different kinds of evidence throughout this guide. **Source-verified behaviour** comes from the pinned repository and its dependencies. **Reference-implementation behaviour** comes from the supplied Colour Studio, Cheats and standalone Cheat Menu source packages. **Recommendations** are development practices explained here; they are not extra features secretly provided by the loader.

Some public decompilation functions are still assembly placeholders. A function name plus an `INCLUDE_ASM` line is not a full explanation of what that function does. Where a reference mod uses observations from earlier local generated-code inspection, that provenance is kept explicit. This edition does not distribute the original instruction excerpts or pretend the public placeholders prove all those observations. [[R03]](#source-r03)

The reference packages include their original validation notes. Those notes are historical evidence, not a guarantee that a modified copy of the code will work. Fresh checks performed for this documentation are listed separately in `VALIDATION.md`.

## What is Rocket-R doing?

The original game's CPU code is MIPS code. Rocket-R's build uses N64Recomp and RocketRet's matching decompilation to produce the game code used by the host. N64ModernRuntime provides runtime services, SDL handles native input/audio/windows, and RT64 renders the game's F3DEX2 graphics tasks. The original ROM is still supplied by the player; it is not part of a mod. [[S11]](#source-s11)

A normal Rocket-R **code mod** is also compiled to MIPS using the N64 O32 calling convention. The runtime translates that package when it loads it. You are not building a Windows DLL, an Android Java plugin or a C# assembly. The same suitable `.nrm` can be used by the supported x64 and ARM64 hosts, although actual behaviour and performance still need checking on the target devices. [[S01]](#source-s01)

Think of the process as:

```text
Your C source + manifest + matching symbol references
                         |
                    Clang / LLD
                         |
                       mod ELF
                         |
                  RecompModTool
                         |
                Importable .nrm package
                         |
           Rocket-R loads it for a game session
```

**The ELF is a build product, not the file players import.** Equally, a ZIP full of C files is a source distribution, not an executable mod.

## The three package types you will meet

| File | What it is | What players do with it |
| --- | --- | --- |
| `.nrm` | A code/data mod package. Code packages contain a manifest, compiled binary and symbols. | Import it through Add Mods. |
| `.rtz` | An RT64 replacement-texture archive with its database. | Import it as a texture pack. |
| Distribution `.zip` | A bundle containing `.nrm` and/or `.rtz` packages. | Import the bundle, or extract and import its packages. |

The loader also accepts exported profile JSON through the frontend's profile-import route. A profile contains choices and settings, not the packages or saved games themselves. **`.mdm` is not this build's code-mod format.** Renaming an unrelated file to `.nrm` does not convert it. [[S01]](#source-s01) [[S04]](#source-s04) [[S05]](#source-s05)

## Installed does not mean active

There are several separate stages:

1. **Installed:** the package has passed the importer's checks and is in the library.
2. **Selected:** a profile chooses a particular package version and enabled state.
3. **Included in this session:** the launch snapshot contains that package.
4. **Executing:** the callback or hook you rely on actually runs.
5. **Effective:** the code reaches the correct target and produces the intended result.

The first three stages do not prove the last two. This is not just theory: the failed Colour Studio report showed a loaded package, increasing update counters and the selected RGB value, but zero active recoloured materials. The targeting code was the problem. [[D01]](#source-d01)

When the menu says **Enabled next launch**, it is describing the package-selection checkbox. Check the separate running-session message, then use your own diagnostics to establish execution. Do not keep reinstalling the same file solely because that checkbox did not change wording. The bundled camera has a special live-activation path; most code mods do not. [[S05]](#source-s05) [[S06]](#source-s06)

## Choose the smallest route that does the job

Use an existing public event when it supplies the information and timing you need. Use a verified game-specific hook when the public events do not cover your feature. Use a texture pack when the task really is texture replacement. Only move into host development when the feature needs a service the host does not expose.

For example, changing a material on Rocket is different from adding a new native launcher widget. Drawing an in-game menu is different from accessing every other mod's host-managed settings. We have already demonstrated the first sort of menu; it did not automatically solve the second problem. [[R01]](#source-r01) [[R03]](#source-r03) [[S20]](#source-s20)

## What should I read next?

For a first build, read Chapters 2–4, or use the separate `START_HERE.md`. For live options, go to Chapter 6. For colour/material work, go to Chapter 10. For an in-game menu, go to Chapter 11. If the package imports and then does absolutely nothing, go straight to Chapter 14.

Keep Chapter 15's release checklist beside the project. A mod that works once in an empty area is a starting point, not a complete release test.

## Three rules before you touch the game

**Use a test profile and a save you can afford to lose.** Modded profiles have separate save directories, but separation is not a backup and does not make gameplay code harmless. [[S01]](#source-s01) [[S04]](#source-s04)

**Keep a clean baseline.** Save the original source, the package hash, the host version and the exact change you are testing. Otherwise you can spend hours testing a different file from the one you think you installed.

**Make failure visible.** Add a counter or a bounded diagnostic result at each important stage. "It didn't work" tells us almost nothing. "The hook ran 400 times, received blue, found three models and rejected five owned parts" gives us something concrete to fix.



---

<a id="chapter-02"></a>

# 02 — What can we actually make?

**Scope:** the current host and supplied reference implementations, not hypothetical future APIs.

The modding system can do considerably more than the handful of camera functions in the public header. That does not mean every native host feature is available to mods. The useful question is: **which layer owns the feature, and what verified connection do we have to it?**

## Capability map

| Feature | Current position | What is actually needed |
| --- | --- | --- |
| Package a MIPS code mod | Supported directly | Correct manifest, compatible compiled code/symbols and the matching toolchain. |
| Add Number, Enum or String settings | Supported directly | Manifest entries and code that consumes the values. |
| Apply a mod's own settings live | Supported directly, with conditions | Matching running profile/package, host forwarding and a suitable recurring read in the mod. |
| Orbit/first-person camera changes | Public API support | Valid packets, supported camera path and appropriate ownership. |
| Change Rocket's red materials | Demonstrated game-specific implementation | Verified part ownership, formats, safe copies and restoration. |
| Activate built-in cheats | Demonstrated game-specific implementation | Exact native record matching, execution timing and gameplay-state checks. |
| Draw a standalone in-game menu | Demonstrated game-specific implementation | Game rendering/input hooks and safe state/buffer handling. |
| Replace textures using `.rtz` | Supported package path | Real RT64 texture mappings and compatible replacements. |
| Call another cooperating mod | Supported underlying mechanism | Declared dependency and matching exported function/event contract. |
| Add custom native buttons to Details and settings | Not a generic manifest feature | Host UI work, or a separately drawn game-side interface. |
| Capture arbitrary unmapped PC keys in an ordinary guest mod | No public general input service identified | Existing N64 mappings, or additional host support. |
| Read/write every mod's host settings from one guest menu | No general API identified | A host connection for enumeration, schemas, reads, validated writes and persistence. |
| Hot-load/unload arbitrary packages in gameplay | Not provided by this build | More than UI: a safe lifecycle/loader design. |
| Import a new model, level, music track or collision file by dropping it into a package | No automatic importer | Game-specific loading, formats and runtime integration. |
| Ship a native DLL/SO/EXE inside a mod | Rejected by this loader | Not an allowed package route in the audited build. |

The direct support entries come from the modding guide, loader, public API and runtime contracts. The demonstrated entries refer to the specific supplied implementations, not promises that arbitrary hooks or assets are safe. [[S01]](#source-s01) [[S03]](#source-s03) [[S04]](#source-s04) [[S18]](#source-s18) [[S20]](#source-s20) [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)

## Public API versus game-specific code

The public Rocket API exposes four named events: `rocket_on_game_ready`, `rocket_on_camera_update`, `rocket_on_mouse_look` and `rocket_on_first_person_update`. There is no published `rocket_on_frame` or `rocket_on_draw_mod_menu` event in this header. Do not invent one because it sounds like it ought to exist. [[S03]](#source-s03) [[S06]](#source-s06)

A game-specific hook takes a different route. It attaches to a verified function in the original game's symbol set. That can expose a useful update or render boundary, but it also ties your code to that game layout and the host's protection policy. A function being unprotected only removes one obstacle. It does not prove that its arguments, thread, timing or side effects are appropriate.

The standalone Cheat Menu is an example. Its rendering is built inside the mod. It does not import ImGui or add a host settings page. That is a demonstrated technique worth teaching, but it is not a general-purpose game-UI framework supplied by Rocket-R. [[R03]](#source-r03)

## Live settings are not live unloading

It is perfectly reasonable to keep a package loaded for a whole session and put an **Effect: Off/On** setting inside it. When Off, the mod does no new work or restores the particular reversible change it owns. That is different from removing all its hooks, freeing everything other code may reference and unloading its executable code.

In the audited host, the bundled camera's exact package ID/hash is explicitly approved for standby/live switching. The live toggle handler dispatches to the camera's own activation code. A manifest flag is not enough to give another mod the same integration. [[S06]](#source-s06)

A mod disabled before launch is normally not resident. You cannot switch it into action later if its code was never loaded. A mod's internal Off setting can work live precisely because the code is still present to observe the next On setting. These distinctions should also appear in your release description.

## Why a universal settings menu is a separate problem

A guest mod's `recomp_get_config_u32` and related getters are bound to that calling mod's identity. Giving them a key named after another package does not change whose settings they read. The pinned configuration API inspected here does not expose a general catalogue/schema API or a cross-mod setter. [[S20]](#source-s20) [[S28]](#source-s28)

The host library does have validated setting writes, profile persistence and a live forwarding path. Those are host implementation functions. They are not automatically guest-callable imports. To expose them safely would require a defined host interface; a menu's ability to draw does not create that interface. [[S04]](#source-s04) [[S06]](#source-s06)

A cooperative provider can export a value for a consumer. Chapter 13 demonstrates that approach. It is a deliberately shared contract between those packages, **not** a hidden universal host-settings facility. Likewise, changing a cooperative mod's private runtime value is not the same as updating its saved Details and settings value.

## Assets: possible is not automatic

An archive can carry bytes without anything in the game knowing how to use those bytes. New models require appropriate geometry/material loading and ownership. New collision requires the game to recognise the data. New music needs the correct format and playback path. The current documentation explicitly says those general asset importers are not included. [[S01]](#source-s01)

That is a limitation of the supplied workflow, not a proof that nobody can implement one. For an ambitious asset project, first identify the game's existing format and loader, then prove a tiny replacement on a test build. Record which parts are sourced and which remain research. Do not advertise "drop any model in a ZIP" when no loader exists.

## A quick decision before starting a project

Write down the desired effect, the data or function that controls it, the update boundary, how it is disabled, and what happens when a scene unloads. Then answer whether the required service is public, game-specific or host-only.

If any of those answers is "we assume", that is the next experiment. Build a probe, not the final feature. A small successful probe saves far more time than a large menu connected to the wrong thing.

## What is outside the claim of this edition?

This book does not claim general hot-reload, arbitrary native plugins, online mod catalogues, automatic game-asset importers or a generic all-mods settings editor. It does not claim that all raw-ROM functions are hookable, or that every platform has been gameplay-tested with every example. It does show the current routes, the working reference techniques and the missing pieces that future development would need to supply. [[S01]](#source-s01) [[S14]](#source-s14)



---

<a id="chapter-03"></a>

# 03 — Getting the tools ready

**Goal:** prepare the development tools once, then rebuild the mod rather than the entire game every time.

There are two different jobs here. The full Rocket-R build prepares a matching game ELF, generated code and development tools. The mod builder compiles a much smaller project against the prepared references. Players only need a compatible Rocket-R build and the finished package; they do not need your compiler. [[S01]](#source-s01) [[S02]](#source-s02) [[S12]](#source-s12)

## The supported source baseline

The audited host pins these components:

| Component | Revision |
| --- | --- |
| Rocket-R | `37b387e38a04673c1ea346fee98cb17b4ed1eba8` |
| RocketRet decompilation | `cd1fc6d3f575841a240373d7b8a2baf532da2f9f` |
| N64ModernRuntime | `ae1ffbb909d9f93c88c41830deb539f7feef5ed2` |
| N64Recomp | `81213c1831fab2521a6a5459c67b63437d67e253` |
| RT64 | `6f1c2d99a4ea571c139f449c326fd176ba8f3496` |

Use `dependencies.lock.json` from the checkout you are targeting. Do not independently update the dependency folders and then describe the result as the same baseline. [[S07]](#source-s07)

The project guide targets the unmodified US NSUE game. A ROM, generated instruction dump or extracted commercial asset is not included in this handbook. Use your own permitted local game copy in the project's normal preparation process. [[S11]](#source-s11) [[S12]](#source-s12) [[S15]](#source-s15)

## Windows: prepare the checkout

The project's documented entry point is `ONE-CLICK-BUILD.cmd`. Its Windows workflow uses Visual Studio C++ tools, Windows SDK, clang-cl, CMake, Ninja and Python, with WSL Ubuntu for the matching game build. Open a newly installed WSL distribution once and finish its account setup before using the builder. Build logs are under `build/logs`. [[S12]](#source-s12)

For **guest mod compilation**, the modding guide additionally specifies Python 3.11+, a full Clang with MIPS support, LLD, the pinned RecompModTool and a generated Rocket symbol dump. It names Clang 18 on Ubuntu 24.04 as the tested guest compiler. Visual Studio's bundled Clang is not the stated MIPS compiler route. [[S01]](#source-s01)

Useful checks from PowerShell:

```powershell
python --version
wsl --list --verbose
wsl -d Ubuntu-24.04 -- clang --version
wsl -d Ubuntu-24.04 -- clang -print-targets
wsl -d Ubuntu-24.04 -- ld.lld --version
```

The target list must contain MIPS. A compiler that only supports your PC's architecture is not enough. These commands inspect your installation; they do not install or change it.

From a prepared checkout, check the files the guide's example uses:

```powershell
Test-Path .\build\windows\N64ModernRuntime\librecomp\N64Recomp\RecompModTool.exe
Test-Path .\build\generated\rocket.functions.dump.toml
Test-Path .\modding\include\rocket\mod.h
Test-Path .\modding\mod.ld
```

All four should exist for that documented path. A different build directory is fine when supplied explicitly, but do not point `--tool` at a random similarly named executable or replace the symbol file with an address list from another build.

## Build a mod on Windows

From the Rocket-R repository root, the documented pattern is:

```powershell
python scripts/build_mod.py modding/examples/modern-camera --wsl --tool build/windows/N64ModernRuntime/librecomp/N64Recomp/RecompModTool.exe --symbols build/generated/rocket.functions.dump.toml
```

That command builds the included camera example. Do not use its mod ID for your own package. The maintained script has a special branch for `rocket_modern_camera` that generates the bundled camera header. Ordinary mods with unique IDs do not take that branch. [[S02]](#source-s02)

For the lessons in this handbook, keep their folders outside generated or dependency output and pass the project path to the same builder. The included `tools/build_lesson.py` is only a convenience wrapper around that maintained script. It does not replace RecompModTool, create game symbols or edit the host.

```powershell
python tools/build_lesson.py 02-simple-camera --repo D:\Rocket-R --tool D:\Rocket-R\build\windows\N64ModernRuntime\librecomp\N64Recomp\RecompModTool.exe --symbols D:\Rocket-R\build\generated\rocket.functions.dump.toml --wsl
```

Run that wrapper from the extracted handbook folder. `D:\Rocket-R` is an example checkout location: use the actual location on your machine. Put quotes around arguments containing spaces.

The wrapper's default output is `built-mods` inside the handbook folder. The maintained builder's own default is `build/mods` relative to its working directory. Read the path printed at the end rather than guessing where the package went. [[S02]](#source-s02)

## Linux: same project, native tools

Use Python 3.11+, a Clang with MIPS support, LLD and the pinned **native** RecompModTool. Omit `--wsl`; that switch is a Windows-host routing convenience, not a required part of the package format. [[S01]](#source-s01) [[S02]](#source-s02)

```bash
python3 --version
clang --version
clang -print-targets
ld.lld --version
python3 scripts/build_mod.py /path/to/my-mod --tool /path/to/RecompModTool --symbols /path/to/rocket.functions.dump.toml
```

The paths above are placeholders to replace with real prepared tools. This book does not claim the native tool appears at one universal path on every Linux setup. The full project's Linux game-build helpers and the much smaller guest-mod builder are separate workflows. [[S12]](#source-s12)

## What the maintained builder actually does

It reads `mod.toml`, scans **top-level `*.c` files** in the project folder, compiles them, links an ELF, runs RecompModTool and normalises ZIP metadata for stable output. It automatically adds `rocket.json` and `thumb.png` when present. Its generated inputs supply an empty data-reference-symbol list. [[S02]](#source-s02)

That has practical consequences. Put C unit tests in a subfolder so the builder does not try to ship them. A nested `src/` tree is not recursively compiled by this script. Arbitrary extra assets are not automatically packaged just because they sit beside your source. The script appends its own `[inputs]` table, so do not paste a second `[inputs]` into a manifest used by this workflow.

The flags include MIPS II, the 32-bit ABI, `-G0`, freestanding compilation, no builtin assumptions, no system include search, function sections and warnings-as-errors. Those are not decorative options. For example, `-nostdinc` means ordinary desktop headers are not automatically available, and `-ffreestanding` does not magically provide a complete C runtime. Use the supplied types/imports or deliberately provide what your code needs. [[S02]](#source-s02)

The build script compiles C, not arbitrary C++ or C# projects. A different language/toolchain would be a separate development effort with its own compatibility checks.

## Symptoms that tell you which stage failed

| Symptom | Check first |
| --- | --- |
| `tomllib` cannot be imported | Python version; the documented floor is 3.11. |
| No compatible MIPS target | The actual Clang executable used, not just whether Clang is installed. |
| WSL distribution is missing | `--wsl-distro` and the name returned by WSL. |
| No `.c` files found | The source location and top-level scan rule. |
| Undefined or missing import at game launch | Whether the target host registers that import; successful linking is not proof. |
| Reference symbol missing | Symbol name, matching dump, correct target build and whether a size/reference exists. |
| Package built but never changes behaviour | Stop rebuilding tools and follow Chapter 14's runtime checks. |

## This edition's actual test environment

The new lessons were compile/link checked with the available **Clang 17.0.0**, plus native mock tests. That is recorded as local evidence, not a replacement for the project's documented Clang 18 preparation route. The official RecompModTool executable and complete generated Rocket symbol dump were not available in this documentation environment, so the new lessons were **not** packaged through that official path here. The three inherited reference packages were rebuilt using their own included, narrow source-derived packers. See `VALIDATION.md` for the exact distinction.



---

<a id="chapter-04"></a>

# 04 — Your first working mod

**Project:** `examples/02-simple-camera`. **Technique:** public camera events. **Expected effect:** adjustable analogue orbit speed in a supported gameplay view.

We are deliberately starting with the public camera interface rather than arbitrary game addresses. You will get an observable feature with a small amount of code, and the important patterns—unique ID, settings, callbacks, checks and packaging—apply to other projects too. This is a teaching camera, not a replacement for every feature of Modern Analogue Camera.

**Testing status:** the source has passed native mock tests and MIPS compile/link checks. Its final RecompModTool packaging, import and game behaviour must still be verified on your prepared checkout. No new lesson `.nrm` is supplied pretending those steps already happened.

## The project folder

```text
02-simple-camera/
  main.c             The actual feature
  lesson_api.h       Uses the Rocket header; mocks it only in native tests
  mod.toml           Identity and three settings
  rocket.json        API/category/live-settings/resource declarations
  README.md          Purpose, build route and validation boundary
```

The source uses the `rocket/mod.h` in your checkout during a normal build. The separate header snapshot under the handbook tools exists only to make the local compiler checks reproducible. It is not a reason to stop using the matching project's public header. [[S03]](#source-s03)

## Start with an identity of your own

The lesson ID is `bible_simple_camera`; change it before releasing your own derivative. Display names can be friendly, but the ID is a stable machine-readable key. The host uses it for profiles, dependencies and runtime configuration. Keep it lowercase with letters, numbers, underscores or hyphens. [[S04]](#source-s04)

The manifest contains:

```toml
[manifest]
id = "bible_simple_camera"
version = "1.0.0"
display_name = "Bible: Simple Camera"
authors = ["ThatGuyMcd"]
game_id = "rocket"
minimum_recomp_version = "1.0.1"
dependencies = []
```

The actual included file also supplies descriptions and the options. Do not replace that complete file with this short excerpt unless you deliberately intend to remove those fields.

## Three settings, each with a job

**Camera effect** is an Off/On enum. Off makes the example stop requesting a camera change; it is not a request to unload its code. **Camera speed** is a Number from 0.5 to 5.0. **Invert vertical look** is another Off/On enum.

The strings in an Enum's `options` array are what the frontend stores and displays. The `recomp_get_config_u32` getter returns its zero-based selection index to the mod. With `['Off', 'On']`, Off is 0 and On is 1. Keep the declaration and code consistent. [[S04]](#source-s04) [[S20]](#source-s20)

## Claim the supported camera path

At game-ready time the lesson calls the public claim function:

```c
ROCKET_CALLBACK(rocket_on_game_ready)
void bible_camera_ready(void) {
    rocket_claim_analogue_camera();
}
```

The game-ready event runs before the game entrypoint, after mod code and runtime services are ready. It is not a guarantee that a save has been selected or a player object exists. This example only requests the supported camera path there; it does not dereference a player pointer. [[S01]](#source-s01) [[S30]](#source-s30)

## Use the packet while it is valid

The camera callback receives a temporary `RocketCamera` packet. Check the pointer, API version and size, read the current settings, validate the inputs, write the three output values and set `apply`. Do not store the packet pointer and use it next frame. [[S03]](#source-s03)

The essential calculation is:

```c
p->output_yaw = p->yaw + yaw_delta;
p->output_pitch = bounded(p->pitch + pitch_delta,
                          -0.30f, 1.10f, 0.0f);
p->output_distance = p->distance;
p->apply = 1u;
```

`yaw_delta` and `pitch_delta` use the packet's delta time. They are not increments per monitor refresh. Recenter is treated separately so the supplied behind-Rocket heading is not immediately altered by a simultaneous stick input.

The complete `main.c` also checks for implausible or non-finite values, bounds the configured speed, preserves distance and declines to write when the effect is Off. Copy the **whole file** for the tutorial, not just these four lines.

The host still performs its original camera obstacle handling after an accepted request. The lesson is not replacing collision detection or writing arbitrary camera memory. [[S01]](#source-s01) [[S06]](#source-s06)

## Declare the resource conflict

The lesson's metadata includes:

```json
{
  "schema": 1,
  "api": 1,
  "category": "Tutorial",
  "live_settings": true,
  "exclusive_resources": ["camera.orbit"]
}
```

Disable Modern Analogue Camera and any other package declaring that orbit resource before launching the example. The host should reject incompatible selections rather than let two packages compete for the same camera. Merely selecting the lesson's internal Off option does not remove its package-level resource declaration. [[S04]](#source-s04)

## Build, import and check the actual package

Use the command in Chapter 3 with `02-simple-camera`, or the maintained `build_mod.py` with the full path to that folder. Confirm that the printed output path is an `.nrm` file and that the build completed without an error.

Run the optional package preflight:

```text
python tools/check_package.py built-mods/bible_simple_camera.nrm
```

This checks archive structure, not hook resolution, native camera behaviour or full importer equivalence. Then import the final file into a test profile through **Mods → Installed → Add Mods**, enable it, close/restart Rocket-R to load the new package, and enter a save. Installing new code is not a hot-reload operation. [[S01]](#source-s01)

## What you should see

In a supported third-person gameplay view, horizontal and vertical mapped look inputs should request an orbit. Changing **Camera speed** from 0.5 to 5.0 should produce an obvious difference. Inverting vertical look should reverse its direction. Turning **Camera effect** Off should stop this lesson requesting an override; the host's supported fallback behaviour remains in charge.

These are **acceptance checks to perform**, not a claim that the lesson has already passed a real game session. It deliberately omits mouse-look event integration, first-person handling, momentum and the bundled camera's complete input experience. Use the included camera source and API reference when adding those features. [[S03]](#source-s03) [[S26]](#source-s26)

## Verify live changes properly

Make sure Details and settings identifies the matching running profile/package and says options apply immediately. Change speed while playing, leave the overlay and test the camera. Close the game normally, relaunch and check that the chosen value is still present.

Now deliberately select a different profile while the first is running and edit its setting. The first session should **not** change. That is expected isolation, not a broken slider. The host's forwarding checks require the active profile and package hash to match. [[S04]](#source-s04) [[S10]](#source-s10)

## A smaller probe when even this is too much

`examples/01-ready-probe` only increments a diagnostic counter and records a startup setting on `rocket_on_game_ready`. It does not draw a message or change the game. Use it when learning callback registration or inspecting package structure. Do not expect a visible result from it.

Changing its startup setting after the event has run will not update its cached value. That makes it a useful comparison with the camera lesson, which rereads values on every appropriate callback. Both behaviours are intentional and documented.

## Build on it without making debugging miserable

Change one thing. Rebuild with a new development version. Import that file. Check which version is running. Test the intended effect and restoration. Only then add the next feature.

Keep your build log, package hash and test notes. When something breaks, being able to say "the callback still runs and the speed getter still changes" is far more useful than starting again from a blank project.



---

<a id="chapter-05"></a>

# 05 — Understanding the files and the package

**Reference:** the maintained builder, RecompModTool and Rocket-R's importer. [[S02]](#source-s02) [[S04]](#source-s04) [[S17]](#source-s17)

There are two sets of files: the editable project and the package produced from it. Keep that distinction clear. Editing a manifest inside the installed library is not the same as making a new, versioned mod release.

## Source manifest: `mod.toml`

The maintained workflow reads a TOML `[manifest]` table. RecompModTool turns it into the `mod.json` in the finished archive. The source uses `[[manifest.config_options]]`; the package exposes those options through `config_schema.options`. They are corresponding parts of the pipeline, not two unrelated settings systems. [[S02]](#source-s02) [[S17]](#source-s17) [[S26]](#source-s26)

| Field | Purpose and practice |
| --- | --- |
| `id` | Stable unique package ID; lowercase letters/numbers/underscore/hyphen. Do not copy another mod's identity. |
| `version` | This mod's version. Increase it for releases so users can identify what they are running. |
| `display_name` | The friendly name in the frontend. Renaming this does not require changing the ID. |
| `short_description` | A short explanation of the feature, not the complete manual. |
| `description` | Behaviour, important limits, controls and anything the user needs before activating it. |
| `authors` | An array of author names. Preserve relevant credits. |
| `game_id` | Must be `rocket` for this host. |
| `minimum_recomp_version` | Minimum Rocket-R version, not the mod version, API number or upstream library version. |
| `dependencies` | Required packages, optionally with a minimum version such as `provider:1.0.0`. |
| `optional_dependencies` | Underlying optional relationship declaration; code must handle absence correctly. |

For the simplest release numbering, use plain `major.minor.patch` values such as `1.0.1`. The host and upstream producer both validate version syntax, but that does not justify assuming every labelled-version ordering edge case behaves like a package manager you have used elsewhere. Keep compatibility requirements explicit and test upgrades. [[S04]](#source-s04) [[S17]](#source-s17)

## Rocket-specific metadata: `rocket.json`

```json
{
  "schema": 1,
  "api": 1,
  "category": "Gameplay",
  "live_settings": true,
  "conflicts": [],
  "exclusive_resources": []
}
```

`schema` is the metadata format version. `api` expresses the Rocket API requirement. `category` affects the displayed kind. `live_settings` says that the mod implements live option application. `conflicts` names known incompatible mods; `exclusive_resources` declares resources that must not have two owners in the selected profile. The loader also understands an `adventure` declaration for mutually exclusive adventure packages. These declarations help resolution; they are not an automatic conflict detector for all memory writes. [[S04]](#source-s04)

Do not add `live_toggle: true` and assume you have obtained the camera's activation switch. The camera's approval and handler live in host code, and the public metadata path is not a substitute for them. [[S06]](#source-s06)

## What belongs in a finished code package?

The core files are:

```text
my_mod.nrm
  mod.json
  mod_syms.bin
  mod_binary.bin
  rocket.json          optional Rocket metadata
  thumb.png            optional thumbnail supplied by the builder
```

The importer requires code and symbols to appear together. A manifest-only `.nrm` can be classified as data, but that does not provide executable behaviour or a universal asset loader. A package successfully importing does not establish that every import, hook and native call will resolve at game launch. [[S04]](#source-s04) [[S28]](#source-s28)

`mod_binary.bin` contains the guest code/data. `mod_syms.bin` describes sections, functions, references, imports, callbacks, hooks and other information used to load/recompile it. Do not edit either by hand to "fix" a stale build. Rebuild from source and inspect the resulting package.

## Build output is not the installed library

The host stores installed immutable content under `mod-library`, addressed by its SHA-256. Profiles choose a hash for each mod ID. At launch it verifies the selected package again and builds a separate runtime directory under `mod-runs/run_...`, with the chosen `.nrm` packages, per-mod configuration and a launch snapshot. [[S04]](#source-s04)

A runtime package can be hard-linked to the library package, falling back to copying where needed. Editing a supposedly temporary runtime copy may therefore affect the installed file too. **Treat both as managed output.** Edit the project, rebuild and re-import. [[S04]](#source-s04)

When an existing mod ID is imported with a new package, the selected profile's hash is updated while its stored settings and enabled state are retained. A different ID is a different mod; it does not automatically replace the previous one. This is why a renamed feature can accidentally leave two similar packages selected together. [[S04]](#source-s04) [[S10]](#source-s10)

## Actual import limits

These are the audited loader limits, not recommended budgets to fill:

| Item | Limit/check in this host |
| --- | --- |
| Archive size | 256 MiB |
| Total expanded contents | 512 MiB |
| Individual archive entry | At most 256 MiB |
| Archive entries | Non-empty; at most 16,384 |
| Metadata object | 256 KiB read limit |
| Code symbol file | 16 MiB read limit |
| Code binary file | 32 MiB read limit |
| Setting definitions | At most 128 options |
| Required/optional dependency arrays | At most 128 entries in each |
| Mod/option ID | At most 96 characters, restricted alphabet |
| Display name/individual author name | At most 160 UTF-8 bytes |
| Description fields | At most 32,768 UTF-8 bytes |
| String option value | At most 4,096 bytes as stored in the validated string |
| Distribution ZIP | At most 32 embedded mod packages |

Paths must stay inside the archive. Absolute paths, `.`/`..` components, backslashes in entry names, colons, control characters, symlinks and case-colliding duplicate filenames are rejected. Archive names also have a length limit. Native `.dll`, `.so`, `.exe` and `.dylib` payloads, or non-empty native-library declarations, are rejected. [[S04]](#source-s04)

These checks are not a sandbox. Code mods execute in the game process. A valid ZIP and a matching checksum do not prove the author trustworthy or the code safe. [[S01]](#source-s01)

## Dependencies and resource ownership

Dependencies are resolved before the packages that need them. A missing required dependency, dependency cycle, incompatible minimum version or explicitly disabled required mod prevents a valid launch plan. Two selected versions of the same required identity can also conflict. [[S04]](#source-s04) [[S10]](#source-s10)

Declare a conflict because you know there is a real incompatibility. Do not claim broad resources unnecessarily. Equally, do not remove the declaration merely to make the resolver stop warning: if both packages write the same camera state, removing the warning does not solve the conflict.

A provider required by another running package cannot simply be switched off through the camera-style standby mechanism. The runtime plan disables live-toggle capability for required dependencies in that path. This is one example of why package lifecycle is more than a checkbox. [[S04]](#source-s04)

## Reproducible packages

The maintained mod builder rewrites ZIP entries in stable name order with fixed ZIP timestamps. This avoids a build-time timestamp changing the package hash when the actual payload is unchanged. It does not guarantee reproducibility across arbitrary compiler/tool versions, settings or source changes. [[S02]](#source-s02)

Record the source revision, compiler, linker, RecompModTool revision, host baseline and final SHA-256. A second identical build is useful evidence. It is still separate from verifying an import and the effect in game.

The inherited reference mods use their own narrow source-derived N64RSYMS v1 writers. Those writers have deliberate restrictions on section layouts, payload sizes and relocation types. They are included as part of those exact projects, **not presented as a general replacement for the maintained RecompModTool workflow**. [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)

## Before sharing a package

Open the archive and check its contents. Confirm the version, ID and author. Check that code/symbols are paired, metadata is valid, and no source-only build was mistaken for the release. Run the actual importer and launch the game. Then test the feature with the expected settings.

The supplied `tools/check_package.py` provides an extra conservative preflight. It explicitly does not claim to reproduce the full host validator or the runtime's import/hook resolution. A passing result is one step, not a release certificate.



---

<a id="chapter-06"></a>

# 06 — Settings that actually do something

**Goal:** make options understandable, live when appropriate, and safe across restarts.

A slider existing in the frontend is not proof that the mod uses it. A mod has to read the value at an appropriate time and apply it to the intended feature. Start there before assuming the UI is broken.

## The complete settings path

```text
mod.toml config_options
        -> package mod.json config_schema.options
        -> selected profile's stored values
        -> launch-time runtime configuration
        -> this mod's recomp_get_config_* imports
        -> your feature logic
```

During gameplay, the host validates a changed option, persists the profile value and forwards the corresponding typed update when the selected profile/package matches the active session. The mod must then read that updated value. [[S02]](#source-s02) [[S04]](#source-s04) [[S06]](#source-s06) [[S20]](#source-s20)

## The three option types

### Number

```toml
[[manifest.config_options]]
id = "strength"
name = "Effect strength"
description = "0 leaves the original result; 100 uses the full effect."
type = "Number"
min = 0.0
max = 100.0
step = 1.0
default = 100.0
```

Use `recomp_get_config_double("strength")` for a continuous value. Clamp and validate at the point of use as well. The current host displays Number as a floating-point slider with two decimal places; the UI implementation does not automatically enforce the manifest's `step` as a slider increment. Round in your mod when the feature needs discrete integer steps. [[S05]](#source-s05) [[S20]](#source-s20)

For a value intended to be 0–255, define that range in the manifest, reject non-finite values in your logic, round or deliberately truncate, and document which you chose. Do not let a compiler's incidental cast decide what the UI means.

### Enum

```toml
[[manifest.config_options]]
id = "mode"
name = "Colour mode"
type = "Enum"
options = ["Original", "Custom RGB", "Hue Shift"]
default = "Original"
```

The profile value is the selected string. The runtime converts that selection to a zero-based integer for `recomp_get_config_u32`. Here Original is 0, Custom RGB is 1 and Hue Shift is 2. The code and schema must agree. [[S04]](#source-s04) [[S20]](#source-s20)

An Off/On setting is an Enum with two choices in this schema. There is no separate public Boolean or Action type in the audited frontend. An enum named On/Off is still rendered as a dropdown, not automatically as a coloured button. [[S05]](#source-s05)

### String

```toml
[[manifest.config_options]]
id = "label"
name = "Label"
type = "String"
default = "My mod"
```

The host String widget commits on Enter. Its validated string limit is 4,096 bytes; your own display/format limits may need to be much lower. The public Rocket convenience header only declares the numeric getters, but the pinned runtime also registers `recomp_get_config_string` and `recomp_free_config_string`. That is an audited runtime import, not an invented host function. [[S03]](#source-s03) [[S05]](#source-s05) [[S20]](#source-s20)

The string getter returns allocated guest memory. Copy only as much as your own bounded buffer can hold, keep it terminated, and free the returned allocation using the matching function. Do not retain a stale pointer indefinitely or request a fresh allocated string every frame for an unchanged label. [[S20]](#source-s20)

A declaration pattern for the audited runtime is:

```c
ROCKET_IMPORT char *recomp_get_config_string(const char *key) {
    (void)key;
    return 0;
}
ROCKET_IMPORT void recomp_free_config_string(char *value) {
    (void)value;
}
```

These are import placeholders for the MIPS build. They are not native implementations and are not a substitute for runtime resolution. A full feature should use a deliberate refresh point or change trigger for expensive text handling.

## Live updates: where to read the setting

For a camera effect, read cheap numeric settings during the appropriate camera callback. For a material effect, use its verified material-update boundary. For a one-time startup option, reading only at game-ready is intentional. Choose according to behaviour, not because it is easiest to put everything in the first callback you found.

The `live_settings` metadata informs the user that you support immediate application. It does not arrange your recurring callbacks, update cached local variables or call your feature automatically. [[S01]](#source-s01)

The current host forwards ordinary live writes only when the runtime handler is ready, the active profile is the selected profile, the package hash matches, and the selected entry remains enabled (or has the special resident live-toggle capability). Unticking **Enabled next launch** can therefore stop future live option forwarding for an ordinary still-loaded package. Leave the package selected and use an internal effect setting when that is the behaviour you want. [[S04]](#source-s04) [[S10]](#source-s10)

## Separate a preference from a command

**A preference** describes the desired ongoing state: speed, colour mode, intensity, or a reversible effect being on.

**A command** requests one occurrence: refill health, move to a level, reset a set of cheats, or run a test action.

Do not implement a command as `if (setting) perform_action();` in a recurring hook. That fires every update while the option remains selected. It can also unexpectedly repeat at startup from a saved selection.

A safer command pattern stores the previous observed selection, establishes a baseline before accepting commands, and reacts only to a deliberate transition. It rechecks gameplay readiness immediately before executing. After leaving gameplay it discards pending commands; it does not queue a dangerous action for an unrelated later scene. These are design rules demonstrated by the reference Cheats projects, not built-in Action semantics in the host. [[R02]](#source-r02) [[R03]](#source-r03)

```c
/* Design pattern, not a Rocket API callback declaration. */
if (!initialised) {
    previous = current;
    initialised = 1;
    return;                 /* Do not replay saved commands. */
}
if (current != previous) {
    previous = current;
    if (current == RUN && gameplay_is_ready()) {
        perform_once();
    }
}
```

A generic code example cannot supply `gameplay_is_ready()` for every possible feature. Chapter 11 explains the specific predicate inherited by the supplied menu and its evidence limitations.

## An Off setting needs a real definition

For Colour Studio, Off/Original restores the original material calls. For a reversible cheat, Off removes its verified effect. For a health refill, there is no meaningful "undo" that can restore an earlier health value without introducing new behaviour. For package unloading, an internal Off preference is not sufficient.

Write the meaning in the option description. "Off stops accepting requests" and "Off disables every active cheat" are different actions. The feature-only Cheats reference deliberately uses the original global reset for its master Off behaviour and warns that progress or level changes are not undone. [[R01]](#source-r01) [[R02]](#source-r02)

If you keep executing hooks while Off so the user can turn the feature back on, make that path cheap. It should not continue scanning the level or allocating buffers just to discover that the effect is disabled.

## Defaults and upgrades

At launch, the host starts with schema defaults and overlays saved values that match declared option IDs. Existing matching values are validated against the new schema. They are not silently migrated to a different enum label or clamped to a newly narrowed range. A saved invalid choice or out-of-range number can block launch. [[S04]](#source-s04)

Keep IDs stable. Keep established enum labels when possible. Add new options with safe defaults. When changing enum order, update the code and test old profiles: the saved string is converted according to the current schema, so code with the old positional assumptions can misinterpret it.

A display-label edit is not a reason to change the saved ID. A package version change is not automatic settings migration. Test the actual old profile as part of every schema change.

## Presets without losing the user's custom colour

Use one mode selector and separate custom RGB settings. A preset should compute a target colour without overwriting the user's RGB preferences. Returning to Custom RGB can then recover exactly what they entered. Hue, saturation, brightness and strength should have documented neutral values and ordering. [[R01]](#source-r01)

For example, say whether hue shift applies to the preset colour, to the original source colour, or both depending on mode. Say whether strength blends before or after brightness. There is no universal correct choice, but an unexplained order makes the controls feel inconsistent.

## Test both directions of the lifecycle

Change a setting while playing. Check the feature. Restart and check persistence. Change profile/version during the session and confirm isolation. Test Off, On, minimum, maximum, defaults, invalid test values and restoration. Check that a saved command does not execute just because you reopened the game.

Finally, inspect the description as a user. Could somebody who has never seen the source understand what the option does and whether it needs a restart? If not, rewrite it.



---

<a id="chapter-07"></a>

# 07 — Events, hooks and original game functions

**This is where timing starts to matter.** A correct calculation at the wrong point in the frame can still do nothing—or break something unrelated.

## Prefer the public event when it fits

`ROCKET_CALLBACK(event)` places a retained function into a special callback section. The public header constructs the base-host event name using the `*` dependency marker. `ROCKET_IMPORT` declares a placeholder in a special import section. The package tool and runtime give those sections meaning; they are not ordinary operating-system plugins. [[S03]](#source-s03) [[S18]](#source-s18)

| Event | When it is useful | Important boundary |
| --- | --- | --- |
| `rocket_on_game_ready` | Mod/runtime setup before the game entrypoint | Not "a save is loaded" and not a per-frame callback. |
| `rocket_on_camera_update` | Supported orbit camera updates | Temporary 56-byte packet; return without retaining it. |
| `rocket_on_mouse_look` | Angular mouse deltas before the camera update | Requires opt-in and appropriate ownership; no arbitrary keyboard catalogue. |
| `rocket_on_first_person_update` | Original first-person input update | Separate 48-byte packet and opt-in; do not reinterpret an orbit packet. |

Only write the output fields and `apply` described by each camera contract. Inputs and timing are read-only. Angles are radians and Rocket's vertical axis is Z. The current host validates results and preserves its original obstacle-handling path. [[S01]](#source-s01) [[S03]](#source-s03)

## Entry hook, return hook or replacement?

An **entry hook** observes a verified game function before its body runs. A **return hook** runs after it finishes. A **replacement** takes responsibility for implementing the target function. They have very different risk profiles.

For a teaching hook on the known cheat update, the underlying section form is:

```c
#define LESSON_HOOK_RETURN(name) \
    __attribute__((used, retain, \
                   section(".recomp_hook_return." name)))

LESSON_HOOK_RETURN("func_8004ED04")
void my_after_cheat_update(void) {
    /* Bounded work appropriate to this verified boundary. */
}
```

This macro is a local convenience shown here. It is **not** named `ROCKET_HOOK_RETURN` in the public Rocket header. The underlying `.recomp_hook_return.` section prefix is part of the pinned N64Recomp contract. The final symbol dump and package tool still have to resolve the target. [[S18]](#source-s18)

The inherited mod projects use narrow custom builders that explicitly emit their hook tables. Their source therefore uses `KEEP` on the hook functions rather than necessarily containing the maintained tool's hook section annotation. Do not move one of those C files into the official builder and assume a retained function automatically registers itself as a hook. Read its `src/build.py` and the emitted hook list. [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)

## What a hook can and cannot change

The pinned runtime saves the interrupted CPU context before running hooks and restores it after each hook. That protects the original registers from incidental changes. Persistent writes to valid game memory are a separate matter: restoring CPU registers does not undo them. [[S19]](#source-s19)

Consequently, changing a normal C argument or returning a different value from an ordinary observational hook is not a supported way to replace the original function's arguments or result. A return hook also must not assume the original argument registers still contain the entry arguments.

The runtime exports return-value accessors such as `recomphook_get_return_u32`. Use an audited accessor for an original return value, not a guess about whichever register the compiler happened to leave visible. An integer/pointer access example needs its correct import declaration, original result type and lifetime checks. The existence of several accessor names is not a reason to skip auditing the exact one you use. [[S19]](#source-s19)

If you genuinely need to pair entry/return information, design it for nested calls and reentrancy. A single global "current object" can be overwritten by another invocation before the first returns. A bounded stack or ownership-specific record may be appropriate; verify the actual call/thread pattern first.

## The protection inventory

Rocket-R generates `generated/mod_protection.generated.hpp` from the checked recomp policy and ELF. The generator collects functions named by `functionHooks`, `instructionPatches` and `stubs`. The runtime registers their addresses as protected and rejects raw-ROM hook/replacement paths that would bypass the host's built-in changes. [[S08]](#source-s08) [[S09]](#source-s09)

Do not patch the protection check out. Do not use a force-replacement label as a promise that host protections disappear. Those checks preserve such things as interpolation, rendering and camera fixes.

The included helper performs the same **name collection**, not a full hookability proof:

```text
python tools/check_hook.py --repo D:/Rocket-R func_80075ABC get_controller_data
```

A result saying NOT LISTED means only that the named function was absent from those policy collections in that checkout. Next verify that the intended host was built from that policy, that the symbol exists with a usable size, and that the boundary is actually safe for your task.

## Finding a useful target

Start from the visible effect and work backwards. Find the relevant game structure or producer. Follow its callers. Identify whether the work happens during simulation, display-list construction, final HUD drawing, save selection or loading.

For the supplied standalone menu, the game source shows `func_80075ABC()` being called before `schedule_gfx_task()` in `func_8007F22C`. The reference attaches a return hook to that former function rather than hooking the protected task-submission function. That establishes a specific useful ordering relationship; it is not a blanket rule that every HUD-looking function is safe. [[S23]](#source-s23) [[R03]](#source-r03)

For the cheat mod, the original game loop calls `func_8004ED04` after the render/update work. The mod marks an exact native record as complete at its entry and checks the native processing at return. It does not replay the code sequence as player controls. [[S29]](#source-s29) [[R02]](#source-r02)

## Calling a native game function

A direct call needs a correct symbol reference and ABI-compatible declaration. An address copied from a log is not a complete declaration. Verify argument count, argument types, return type, required state and side effects.

The supplied Cheats project calls the verified `func_8004EE84` helper through a mapped reference named `native_cheat_enable`. Its builder associates that alias with the exact function and section. That alias is part of this custom build arrangement; it is not automatically in every Rocket symbol dump. [[R02]](#source-r02)

A maintained-tool project should use the matching real symbol name or deliberately provide its own verified reference configuration. Do not leave an unexplained alias unresolved. The maintained linker permits unresolved symbols for the later packaging/reference-resolution process, which is precisely why successful linking alone does not validate a native call. [[S02]](#source-s02) [[S17]](#source-s17)

## Be cautious with replacements

A replacement can discard behaviour you did not know existed. A function may update ownership, initialise scratch state, call another subsystem or maintain an invariant needed later. Returning the same value is not proof that you preserved its contract.

Prefer a small hook that modifies only the intended data at a known boundary. When a replacement is necessary, document the original contract, which host changes must remain, and how the replacement is tested. Protected targets still require the project's host-development route rather than an ordinary mod workaround. [[S09]](#source-s09) [[S13]](#source-s13)

## Hook ordering and cooperation

The pinned runtime sorts entry hooks according to mod order and return hooks in reverse order. That helps form nested behaviour, but it does not automatically resolve two mods editing the same pointer. Treat write ownership and restoration as an explicit agreement. [[S19]](#source-s19)

A conservative restore should verify that the field still points to the value your mod installed. If another system has legitimately replaced it, blindly writing your saved old pointer back can undo the other system or restore freed scene data. This is a design rule, not something the loader performs on your behalf.

## Evidence to keep beside every hook

Record the source file, function name, symbol address/section, original signature, update ordering, protection-policy check, permitted game states and the reason this hook is needed. Add the failure case to tests when you find one. Future-you should not have to reverse-engineer the same decision from a magic number in a macro.



---

<a id="chapter-08"></a>

# 08 — Working with game memory without guessing

**Rule one: a plausible address is not an ownership check.**

Game-specific mods often need to inspect original structures. This is powerful, but it is also where a small incorrect assumption can make a mod do nothing, corrupt an object or fail only during a scene change. Keep the scope narrow and the evidence explicit.

## Guest pointers and host pointers are different

Code in an `.nrm` is compiled for the guest's 32-bit environment. A pointer in that C code is a guest pointer interpreted by the runtime. Native host code, including a diagnostic reader, works with real host addresses and the runtime's mapped guest-memory layout. Copying a native pointer into a guest field is not valid translation. [[S03]](#source-s03) [[S11]](#source-s11) [[S28]](#source-s28)

Likewise, an original ROM offset, a CPU virtual address and an RSP segmented address are not interchangeable just because they are all printed in hexadecimal. Name them accordingly in your code: `rom_offset`, `guest_address`, `segment_offset`, `host_buffer`. That simple habit prevents a lot of mistakes.

The old diagnostic collectors explicitly distinguished the loaded guest address from the omitted host allocation address. Their process-read logic is not code to paste into a portable MIPS mod. [[D01]](#source-d01)

## Byte order and typed access

The original game uses big-endian guest data. Runtime/native memory helpers account for the representation used by the host. Guest compiled byte/halfword/word accesses and a native process reader cannot be assumed to use identical raw byte indexing. The inherited reference mods test their packaged MIPS instructions separately from their native-memory fixtures for this reason. [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)

A palette colour in RGBA16 is not a 32-bit desktop RGBA value. An indexed texture's image bytes are indices, not the colours themselves. Read the documented structure or format before trying to paint every byte that looks red.

## Validate ranges without overflowing

The reference code uses subtraction-based range checks: after checking the address is within the region, compare the requested size to the remaining space. That avoids an unchecked `address + size` wrapping around before comparison. [[R02]](#source-r02)

```c
/* Pattern for a known region, not a universal valid-object test. */
static unsigned int within(unsigned int address,
                           unsigned int bytes,
                           unsigned int first,
                           unsigned int end) {
    return address >= first && address < end
        && bytes <= end - address;
}
```

This establishes only a range. It does not establish that the pointer refers to a live object, the right object type, the right scene or storage your mod owns. Apply alignment, count, structure and ownership checks separately.

## Structure layouts need proof

The game's `GameObject` declaration contains useful fields such as its own display-list pointer, submodel pointer and count. Some later player-specific fields are not self-documenting. A field named `unk230` is not an invitation to invent a meaning. [[S24]](#source-s24)

The Colour Studio failure is the useful cautionary example. An assumed child index caused valid player parts to be rejected. The live report showed parent links back to Rocket, but a supposed index contained values that did not match the assumed slot numbers. The later implementation stopped requiring that invented index and used the verified links. [[D01]](#source-d01) [[R01]](#source-r01)

Do not "fix" an ownership check by deleting all validation. Replace the invalid assumption with evidence appropriate to the actual object relationships. Record whether a link is direct or through another known player part.

## Root objects are not necessarily rendered objects

A player object can own several visual parts without having its own traversable display list. The diagnostic report recorded exactly that: the player root had a null display-list pointer, while child references carried the visible material lists. Searching only the root was not enough. [[D01]](#source-d01)

That does not justify scanning the entire level for every red palette. Walk the verified ownership graph, bound the traversal, detect cycles and deduplicate references. A cosmetic mod for Rocket should not repaint unrelated scenery merely because a colour matches.

## Lifetime is part of the pointer's meaning

A pointer can be in range and still refer to an object from a previous scene. A temporary allocation can remain numerically unchanged while its contents are reused. A saved source pointer can become invalid after a reload. Treat scene identity, allocation generation and object ownership as part of your cache key.

The supplied material implementation tracks relevant pool/scene changes and discards private bookkeeping at appropriate transitions. That is an implementation-specific strategy, not a universal heap API. If a future host changes allocation policy, revalidate it. [[R01]](#source-r01)

For a temporary camera packet, the rule is simpler: do not retain it after the callback returns. For a string obtained from the runtime getter, follow its allocation/free contract. For a game's graphics buffer, follow its task lifetime. Different pointers need different rules. [[S03]](#source-s03) [[S20]](#source-s20) [[S11]](#source-s11)

## Shared source data versus private replacement data

Two objects can share a material or texture. Editing the source in place may affect both. The Colour Studio design instead makes private material/colour-data copies for the intended owned parts and redirects their calls while preserving the shared original. [[R01]](#source-r01)

Keep enough information to restore the exact call or field you changed. Rebuild the recoloured result from the original source, not from yesterday's recoloured copy. Otherwise changing a slider can progressively damage colours, lose saturation, or prevent a proper return to Original.

When allocating, bound the number and size of copies. Reuse valid owned buffers when possible. Recolouring should not allocate another copy every frame forever. Tests should check stable allocation counts across repeated slider changes, not merely the first successful colour.

## Renderer-visible memory is not every byte of guest memory

Rocket-R's architecture describes an immutable **8 MiB RDRAM snapshot** attached to graphics tasks. Mod code itself can live in extended guest memory, but that does not imply a display list or texture pointer into any such memory is available in the task snapshot. [[S11]](#source-s11)

The standalone menu addresses this by copying its generated drawing resources into bounded task-visible game storage and reserving space for the original footer. This is why its renderer does more than point a display-list call at a static array in the mod's extended memory. [[R03]](#source-r03)

The practical rule is to prove where the renderer reads from and when the snapshot happens. A perfectly valid CPU pointer can still be the wrong place for graphics data.

## Avoid accidental interaction with interpolation

A shared model pointer is not always a stable identity for an individual draw. Rocket-R's presentation code uses verified owners, callsites and roles to distinguish objects and attachments. Do not assume that editing arbitrary render-entry identities or replacing protected render functions is harmless. [[S11]](#source-s11) [[S13]](#source-s13)

For a colour-only feature, leave matrices, geometry order and ownership metadata alone. The narrowest correct change is usually the easiest to keep compatible.

## Native test fixtures have limits

A fixture should independently model the layout or failure you are testing. If both your implementation and test invent the same child index, the test will pass while the real game fails. Keep realistic invalid values, scene transitions, zero-count cases, aliased materials and reused storage in the suite.

Native sanitizers can detect mistakes in the native test's memory use. They do not prove the guest address belongs to Rocket in a live game. A bounded MIPS interpreter tests the emitted instructions against its fixture; it is not RT64, the host's live recompiler or the original gameplay session. State those limits rather than treating a large assertion count as a substitute for observation. [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)



---

<a id="chapter-09"></a>

# 09 — Gameplay changes and built-in cheats

**Case study:** `reference_projects/Rocket_Cheats_1.0.0`. This is the feature-only package; it contains no drawn menu or menu hotkey.

When the original game already has a feature, it is worth understanding that implementation before recreating it. The Cheats mod uses the native cheat system rather than inventing replacement physics for every code. That keeps the operation tied to the original handler—but the request still needs proper validation and timing. [[R02]](#source-r02)

## What the reference actually does

The project stores the known input sequences as identifiers for the original cheat records. It looks for exact matching records, validates their structure and callback addresses, and verifies a known reset-handler sentinel. It then marks one chosen record's sequence progress complete for the original dispatcher to process. It does **not** press a string of buttons on the player's behalf, and it does not replace the record's callback pointer or argument. [[R02]](#source-r02)

The entry/return hook is on `func_8004ED04`. Public game source confirms that the main game loop calls that function, while the decompilation file still contains a placeholder for its full body. The detailed registry layout and queue operations are inherited from the supplied implementation and its documented earlier inspection, not newly proved by that placeholder. [[S22]](#source-s22) [[S29]](#source-s29) [[R02]](#source-r02)

This distinction is important in your own mods. A descriptive function name or nearby source comment does not establish every internal field.

## Native flag helpers

The public decompilation provides the generic helper:

```c
D_800A63C4 |= arg0 & ~0x4000;
if (arg0 & 0x4000) {
    func_8007F378(0);
}
```

It also provides the reset helper that clears the cheat flags and invokes the original reload-related helper. These source facts support the reference's interpretation of the generic flag operation and the special `0x4000` bit. They do not make every cheat record an independently reversible toggle. [[S22]](#source-s22)

The reference only offers its flag operation for a record with the verified generic callback and a single nonzero effect bit after excluding the reload flag. It rejects zero, composite or unsupported masks, and the feature-only implementation checks that two advertised toggles do not share a duplicate mask. [[R02]](#source-r02)

## ON is a request; state is an observation

Clicking or selecting ON tells the mod what the user wants. Reading the native flags tells it what the game reports. Those can temporarily differ while a request is pending, or permanently differ when the handler did not produce the expected flag.

Do not turn an indicator green solely because somebody pressed A. Track requested, pending, accepted and observed states separately. The standalone menu displays active state from verified native flags. The feature-only Cheats project exposes desired preferences through the host settings and publishes diagnostic state separately. [[R02]](#source-r02) [[R03]](#source-r03)

For a feature without a trustworthy observable flag, label it **Activate** or **Run**. Inventing an ON state makes the UI look better while making it less truthful.

## Turning off one cheat without clearing everything

The reference's individual OFF path clears only the verified effect mask and leaves other bits intact. If the original record requests the reload-related side effect, it invokes the original generic helper with that bit. This is an inverse operation derived from the verified helper, not an assertion that the original game had a dedicated off button for every code. [[R02]](#source-r02)

Some effects can have consequences beyond the flag: a level may have changed, health may have been restored, or progress may have been altered. No flag clear can honestly be described as undoing all those events.

A master switch also needs a clearly stated policy. In the feature-only reference, master Off requests the native **Disable All Cheats** behaviour. It is broader than "disable only cheats first activated by this mod". The saved individual preferences remain available for when the master is On again. Document this interaction with manually entered codes or other cheat mods. [[R02]](#source-r02)

## Preferences versus one-shot actions

The twelve reversible preferences cover Heavy Rocket, Invincibility, Low Friction, Low Gravity, Super Double Jump, Super Freeze, Super Grab, Super Grapple, Super Jump, Super Speed, Super Swing and Super Throw. The four separate actions are Full Health, All Vehicles, Credits Level and Test Code. The last retains **no known effect** rather than promising a visible result that has not been established. [[R02]](#source-r02)

The reference action widgets use a Ready/Run-style enum. A rising edge requests the action; returning to Ready rearms it. Saved selections are baselined when entering gameplay and are not replayed simply because a save loaded. The original standalone menu instead presents action rows and confirmation pages, but uses the same distinction between commands and persistent effects. [[R02]](#source-r02) [[R03]](#source-r03)

## The safe execution boundary

Validate the complete record/list before writing any of its fields. Bound the linked-list walk, reject cycles and duplicate matches, and confirm that the expected native callback is present. Then recheck gameplay readiness immediately before the mutation.

The reference dispatches at most one native activation per update. It does not hammer a failed native enable every frame forever: an individual retry latch records that failure until an appropriate preference change rearms it. These are sensible protective choices because a native handler may cause a reload or other state transition. [[R02]](#source-r02)

Do not invoke native cheat callbacks directly from a renderer thread just because that is where your button was drawn. Collect the request in private mod state and process it at the verified game-update boundary. The standalone menu's input, drawing and command execution are separate for this reason. [[R03]](#source-r03)

## Save selection is a real boundary

A live preview player does not establish that the user has loaded a save. The inherited selected-save predicate checks native menu mode, save-slot bounds, fade/transition state and the player/vtable range. Its source notes explicitly explain that the save index can be populated while previewing slots. [[R02]](#source-r02) [[R03]](#source-r03)

Use that exact predicate only for the baseline it was developed against, with its documented evidence and tests. It is not a new public `is_save_loaded()` API. For a different feature or future build, revalidate the states you need.

## Study the source in this order

Start with `src/mod_manifest.json` to understand the user-facing options. Read `gameplay_block_reason`, `matches`, `catalogue` and `queue_native` before the preference loop. Then read `native_flag_mask`, `refresh_masks`, `rocket_cheats_tick` and `rocket_cheats_after`.

Finally inspect `tests/test_source.c`, the MIPS tests and the relocation tests. They show the synthetic conditions used to validate the implementation. Do not mistake their fabricated registry data for a dump of the retail game.

## Acceptance tests for a gameplay mod

Use a disposable profile. Test one reversible effect alone, another alongside it, then switch off the first and confirm the second remains active. Check the original/manual reset interaction. Trigger Full Health twice with deliberate rearming. Confirm a saved Run selection does not fire on launch. Enter File Select, load each slot, die/reload, change levels and return to the frontend.

A source assertion can prove which bit your code cleared in a fixture. Only a gameplay run can establish the actual effect and transitions on that host. Keep both kinds of evidence.



---

<a id="chapter-10"></a>

# 10 — Colours, materials and textures

**Case study:** `reference_projects/Rocket_Colour_Studio_1.0.0`. The lesson is not "find red bytes". It is "find the right owned material, then understand how it produces red".

Colour Studio originally did nothing because it was looking at an incomplete and incorrectly filtered part of Rocket's rendering data. It imported successfully; that was not the missing piece. The corrected implementation is a useful example of how to turn a cosmetic idea into a carefully scoped mod. [[R01]](#source-r01) [[D01]](#source-d01)

## Where can the colour come from?

A visible surface can derive colour from a solid material, material constants, lighting, a direct-colour texture or an indexed texture's palette. The combiner determines how some of those inputs are used. A red-looking object does not necessarily have a red environment constant, and a red constant does not necessarily affect the visible RGB result. [[S24]](#source-s24) [[S25]](#source-s25) [[R01]](#source-r01)

The game's `MaterialGfx` union can represent a textured-material address or a solid colour. Loaded textured materials refer to texture arrays with their own format, size, image and palette information. That is why interpreting every `materialData.raw` as an RGB value is wrong. [[S24]](#source-s24) [[S35]](#source-s35)

## Indexed colours are not pixel colours

For CI4 and CI8, image data selects palette entries. If you want to recolour the red entries without changing the texture layout, preserve the indices and modify a private copy of the relevant palette. Do not convert index values directly into red/green/blue channels. [[R01]](#source-r01)

For direct RGBA16 or RGBA32 textures, the colour is stored in the texture samples themselves, with a different channel representation. The reference has separate handling for those transfer kinds. It also preserves transparency rather than replacing an entire packed value with an opaque RGB result. [[R01]](#source-r01)

For intensity or intensity/alpha data, colour can come from other rendering inputs. Do not force it through an RGBA algorithm simply because its bytes are accessible.

## RGBA16 as a small format example

An RGBA16 sample contains 5-bit red, green and blue channels plus one alpha bit. A small unpacking helper can extract those fields before applying a colour rule:

```c
unsigned int r5 = (sample >> 11) & 31u;
unsigned int g5 = (sample >>  6) & 31u;
unsigned int b5 = (sample >>  1) & 31u;
unsigned int a1 =  sample        &  1u;
```

Repacking must retain the intended alpha and channel range. This format-level helper says nothing about whether a particular address actually contains an RGBA16 sample. Establish the material/transfer format first. [[S25]](#source-s25) [[R01]](#source-r01)

## Only Rocket's original red parts

The reference scopes its traversal to explicit player-part relationships, excluding the main wheel, instead of walking the entire level for red materials. It evaluates red eligibility against the **original** source colour. That matters after the user chooses blue: a second update must not reject the same owned source simply because the previous output is no longer red. [[R01]](#source-r01)

A red predicate is a feature-design choice with thresholds. Test dark reds, orange/yellow neighbours, neutral greys, highlights and transparent entries. Do not describe a threshold as the game's official definition of a red panel. The reference's rules are its own scoped recolouring policy.

## Private copies preserve shared originals

The important sequence is:

```text
Find a verified Rocket-owned material call
  -> resolve the original material and transfers
  -> classify the actual colour inputs
  -> make/reuse bounded private copies
  -> compute colours from the original source
  -> redirect only the owned call
  -> restore the original call when Original is selected
```

The original palette or material may be shared elsewhere. Editing it globally can repaint scenery or another object using the same asset. Private copies and conservative restoration avoid that broad side effect in the reference design. They still need lifetime and compatibility checks; "private" is not an excuse to ignore scene unloads. [[R01]](#source-r01)

## Mip levels and animated/dynamic materials

A material can upload multiple related texture levels. Recolouring only the largest source may make the colour change back at a distance. The reference parses supported uploads rather than assuming there is one texture pointer per material. [[R01]](#source-r01)

Likewise, a dynamic material's display-list address may be rebuilt through a segmented buffer. Resolve the current address at the appropriate update point. Do not cache one frame's temporary resolved address and assume it remains the material forever.

Not every display-list layout is supported by every custom parser. Bound the command walk and reject layouts you cannot safely interpret. Add unsupported cases to diagnostics so "skipped" is not confused with "recoloured".

## Colour controls: state the order clearly

The finished reference includes custom RGB, presets, hue shift, saturation, brightness and colour strength. Original bypasses the transform and restores the original calls. The custom RGB values remain separate from preset selection so switching modes does not destroy them. [[R01]](#source-r01)

A useful way to document any colour pipeline is: define the target from Original/Custom/Preset/Hue mode; apply the chosen modifiers; blend using the defined strength; clamp and repack to the original format. Then point to the exact implementation for its real ordering. Do not imply that every colour mod or graphics API uses this same order.

Neutral settings deserve explicit tests. At the documented neutral values, the working baseline should be unchanged except for the selected target colour. At zero strength or Original, source values and material calls should be restored according to the feature's contract. At extreme brightness/saturation, packed channels must remain valid and transparency must be preserved.

## Reading the failed report properly

The v0.4.0 report recorded three reached models, zero red targets, zero active private calls and five ownership rejections. A rejected player part contained a palette with 248 red entries. This is evidence that relevant data was present but excluded by the mod's checks—not evidence that the host failed to load the package. [[D01]](#source-d01)

The report's "index" field was the collector's label for an assumed value. Its large number did not establish an invalid object. The corrected implementation changed the ownership proof; it did not simply remove all bounds and colour checks. The tests now include the report-derived structure with synthetic colours, separately labelled from an actual texture dump. [[R01]](#source-r01)

## Interaction with replacement texture packs

A code mod changing original colour data and an RT64 texture replacement are two different mechanisms. A replacement may visually supersede the source you changed; changing source data can also affect the texture identity a replacement system observes. Treat that as a compatibility question to test, not a guarantee that every texture pack will preserve the same cosmetic result. This is a consequence to investigate from the two designs, not a blanket result measured for all packs. [[S16]](#source-s16) [[R01]](#source-r01)

Test with no texture pack first. Then test a specific pack, record its version/hash and describe any limitation. Keep the original-red-only promise scoped to the actual tested appearance.

## Where to look in the reference project

Read `src/rocket_colours.c` with `src/engine_baseline.json` and `src/report_fixture.json` beside it. The baseline records the retained material/ownership engine; the fixture captures the structural failure case. Colour-control tests exercise the new presets and modifiers, while ownership and material tests address the actual targeting path.

The reference's `src/build.py` is a narrow custom package writer. Its hook/relocation details are part of that project and must not be confused with automatically registered functions in the maintained generic builder. [[R01]](#source-r01)

## Visual acceptance test

Check Rocket in more than one level, in different camera positions, while moving and while animations are running. Change to a conspicuous colour, then Original. Check non-red parts, the excluded wheel, scene changes and distant views. Watch for one-frame flashes, flickering materials, unbounded allocations or unrelated surfaces changing.

The native test can compare packed colours exactly. The game run is still needed to establish that the corrected material is the one actually being drawn.



---

<a id="chapter-11"></a>

# 11 — Building a menu inside the game

**Case study:** `reference_projects/Rocket_Cheat_Menu_3.0.1`. This is a standalone guest-rendered menu, not a new launcher page.

The basic approach worked: the mod draws a compact panel into the game's graphics stream, reads mapped N64 controls and posts cheat requests to a separate execution hook. It does not need a native UI library inside the package. The later File Select issue showed why the state gate is just as important as the drawing. [[R03]](#source-r03)

## What the reference menu looks like

![Software preview of the Movement page, with one green ON row, grey OFF rows and a highlighted selection](assets/menu_page_0.png)

*Software preview decoded from the reference project's emitted display-list commands. This is not a screenshot captured from Rocket-R, and it does not verify the final renderer.* [[R03]](#source-r03)

## Keep four responsibilities separate

**Input** determines whether the user opened the menu, moved a selection or requested an action. **Menu state** holds the page, row, confirmation and status. **Feature logic** validates and performs a cheat at the correct game boundary. **Rendering** turns the current state into drawing commands.

Do not invoke a gameplay-changing callback because a row happened to be drawn. Do not turn a draw function into your save-state manager. Keeping these responsibilities separate makes the implementation easier to test and prevents the renderer from becoming the owner of unrelated game mutations.

## The four hooks in the supplied menu

| Function | Position | Responsibility |
| --- | --- | --- |
| `func_8004ED04` | Entry | Accept/validate a pending command for the native cheat dispatcher. |
| `func_8004ED04` | Return | Observe completion and refresh reported state. |
| `get_controller_data` | Return | Work with the game's mapped input after its read. |
| `func_80075ABC` | Return | Append the panel after the native HUD pass and before task submission. |

These are the specific reference targets. The public source confirms the render/update ordering, while the exact ABI, state values and extra checks are documented in the reference's `SOURCES.md`. Some underlying functions remain public assembly placeholders. [[S21]](#source-s21) [[S23]](#source-s23) [[S29]](#source-s29) [[R03]](#source-r03)

The mod deliberately avoids taking a raw hook on protected `schedule_gfx_task`, `update_gfx_context` or interpolation functions. That choice is part of compatibility with this host, not an invitation to bypass protection. [[S08]](#source-s08) [[S09]](#source-s09) [[R03]](#source-r03)

## Do not open it on File Select

This is not optional polish. The old menu could open while a preview player existed on File Select. A player pointer alone therefore did not establish ordinary gameplay.

The inherited v3.0.1 predicate reads the following baseline-specific state:

| Guest address | Required condition in this reference |
| --- | --- |
| `0x800ABCD8` | Native menu mode is 4. |
| `0x800A5A48` | Native save index is within 0–2; not sufficient by itself. |
| `0x800AC2E4` | Fade/transition controller is idle (0). |
| `0x800AF5F0` | No pending level transition (`0xFFFFFFFF`). |
| `0x800AAF5C` | Player pointer and its expected vtable meet the reference's range/alignment checks. |

**These values are inherited implementation evidence, not a public stable API.** The supplied source notes identify the earlier local control-flow observations used to establish them. The public placeholders alone do not prove every value. The File Select hotfix has synthetic regression coverage; a fresh actual-game verification of that exact hotfix was not recorded for this edition. [[R03]](#source-r03)

Check readiness at input, drawing and command-execution boundaries. If gameplay becomes unavailable, close the menu, discard pending confirmations and reset only private mod state. Do not keep clearing frontend controls or appending graphics while trying to make the panel disappear.

## Holding the shortcut across a transition

A user can hold L+R while a save loads. If the first gameplay sample treats that held chord as a new press, the menu opens unexpectedly. Establish a fresh baseline after the transition and require release followed by a new deliberate press.

Distinguish an ordinary close from a state-loss close. An ordinary close can drain held menu buttons so they do not immediately jump/attack in gameplay. A return to File Select must **not** keep draining the original frontend's input. The reference has separate paths for those cases. [[R03]](#source-r03)

## Mapped N64 controls, not arbitrary PC keys

The menu sees the N64 input after Rocket-R's keyboard/controller mappings. Its default L+R means **N64 L and N64 R**, not the literal letters L and R on the keyboard. The reference offers chord or individual N64-button choices, and users can map those actions using the existing host controls. [[R03]](#source-r03) [[S32]](#source-s32)

A plain guest mod has no general public facility here to capture a spare, unmapped F-key directly. A custom SDL hotkey-capture interface belongs to the host unless an appropriate guest service is deliberately exposed. Do not advertise direct arbitrary-key rebinding when your implementation only reads mapped N64 buttons.

The native modern camera can use separate host-side inputs. Consuming the guest's normal N64 controls is not a promise that every camera/mouse/overlay shortcut is also intercepted. The original standalone menu explicitly keeps this limitation. [[S32]](#source-s32) [[S33]](#source-s33) [[R03]](#source-r03)

## Navigation and confirmations

The reference uses up/down for rows, left/right for pages, A to toggle/run and B/Start to back out. Repeated held navigation is handled separately from one-shot activation. A confirmation should not accept the same held A that opened it; require the deliberate next input edge.

Use text as well as colour. Green ON and grey OFF remain clear with their labels; unsupported or unverified actions should say RUN, USE or N/A rather than a fabricated state. The highlighted row and an active effect are different things and need different visual treatment.

Put destructive or scene-changing actions behind a confirmation. Show what will happen, not just "Are you sure?". If gameplay ends while the dialog is open, cancel it rather than replay it on the next save.

## Drawing safely

Rocket's authored framebuffer is 320×240 with a safe area used by the host presentation. The reference uses a compact panel inside that area, with hand-authored glyph patterns, solid rows and explicit state labels. Its software previews illustrate the emitted commands; they are not captured game frames. [[S11]](#source-s11) [[S34]](#source-s34) [[R03]](#source-r03)

The draw hook validates the current task, buffer bounds, head, limit and framebuffer. It first counts the bytes its commands/resources need. Only when capacity is sufficient does it copy the resources and emit the commands. It reserves 512 bytes for the original footer in this implementation, then lets the original native finalisation continue. [[R03]](#source-r03)

That two-pass approach matters. A buffer failure must not leave half a menu and an overwritten footer. The reference also closes after repeated render-allocation failures rather than trapping the user in an invisible input-capturing panel.

## Graphics state is not yours forever

The panel explicitly sets the state it needs: colour image, scissor, modes, texture upload and combiner configuration. It is drawn at a selected late boundary so the normal frame footer follows it. That is an intentional arrangement, not a general guarantee that a mod can change arbitrary render state mid-scene and leave it behind. [[R03]](#source-r03) [[S25]](#source-s25)

If you move the menu earlier in the frame, change its coordinate system or combine it with another HUD mod, re-evaluate graphics state and command ordering. Memory bounds alone do not prevent a later draw using the wrong state.

## Does the menu pause the game?

No. The supplied standalone menu consumes normal guest gameplay controls, but the simulation keeps running. It does not own a safe global pause facility. State this in the description and advise opening it in a safe place. Freezing a game by changing an unexplained timer or skipping a scheduler is not a harmless UI improvement. [[R03]](#source-r03)

## Reusing this for another menu

The panel layout, navigation, confirmations and buffer writer are useful reference code. Replace the feature-specific model, do not retain a cheat dispatcher in a settings menu merely because it is already there.

A general mod-settings menu additionally needs a reliable catalogue, schemas, per-mod reads, validated writes and persistence. Those are not supplied by this drawing layer. Chapter 13 explains the difference between cooperative interfaces and a host-backed universal manager.

## Test the things users will actually do

Open/close repeatedly. Hold the shortcut, A, B and the stick. Change pages at each end. Cancel confirmations. Open the normal host overlay while the panel is visible. Test after loss of focus, during a reload, on File Select, after selecting all three slots, and after returning to the frontend.

Test small buffers and rejected tasks in fixtures. Then check the real renderer at the supported aspect/scale settings. An attractive software preview proves the layout decoder produced an image; it does not prove the command stream behaves correctly in RT64 on every platform.



---

<a id="chapter-12"></a>

# 12 — Creating an RT64 texture pack

**Scope:** the texture-replacement route. This is separate from a code mod editing Rocket's original material data.

Rocket-R accepts `.rtz` packs containing `rt64.json` and replacement images. The database maps runtime texture identities to files. A folder full of high-resolution images with attractive filenames is not enough unless the database/path rules connect them to the actual game textures. [[S01]](#source-s01) [[S04]](#source-s04) [[S16]](#source-s16)

## The basic workflow

Start with one texture and a verified mapping. Produce a clearly recognisable replacement, package it, import it, restart the game to mount the selected pack and inspect the intended surface. Only then expand to a full set. That is the texture equivalent of starting with one small gameplay change.

Keep editable image sources outside the final pack. Keep original texture dumps local. A published pack should contain the replacements you are entitled to distribute, its database and the necessary notices—not a raw memory/ROM dump.

## What the pinned RT64 guide supports

The pinned upstream guide documents DDS and PNG. DDS is its preferred distribution format because it supports mipmaps and efficient GPU texture formats; PNG is convenient during development. Exact format support and memory behaviour still need verification on each target, particularly mobile. This book does not turn an upstream performance recommendation into a measured guarantee for every Rocket-R device. [[S16]](#source-s16)

Mipmaps are the smaller versions used as a texture becomes less detailed on screen. Replacing the highest level without considering distance can produce shimmering or inconsistent transitions. The guide describes a low-mipmap cache for streaming packs so a coarse replacement is available while larger levels are loaded. Regenerate that cache after changing the DDS content. [[S16]](#source-s16)

## A database entry is a mapping, not a guess

A conceptual entry looks like this:

```json
{
  "hashes": {
    "rt64": "REPLACE_WITH_A_REAL_CAPTURED_HASH"
  },
  "path": "Rocket/MyReplacement"
}
```

**This is explanatory JSON, not a usable texture mapping.** The placeholder must be replaced with a real texture identity from an appropriate matching workflow. Do not hash the replacement PNG's file bytes and assume that is the original runtime texture hash.

The pinned guide describes RT64 hashes based on texture-memory data and associated mapping support. It also explains that RT64 does not compute Rice hashes at runtime; conversion/mapping is a separate tool workflow. Renaming old Rice files alone is not proof they will be found correctly. [[S16]](#source-s16)

## Configuration fields

| Field/group | What it controls |
| --- | --- |
| `configurationVersion` / `hashVersion` | Database and hashing format expectations; use versions compatible with the audited renderer/tool output. |
| `autoPath` | Automatic filename lookup scheme when an entry does not provide a path. |
| `defaultOperation` | Streaming, preloading or stalling policy. |
| `defaultShift` | Half-texel compensation policy. |
| `operationFilters` / `shiftFilters` | Path-pattern rules, evaluated in order, subject to per-texture overrides. |
| `textures` | The mapping records and optional per-texture choices. |

The pinned guide's illustrated schema uses configuration version 3 and hash version 5. Do not blindly copy a future database version from upstream and assume the older bundled renderer understands it. Do not copy its sample hashes as if they were Rocket's textures. [[S16]](#source-s16)

## Stream, preload or stall?

Streaming loads asynchronously; a low-mip cache reduces visible replacement pop-in. Preload keeps selected replacements resident and increases startup/memory costs. Stall blocks while a texture loads and is not the upstream guide's preferred default. These are loading behaviours, not changes to the original game's draw distance. [[S16]](#source-s16)

Avoid preloading everything because one texture popped in. First check the mappings, mip cache and specific load behaviour. A very large permanently resident cache can undermine the reason for streaming in the first place.

Rocket-R's guide gives a replacement-pool cap of **128 MiB on Android and 512 MiB on desktop**. That is a replacement pool limit, not a total application-memory budget or a promise that any pack up to that file size will run smoothly. Archive size, expanded size, GPU allocation and working-memory usage are different quantities. [[S01]](#source-s01)

## Texture coordinates and aspect ratio

Preserve the intended texture dimensions/aspect and the material's mapping unless you are deliberately changing them with appropriate code. Use the shift policy documented for the asset rather than moving pixels around to compensate for an unexplained alignment problem.

The upstream guide supports `half` and `none` shift behaviours and path/per-entry control. Test the visible result for the actual Rocket asset. Do not apply one global "looks right in this screenshot" correction to the entire pack. [[S16]](#source-s16)

## Tool commands, with a host-compatibility warning

The pinned RT64 guide documents:

```text
texture_hasher <dump_directory> --rice
texture_packer <texture_pack_directory> --create-low-mip-cache
texture_packer <texture_pack_directory> --create-pack
```

Its packer defaults to Zstandard compression and also accepts `--deflate` or `--store`. Rocket-R's outer importer uses its own miniz-based metadata reader before handing the pack to RT64. For this host, **Deflate is the conservative compatibility choice for initial testing**; do not assume that every upstream compression default has passed this outer import path. Test the exact final archive. This is an integration recommendation drawn from the two implementations, not a measured comparison of all compression modes here. [[S16]](#source-s16) [[S04]](#source-s04)

```text
texture_packer <texture_pack_directory> --create-pack --deflate
```

Replace the placeholders with real directories and use a tool version compatible with the renderer. No image conversion or packer executable is bundled in this handbook.

## Do I have RT64's developer UI in Rocket-R?

The upstream guide describes texture-dump and interactive-replacement controls in RT64's developer tools. Rocket-R repurposes F1/Escape for its own overlay, and this edition has not verified an exposed end-user route to all of those upstream developer controls in the published host. **Do not follow an instruction saying "press F1 for the RT64 texture debugger" as though it were already a Rocket-R menu item.** [[S16]](#source-s16) [[S33]](#source-s33)

Use a compatible development workflow where the required dump/tool controls are actually available, or a properly sourced existing database for the same game. Record how the mapping was obtained. If that access is missing, it is a tooling task to resolve—not a reason to invent hashes or publish an empty placeholder pack.

## Optional mod metadata

An `.rtz` may include `mod.json` with a friendly name, authors, game compatibility and version. Without it, Rocket-R assigns a generic hash-based identity. The archive still needs `rt64.json`. Use forward-slash archive paths and respect the same archive safety constraints described in Chapter 5. [[S04]](#source-s04)

The maintained code-mod builder is not a texture packer. It only automatically includes its small known additional-file set; it will not recursively turn your art directory into a texture pack. [[S02]](#source-s02)

## Test the pack in context

Check near/far views, alpha edges, texture repetition, UV seams, animated frames, low-mip transitions and performance when entering a new area. Test the game without the pack for comparison. Then test alongside any material-changing code mod, because replacing a texture and recolouring its source are different mechanisms.

A successful import proves the metadata/archive passed that stage. It does not prove a single mapping matched. The first test should always be a conspicuous replacement on one known surface, followed by restoration to the original by disabling the pack for the next launch.



---

<a id="chapter-13"></a>

# 13 — Making mods work together

**Goal:** explicit contracts, shared-state discipline and honest limits—not a pile of patches fighting over the same field.

Two mods can load successfully and still conflict. Package validation checks names, versions, declared dependencies and metadata. It cannot infer every write your C code will make at runtime. Treat compatibility as something to design and test, not a benefit automatically granted by using the same mod format. [[S01]](#source-s01) [[S04]](#source-s04)

## Package identity and selected versions

A profile selects a package hash for each mod ID. Dependencies refer to identities and optional minimum versions. The host validates that the resolution is consistent before launch. Your build folder's newest file does not automatically become the running version merely because its filename looks newer. [[S04]](#source-s04)

When debugging a combination, record all selected mod versions and hashes. Disable one package at a time and repeat the same test. "Works without other mods" establishes an interaction; it does not identify which mod is wrong.

Keep saved IDs stable across cosmetic renaming. A new ID installs a separate mod rather than migrating the previous one. If you deliberately split a combined package into two, explain which old package users must disable and how any preferences are transferred—or state that they are not transferred automatically.

## Required dependencies

A consumer can declare:

```toml
dependencies = ["bible_speed_provider:1.0.0"]
```

The provider must be installed and selected compatibly. An explicitly disabled required package is not a valid way to start the consumer without it. The host resolves the dependency before the dependent package. [[S04]](#source-s04) [[S10]](#source-s10)

Keep the dependency narrow. A camera client that only needs a speed provider should not depend on the entire Cheats package because it happens to export something useful. Separate shared support from unrelated gameplay features.

## A complete cooperative example

The handbook includes **Speed Provider** and **Camera Client**. They demonstrate an explicit export/import relationship, not universal cross-mod configuration access.

The provider's exported function reads **its own** setting:

```c
__attribute__((used, retain, section(".recomp_export")))
RocketU32 bible_speed_v1(void) {
    RocketU32 value = recomp_get_config_u32("speed");
    if (value < 1u) value = 1u;
    if (value > 5u) value = 5u;
    return value;
}
```

The client imports that function from the named dependency:

```c
__attribute__((noinline, weak, used,
               section(".recomp_import.bible_speed_provider")))
RocketU32 bible_speed_v1(void) {
    return 0u;
}
```

The stub is resolved by the runtime to the provider's export. It is not a normal local fallback for a missing required dependency. The underlying pinned runtime resolves imported functions against the named loaded mod's exports. [[S18]](#source-s18) [[S28]](#source-s28)

The client calls it during a supported camera update and uses the returned, bounded speed. The provider itself changes no game state and claims no camera resource. The client declares `camera.orbit`; it must not be enabled alongside another package claiming that exclusive resource.

**Validation boundary:** both source projects passed native mock and MIPS compile/link checks for this handbook. Their actual RecompModTool packaging, cross-mod runtime resolution and camera behaviour were not exercised in Rocket-R here. Follow the expected-result checks after building them on the prepared baseline.

## Expected test for the pair

Build both `.nrm` packages using the maintained toolchain, import both into a test profile and disable competing orbit packages. Launch a save, change the provider's Shared speed in Details and settings and test the client camera. The client should obtain the new provider value when it next calls the export.

Then disable the provider in a separate next-launch setup while leaving the consumer selected. The dependency check should block that invalid combination. That is a good failure, not something to work around by deleting the dependency declaration. [[S04]](#source-s04)

This example intentionally leaves ownership of the saved preference with the provider. It does not ask the consumer to rewrite the provider's host configuration.

## Design the contract before the feature grows

Use a versioned function or structure contract. Define units, ranges, sizes, pointer lifetimes, who owns allocated data and what happens before gameplay or during reloads. A function returning a small integer is much easier to make portable than one handing out an undocumented pointer to a private structure.

For an exported packet, include a version and size. Never assume a caller has space for a future enlarged structure. Do not retain another mod's temporary pointer beyond its stated lifetime. These are contract-design recommendations; the loader does not add such checks to arbitrary exported functions for you.

If you use an optional dependency, handle its absent state using the actual pinned runtime mechanism you have audited. A declared optional relationship is not permission to call an unresolved function unconditionally. The examples here use a required dependency to keep the first lesson unambiguous. [[S28]](#source-s28)

## Conflicts and exclusive resources

`conflicts` declares known incompatible mod identities. `exclusive_resources` describes something that should have a single owner, such as the camera orbit path. These are useful preventative checks, but they cannot discover undeclared memory overlap. [[S04]](#source-s04)

A restoration path can also cause conflicts. Imagine A replaces a material pointer, B replaces that pointer again, then A switches off and blindly restores the original. A has now removed B's change. A conservative restore checks that the current pointer is still the value A installed before replacing it.

Two packages can sometimes cooperate by chaining or composing changes, but that needs an explicit ordering/ownership contract. Simply moving the packages up and down until one screenshot looks right is not a compatibility design.

## What the host does not share with a guest menu

The host owns its package catalogue, profile selections, validated settings schema and persistence. The existing configuration imports are tied to the calling mod. The camera's native input/settings UI also contains host-owned controls beyond its ordinary manifest options. [[S04]](#source-s04) [[S05]](#source-s05) [[S06]](#source-s06) [[S20]](#source-s20) [[S33]](#source-s33)

A universal in-game manager would need services for listing the running packages, reading their option definitions and current values, making validated writes, persisting to the correct profile, reporting application/restart status, and controlling only supported live lifecycles. Those services need an agreed host interface. None appears merely because the manager successfully renders a panel.

A cooperative registry of specially adapted mods is a possible narrower design. It must be advertised as cooperative and must state whether its values synchronise with the host settings. It cannot honestly claim all selected mods work without adaptation or that private-value edits are automatically saved in Details and settings.

## Runtime-only values and saved preferences

A mod can maintain private runtime state that differs from its saved preference for good reasons: a command is pending, a scene does not support the feature, a safety check failed or a user made a temporary in-game choice. Name and display those states separately.

Do not edit managed profile/config files behind the host to simulate a generic settings API. The host has in-memory state, launch snapshots, validation and persistence rules; a disk write alone does not establish a live, synchronised update. The current code path explicitly forwards matching validated changes rather than watching arbitrary edits as a universal control mechanism. [[S04]](#source-s04) [[S06]](#source-s06)

## A compatibility note worth publishing

State which resources your mod owns, which other packages were tested, whether its settings are live, whether Off restores everything reversible, whether a level reload can occur and which versions are supported. List known incompatibilities plainly.



---

<a id="chapter-14"></a>

# 14 — Why is my mod doing absolutely nothing?

Don't change random addresses and hope for the best. Work out which stage failed. There are only a few places between an idea and a visible effect, and each can be checked separately.

## The shortest useful diagnostic chain

```text
Did import succeed?
  -> Is the correct package in the running session?
    -> Did the required hook/callback run?
      -> Did it receive the chosen setting?
        -> Did the target/ownership checks pass?
          -> Was a valid change actually written/requested?
            -> Did the game's consumer/render path use it?
```

If the callback never runs, changing the colour formula is irrelevant. If it receives blue but reaches no owned red materials, reinstalling the same package is irrelevant. Instrument the boundary that failed.

## Import failure

Check the final archive, not only the source tree. Verify `mod.json`, the game ID, author/version fields, path names, size limits, any paired code/symbol files, dependencies and native-payload restrictions. Use the actual importer message as evidence. [[S04]](#source-s04)

An `.nrm` built for another recompilation is not compatible simply because the extension matches. An empty archive is not a data-only mod. A source ZIP does not become executable because it contains a manifest.

Record the exact filename and SHA-256 of the attempted package. Two files with the same friendly filename can have different contents.

## Launch failure after a successful import

Import performs package-level checks. At launch the runtime resolves symbols, imports, callbacks, hooks and replacements. An unregistered host import, missing export, invalid callback event, incompatible hook or protected function can fail at that later stage. [[S09]](#source-s09) [[S28]](#source-s28)

An unresolved symbol accepted by the linker is not necessarily valid. The maintained workflow intentionally leaves resolution work for the package tool/runtime. Confirm that the target host actually registers the import and that its ABI matches your declaration. [[S02]](#source-s02) [[S17]](#source-s17)

If an error names a protected function, stop and inspect the checked policy. Do not remove protection or copy an old raw implementation over a host-fixed function.

## "Enabled next launch" will not go away

That wording is the ordinary package selection checkbox while a game is running. It is not a verdict that the package failed to load. The UI separately identifies a running package version or says the selection is not running this session. The camera has a different, explicitly integrated live toggle path. [[S05]](#source-s05) [[S06]](#source-s06)

Check the running version and active profile. A screenshot of a checked next-launch box alone cannot diagnose execution. A live hook counter can.

## The slider moves but the mod ignores it

Check the option ID, type and enum order. Check whether your code rereads the value or cached it at startup. Confirm the selected profile and package hash match the active session. Keep the ordinary package enabled while testing live writes; the audited host suppresses normal forwarding for a disabled next-launch entry. [[S04]](#source-s04) [[S10]](#source-s10) [[S20]](#source-s20)

For String input, the current frontend commits on Enter. For Number, do not assume a manifest `step` automatically rounds the value. Test what the getter actually returns. [[S05]](#source-s05)

A useful diagnostic stores the last effective setting value beside the hook count. That distinguishes a UI/configuration problem from a feature-application problem.

## Hook runs, but nothing changes

Track target counts and rejection reasons. Did you find the correct player? Does the root actually own the visible display list? Is the material solid, indexed or direct RGBA? Are you resolving a segmented pointer with the current base? Did an ownership guard reject the object before the effect ran?

The Colour Studio report supplied a concrete failure:

```text
ticks=1714
models=3
red_targets=0
active_private_calls=0
registered_parts=0
ownership_rejects=5
```

Later samples increased the tick count while retaining the same failure pattern. One rejected part carried a palette with 248 red entries. That evidence narrowed the fault to the implementation's target checks; it did not support blaming the launcher. [[D01]](#source-d01)

These are observations from the failed v0.4.0 session, not expected counters for a functioning release. The collector also warns that its reads are not atomic, so a single transient inconsistent sample is not a complete diagnosis.

## Change is written, but the screen is unchanged

Check the consumer. A display list may still reference the original material. The game may rebuild the field after your hook. The renderer may read a task snapshot taken earlier. A replacement texture may override the changed source. You may have changed a colour constant that the material combiner does not actually use. [[S11]](#source-s11) [[S24]](#source-s24) [[S25]](#source-s25) [[R01]](#source-r01)

Store the exact original and replacement pointers and a count of redirected calls. Compare the next consuming stage where possible. Do not assume a nonzero allocation proves that it is referenced by the final frame.

## It works until loading or File Select

Look for stale state, cached pointers, preview objects, held-input edges and queued actions. A save index can be populated during preview; a player pointer can exist before gameplay. The reference menu's fix added explicit state checks and cancelled private pending work on loss of gameplay. [[R03]](#source-r03)

Test input preservation, not just menu visibility. A panel that no longer draws but still clears File Select's buttons is still broken. A close handler that drains held controls in the wrong state can be just as disruptive as the opening bug.

## It works alone, but not with another mod

Record all active hashes. Test the smallest pair. Compare hooks, resource declarations, fields written and restoration behaviour. Check whether both depend on the same provider version or claim the same exclusive resource. Different load order may expose an ownership bug without solving it. [[S04]](#source-s04) [[S19]](#source-s19)

If an internal Off setting does not remove a package-level resource conflict, that is expected: the package remains selected and loaded. Distinguish feature state from the launch plan.

## It flickers, stutters or allocates forever

Check for per-update allocation, whole-level scans, unbounded command walks, excessive logging, string allocation every frame and texture-copy work repeated unnecessarily. Use bounded caches with the correct lifetime and record allocation totals over many updates.

For rendering, preserve the host's task and interpolation ownership. Do not hook around a glitch by disabling a protected path that keeps unrelated geometry stable. The architecture and development guide describe why owner identities and frame snapshots exist. [[S11]](#source-s11) [[S13]](#source-s13)

## A compact diagnostic record

A useful private record contains a signature/version, hook counts, last input settings, gameplay block reason, reached/eligible targets, requested/completed actions, active flags, allocation/failure counts and last error. Keep it bounded. A local read-only collector can then inspect values without needing a raw game-memory dump. The supplied reference checkers demonstrate that approach. [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)

Do not ship continuous large dumps or write a log line for every texture every frame. Log state transitions and bounded summaries. The goal is to find the failing stage without creating a new performance problem.

## Useful checks, and what they actually prove

| Check | Establishes | Does not establish |
| --- | --- | --- |
| Compiler success | Accepted source for the chosen target. | Valid game imports, correct runtime state or visible effect. |
| Package preflight | Selected archive/metadata properties. | Full importer equivalence or gameplay. |
| Running-session line | Package included in the host's active snapshot. | A specific feature hook executed successfully. |
| Increasing hook counter | That instrumented hook is executing. | Correct targets or completed effect. |
| Native unit test | Logic against its native fixture. | Correct guest memory layout in the actual game. |
| Packaged MIPS test | Emitted instructions against the bounded test interpreter/fixture. | Host JIT, RT64, audio or actual game behaviour. |
| Software render preview | The tested command/layout decoder produced that preview. | A frame captured from Rocket-R. |
| Actual gameplay test | Observed behaviour in the recorded scenario/build/device. | Every untested state or platform. |

## A bug report somebody can act on

Include the host version/platform, mod ID/version/hash, selected/running profile, other active mods, exact settings, the screen or level, what you pressed, what you expected and what happened. Include the actual error/log or bounded diagnostic report. Keep a copy of the last working package.

A good report can be short: "v1.0.1 Windows, menu v3.0.0, L+R on File Select opens the panel and breaks selection; after loading a save the menu works." That tells us where the lifecycle assumption failed.



---

<a id="chapter-15"></a>

# 15 — Testing and releasing properly

**A successful build is the start of testing, not the end of it.**

Keep separate records for source checks, package validation, simulated execution and actual gameplay. That distinction saved us from repeatedly treating a compiled but ineffective cosmetic mod as finished. It also tells another creator exactly what evidence they still need to collect.

## Four verification labels

**Compiled** means the source was accepted for the intended target and linked as described. **Package checked** means specified archive/metadata/symbol properties were inspected. **Automatically tested** means named tests passed against their stated fixtures. **Gameplay confirmed** means somebody actually ran that exact feature/version in a recorded host/device scenario.

Use the most precise label, not the most impressive one. A large synthetic assertion count does not become gameplay confirmation. Rocket-R's own testing guide explicitly separates CPU/package tests from visual and device evidence. [[S14]](#source-s14)

## Start with the invariant

Before writing a test, say what must remain true. A colour mod must preserve non-target colours, original alpha and shared source data. A cheat toggle must leave unrelated verified flags alone. A menu must leave File Select input and drawing untouched. A callback must not use a packet after its lifetime ends.

Then test both the happy case and the rejection case. The rejected case is not wasted work; safe refusal is often the correct behaviour when a scene or buffer is unsuitable.

## Avoid tests that repeat the implementation's guess

If your code assumes an object field is a child index, and your fixture writes the expected index into that field, both can agree while the real game disagrees. Use source-derived layouts and captured structural evidence independently of the implementation under test.

For the Colour Studio case, the corrected tests include the observed parent links and field values that made the earlier guard reject the relevant parts. The actual colours remain synthetic where the report did not contain original texture data. Both facts should be written in the test description. [[D01]](#source-d01) [[R01]](#source-r01)

## Test the emitted package too

Compilers optimise code and linkers relocate it. A native test that replaces guest memory access with helper functions cannot exercise all of that. The reference projects therefore also load their compiled MIPS payload into a bounded interpreter at several simulated load addresses, checking relocation and emitted logic. [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)

This is stronger than source tests alone, but it still is not the host live recompiler or RT64. Treat it as a distinct layer. If an instruction or relocation is unsupported by the harness, report that gap rather than silently skipping the operation and calling the case passed.

The new tutorial lessons are a different validation scope: native mock assertions and MIPS compile/link checks only. The documentation package records that official RecompModTool packaging was not executed here for those lessons.

## A practical gameplay matrix

| Area | Checks to perform |
| --- | --- |
| Startup | Correct version selected, no unwanted one-shot actions, no writes before required state exists. |
| Frontend | Title, File Select, slot preview and erase paths remain unaffected. |
| Save selection | Each slot, new/existing file behaviour and held shortcuts across loading. |
| Ordinary gameplay | Feature On/Off, minimum/maximum/default settings, movement and idle behaviour. |
| Transitions | Death, reload, level changes, cutscenes and returning to File Select. |
| Input | Keyboard/controller mappings, held/released buttons, repeat navigation and focus changes. |
| UI | Readability, confirmation/cancellation, small screens, supported aspect modes and host-overlay interaction. |
| Compatibility | Targeted pairs of mods, selected versions, dependencies and shared resources. |
| Persistence | Normal close/restart, old profile upgrade, defaults for new options and recovery mode. |
| Performance | Allocation growth, logging load, frame stutter and sustained use. |

Pick scenarios appropriate to the feature, but do not omit frontend/transition checks just because the mod is intended for gameplay. That is often where stale state first becomes visible.

## Platform coverage

A portable guest package is not a guarantee of identical rendering, memory pressure or controls on every host. Test Windows, Linux/Steam Deck and Android according to the devices you support. State untested platforms clearly. The pinned project's platform guide itself distinguishes native device results from QEMU/CPU/package checks. [[S01]](#source-s01) [[S14]](#source-s14)

The handbook's fresh tests ran in a Linux container, not on those game devices. No ROM was used, no Rocket-R game session was launched, and no new visual/audio/controller gameplay verification is claimed.

## When you change the host instead of just a mod

Follow `docs/DEVELOPMENT.md` and `docs/TESTING.md` for host changes. Do not hand-edit generated recompilation files or dependency checkouts. Use the maintained source/policy/patch routes and the affected platform builders. [[S13]](#source-s13)

The project's documented source checks include:

```text
python scripts/self_check.py --root .
python scripts/verify_recomp_policy_v42.py --root .
python -m unittest discover -s tests -p "test_*.py"
```

Other checks in the testing guide cover presentation, interpolation and generated-code output. These are host-repository checks; they do not replace a custom mod's own tests. Do not claim to have run them merely because a tutorial source compiled. [[S14]](#source-s14)

## Release checklist

Confirm the mod ID is yours and the version changed. Confirm the minimum host requirement and declared dependencies. Check that descriptions distinguish live preferences, one-shot actions and restart requirements. Keep unsafe/scene-changing operations explicit.

Rebuild the final source, inspect the actual package and record its SHA-256. Run the tests against that package rather than a stale intermediate. Import it into a clean test profile. Check persistence and upgrade from the previous profile. Keep a rollback package and instructions.

Review the archive for ROMs, raw original instruction dumps, extracted game assets, signing keys, personal paths and accidental native executables. A passing filename scan is not a full rights audit. The project notice states which project code uses GPL-3.0-or-later and keeps the game and third-party component rights separate. [[S15]](#source-s15) [[S36]](#source-s36)

## What to put in the release README

State what the mod does, how to install it, its controls/settings, host/version requirements, known limitations, save implications, tested platforms and the source/test location. Say whether the package replaces an earlier mod ID or installs alongside it. Include the author and any relevant third-party notices.

For an in-game menu, explain whether it pauses, which mapped controls it sees, when it is unavailable and how to close it. For Cheats, explain global reset versus individual OFF. For a colour mod, state the exact target scope and texture-pack compatibility results.

Do not hide "requires a new host build" at the bottom of a release intended for ordinary `.nrm` import. Equally, do not tell users to rebuild the game for a self-contained package that does not require it.

## Updating settings safely

Test real old settings. Stable IDs preserve user intent; friendly labels can change independently. Newly added settings need safe defaults. Removed or narrowed choices need a deliberate migration/recovery plan because existing matching values are revalidated at launch. [[S04]](#source-s04)

Do not alter immutable library contents as an update mechanism. Re-import the new package and select it for the next session. Verify the active-version message after restarting. [[S04]](#source-s04) [[S05]](#source-s05)

## Publish what was actually verified

An honest note might say: "Source and packaged-MIPS tests pass; Windows gameplay checked in two levels; Android not yet checked." That is useful. "Fully tested" without the build, scenario or device is not.



---

<a id="chapter-16"></a>

# 16 — Reference, recipes and the questions that keep coming up

This is the quick reference. Use the earlier chapters for the reasoning and complete projects. Names below are scoped to the audited version; a similarly named upstream feature is not automatically a Rocket-R API.

## Public Rocket API 1

| Name | Contract |
| --- | --- |
| `ROCKET_CALLBACK(event)` | Marks a retained function for a base-host callback event. |
| `ROCKET_IMPORT` | Marks an import placeholder for a registered base-host function. |
| `rocket_on_game_ready` | Setup event after mod/runtime services are ready, before game entry. |
| `rocket_on_camera_update` | Temporary orbit packet during supported main-camera update. |
| `rocket_on_mouse_look` | Temporary mouse-delta packet before the camera callback. |
| `rocket_on_first_person_update` | Separate first-person packet during the original first-person input path. |
| `rocket_claim_analogue_camera()` | Requests the host's dedicated camera input path for the participating mod. |
| `rocket_enable_mouse_look()` | Opts into supported mouse look; host focus/overlay/user settings still apply. |
| `rocket_enable_first_person_look()` | Opts into the first-person event/input support. |
| `rocket_set_camera_smoothing(percent)` | Requests native follow smoothing from 0 to 100 while the mod owns a supported view. |
| `recomp_get_config_double(key)` | Reads the calling mod's numeric setting. |
| `recomp_get_config_u32(key)` | Reads its enum index or integer value. |

The header defines a 56-byte `RocketCamera`, 16-byte `RocketMouseLook` and 48-byte `RocketFirstPersonCamera` under the specified 32-bit layout. Always check API/size where the contract provides them; do not interpret one packet type as another. [[S03]](#source-s03)

### Orbit packet fields

`api` and `size` describe the packet contract. `delta_time`, `look_x`, `look_y`, `recenter`, `reset`, `yaw`, `pitch` and `distance` are input. Write `output_yaw`, `output_pitch`, `output_distance`, then `apply` to request a change. Angles are radians. Retaining the pointer after return is not allowed. [[S03]](#source-s03)

### First-person fields

The separate packet supplies timing, look inputs, recenter/reset and yaw/pitch. It has output yaw/pitch and `apply`, not the orbit packet's distance fields. Call the opt-in during setup and use the matching callback. The host limits the requested pitch and preserves the original transition/control path. [[S01]](#source-s01) [[S03]](#source-s03)

### Extra audited runtime imports

The pinned runtime additionally registers config String read/free, mod version, dependency-status and path/save-related imports. Their existence is visible in `librecomp/src/mod_config_api.cpp`, but not all are declared in the small Rocket convenience header. Treat each as an additional audited contract before using it; do not infer arbitrary filesystem write capabilities or universal mod settings from a returned path. [[S20]](#source-s20)

For the tutorial, use only what you need. A path getter is not a general file API. A dependency-status getter is not a catalogue of all mods. A return-value hook accessor is not a generic register editor.

## Recompiler section names

| Section form | Purpose |
| --- | --- |
| `.recomp_callback.*:event_name` | Callback for an exported base-host event. |
| `.recomp_import.*` | Base-host import placeholders. |
| `.recomp_import.mod_id` | Imports from a named mod dependency. |
| `.recomp_export` | Functions exported by this mod. |
| `.recomp_event` | Mod event definitions in the underlying tool contract. |
| `.recomp_hook.function_name` | Hook at a referenced function's entry. |
| `.recomp_hook_return.function_name` | Hook at its return. |
| `.recomp_patch` / `.recomp_force_patch` | Replacement-related sections; do not bypass host protections. |

The markers `*` and `.` identify the base recomp and self in the underlying dependency contract. These section names come from pinned N64Recomp, not a list of extra Rocket host callbacks. Correct annotations still need correct symbols, dependencies and runtime support. [[S18]](#source-s18) [[S28]](#source-s28) [[S09]](#source-s09)

## A safe own-feature toggle

Keep the package loaded. Read a declared Off/On preference at the feature's appropriate update point. On a transition to Off, restore only what the feature can safely reverse and owns. While Off, keep the checks cheap. On reactivation, acquire fresh valid state rather than reusing a freed scene pointer.

This pattern does not turn an ordinary package into the host-approved standby camera. Package activation and own-feature state remain distinct. [[S06]](#source-s06)

## A safe one-shot action

Baseline stored choices before accepting commands. Detect a deliberate new edge. Recheck the required game state. Post one request at a safe boundary. Observe completion or rejection. Cancel pending work during transition. Require explicit rearming before repeating.

Do not write a saved setting back to Ready behind the host merely to make the UI look convenient unless you have a supported settings write path. The feature-only reference avoids pretending it can generically rewrite its host settings. [[R02]](#source-r02) [[S20]](#source-s20)

## A safe restoration

Save the original reference only while its lifetime is known. Record what your mod installed. Before restoring, verify the current field is still yours and the original is still valid for that owner/scene. If the scene already released the object, discard private bookkeeping rather than write through the stale pointer.

This is a general design recommendation; the exact implementation depends on the game's ownership and allocation behaviour. It is not a host service that automatically rolls back arbitrary memory changes.

## FAQ

### Do players need to build Rocket-R to use a mod?

Not for an ordinary compatible self-contained `.nrm` or `.rtz`. Developers need the relevant tools and references to produce those packages. A feature depending on a new host API is a different case and must declare that host requirement. [[S01]](#source-s01) [[S02]](#source-s02)

### Can I just rename a ZIP to `.nrm`?

Only if that archive already has the correct package content and metadata. Renaming does not compile C, create symbol tables or register hooks. Use the package pipeline. [[S04]](#source-s04) [[S17]](#source-s17)

### Why is an On/Off option a dropdown?

Because this frontend renders Enum options as combos. There is no automatic coloured-toggle schema type here. A standalone game-rendered UI or a host UI change is a separate implementation. [[S05]](#source-s05)

### Why do changes stop when I untick Enabled next launch?

For an ordinary running package, the audited live-forwarding condition also checks the selected entry's enabled state. Keep it selected while using an internal live effect switch. [[S04]](#source-s04) [[S10]](#source-s10)

### Can a menu change all other mods' settings?

Not through a generic public interface identified in this build. The current getters are per-calling-mod, and the host's validated write/persistence path is not a universal exported guest service. A cooperative interface or new host connection must be clearly distinguished from that absent general facility. [[S20]](#source-s20) [[S06]](#source-s06)

### Are all original functions safe to hook?

No. Some are protected, some may lack usable generated/reference information, and an unprotected function can still be the wrong state/thread/boundary. Check the actual symbol and policy, then verify its contract. [[S08]](#source-s08) [[S09]](#source-s09) [[S28]](#source-s28)

### Can I use the included camera's ID as a starting point?

Copy its structure, not its identity. The builder has special handling for `rocket_modern_camera`, and the host approves a specific included camera hash. Use a unique ID and declare your own resource requirements. [[S02]](#source-s02) [[S06]](#source-s06)

### Is the File Select fix a public save-loaded API?

No. It is a baseline-specific predicate from the reference implementation with documented earlier control-flow evidence and synthetic regression tests. Reuse it knowingly and revalidate it when the target changes. [[R03]](#source-r03)

### Can I make new levels or models?

There is no automatic general importer in the current mod workflow. Such a project needs the game's formats, loading code and runtime integration. That missing work is not a claim that the idea is permanently impossible. [[S01]](#source-s01)

### Why does a texture pack import but change nothing?

Archive/metadata validity and texture identity matching are separate stages. Verify one real mapping, the file path, hash/database version and the visible material. Do not mistake a placeholder hash for a usable record. [[S04]](#source-s04) [[S16]](#source-s16)

### Do successful synthetic tests prove the mod works in game?

No. They establish the named properties against the stated fixture/interpreter. Keep them, then run the actual package in the game and record the scenario/platform. [[S14]](#source-s14) [[R01]](#source-r01) [[R02]](#source-r02) [[R03]](#source-r03)

## Glossary

| Term | Meaning in this guide |
| --- | --- |
| ABI | The binary rules for calling functions and arranging values; here the guest O32 contract matters. |
| Guest | The original game's virtual execution/data environment used by compiled mod code. |
| Host | The native Rocket-R executable and its runtime/UI/rendering systems. |
| Manifest | The mod identity, requirements and option definitions carried through packaging. |
| Schema | The structure and allowed values of metadata/settings, not the feature implementation. |
| Callback | A function invoked for a named event. |
| Hook | Code attached to a verified original function boundary. |
| Replacement | Code taking over an original function's implementation. |
| Import/export | A named connection to a host or another mod's callable function. |
| Relocation | Information needed to adjust references when code/data are loaded at another address. |
| RDRAM | The game's memory domain; renderer task snapshots and extended mod memory have different roles. |
| Display list | Commands describing graphics work to the game's graphics pipeline. |
| Segmented address | A graphics address resolved relative to a currently selected segment base. |
| Palette / TLUT | A lookup table supplying colours for indexed texture samples. |
| Mipmap | A smaller texture level used for lower-detail/distant sampling. |
| Ownership | Evidence that the target storage/object belongs to the feature you intend to modify. |
| Lifetime | The interval during which a pointer/resource is valid for the intended use. |
| Launch snapshot | The host's recorded set of selected packages/settings for that running session. |
| Hash | A content fingerprint; not an author's signature or proof of safety. |
| Regression test | A retained test for behaviour that must not break again. |

## The final rule

Make one change, prove each stage, preserve the original behaviour you do not own, and write down the limits. That is how we get useful mods instead of packages that import perfectly and do absolutely nothing.



---

<a id="release-checklist"></a>

# Before you release a Rocket-R mod

Record **host version/commit, mod ID/version/hash, compiler/linker/tool revisions and tested platforms**.

## Package

- [ ] Unique stable ID; friendly name and accurate author/credits.
- [ ] New package version for the release; correct minimum Rocket-R requirement.
- [ ] Valid final `mod.json`; paired code/symbol files where applicable.
- [ ] Correct Rocket metadata, dependencies, conflicts and exclusive resources.
- [ ] No unsafe/case-colliding paths, native executable payloads, ROMs or extracted commercial assets.
- [ ] Actual final package imported and launched; not just a successful compiler run.

## Feature

- [ ] Correct callback/hook and real target found; bounded diagnostics available.
- [ ] Minimum, maximum, neutral, Off and restore paths tested.
- [ ] Shared data and unrelated flags preserved according to the documented contract.
- [ ] No per-frame unbounded allocations or logging.
- [ ] One-shot actions do not replay from old saved selections.
- [ ] Known native side effects and irreversible actions explained.

## Lifecycle and controls

- [ ] Title/File Select/preview/erase paths remain unaffected.
- [ ] Loading a save, death/reload, level changes and returning to the frontend checked.
- [ ] Held shortcuts across transitions cannot unexpectedly open a menu.
- [ ] Input release/cancellation and host-overlay/focus interactions checked.
- [ ] Menu pause/non-pause behaviour and mapped-input limits explained.

## Settings and compatibility

- [ ] Live changes actually reach the running feature.
- [ ] Selected profile/package mismatch cannot alter the wrong session.
- [ ] Normal restart preserves preferences.
- [ ] Old-profile upgrade and invalid old values handled/documented.
- [ ] Important mod combinations tested with version/hash records.
- [ ] Package unloading is not confused with an internal effect toggle.

## Evidence and delivery

- [ ] Source checks, package checks, synthetic tests and gameplay tests labelled separately.
- [ ] Tested scenario/platform list supplied; untested platforms identified.
- [ ] Source, build instructions, relevant notices and known issues included.
- [ ] Rollback package/instructions kept; save backup recommendation included where appropriate.



---

<a id="validation"></a>

# Validation record — edition 1.0

**Documentation baseline:** Rocket-R 1.0.1 / API 1, commit `37b387e38a04673c1ea346fee98cb17b4ed1eba8`. Audit date: 29 September 2026. Pinned-source locators are in [SOURCES.md](#sources).

This record separates checks performed for this handbook from historical observations. A passing test here is not a claim of a new gameplay session.

## New starter projects

All four source projects passed **ELF32 big-endian MIPS compilation and linking** using the available Clang 17.0.0 and LLD. Native mock tests passed **1,530 assertions** with AddressSanitizer and UndefinedBehaviorSanitizer enabled in this Linux environment. They cover packet validation, finite/range checks, live reads, enum handling, effect-off behaviour and the cooperating provider/client calculation.

| Project | Native logic checks | MIPS compile/link | Official package/import/gameplay |
| --- | --- | --- | --- |
| Ready Probe | Passed | Passed | Not performed |
| Simple Camera | Passed | Passed | Not performed |
| Speed Provider | Passed | Passed | Not performed |
| Camera Client | Passed | Passed | Not performed |

The provider/client native test links the two implementations using normal C names. It does **not** exercise the runtime's mod-dependency resolver. The MIPS link permits unresolved references for the packaging stage, matching the maintained workflow; link success does not validate those references.

The repository documents Clang 18 under Ubuntu 24.04 as its tested guest compiler. Our Clang 17 checks are additional evidence, not a replacement for that documented baseline. The complete generated Rocket symbol dump and official RecompModTool were not available for this edition's new lessons. Accordingly, **no new lesson `.nrm` files are supplied**. The instructions use the maintained builder on a prepared checkout to produce them.

Reproduce the available checks with `python tools/check_lessons.py`. The detailed result is [validation/lesson_checks.json](validation/lesson_checks.json). Build the actual packages separately using [Start Here](START_HERE.md).

## Supplied reference projects

The supplied complete test runners were rerun for all three reference projects. Their runners rebuilt the MIPS packages using their own included narrow packers, ran source checks and packaged-MIPS fixture tests, and verified reproducible rebuilding. The test runners completed successfully.

| Reference | Rerun log | Important scope limit |
| --- | --- | --- |
| Colour Studio 1.0.0 | [colours_recheck.log](validation/colours_recheck.log) | Synthetic materials/memory; no new rendered gameplay check. |
| Cheats 1.0.0 | [cheats_recheck.log](validation/cheats_recheck.log) | Synthetic records and simulated original handlers; no new game session. |
| Standalone Cheat Menu 3.0.1 | [menu_recheck.log](validation/menu_recheck.log) | Synthetic state/input and software display-list previews; not RT64 gameplay. |

These are **not** three runs of official RecompModTool. The references implement a narrower packaging path suitable for their own verified symbols, hooks and code. Their local MIPS fixture interpreters do not exercise the host's live recompiler, hook interposition, SDL input or GPU driver.

The optional Windows collector source has been preserved with the menu reference so its original checker-update test remains reproducible. The collector was not executed against a live Windows process for this handbook. It is a diagnostic tool, not a runtime dependency of any mod or lesson.

## Historical game observations

The project creator reported that Colour Studio's corrected v0.4.1 worked, and subsequently described the expanded Colour Studio as working as intended. The original cheat activation package and standalone menu v3.0.0 were also reported working. The standalone menu was then reported to break File Select when opened there.

Version 3.0.1 contains a regression-tested gate intended to address that issue. **This conversation does not establish an actual gameplay confirmation of that exact hotfix.** The feature-only Cheats split likewise does not gain gameplay confirmation merely because its earlier combined implementation worked. The new teaching projects have no historical game result.

The supplied Colour Studio 0.4.0 report shows increasing ticks, received settings and repeated ownership rejections. Its read-only samples were non-atomic. A selected excerpt is preserved in [validation/diagnostic_excerpt.txt](validation/diagnostic_excerpt.txt); no original texture dump or raw RAM image is included.

## Documentation and publishing checks

`tools/check_docs.py` checks chapter presence, citation IDs, local links, required source projects, package hygiene and document text. `tools/build_docs.py` generates the unified Markdown, offline HTML and the two PDF editions from the maintained sources. The publishing check report is [validation/documentation_checks.json](validation/documentation_checks.json).

The PDF editions are rendered and visually inspected for cover, contents, tables, code blocks and source references. Visual/document-layout checking is not an additional test of the mods. The offline reader is self-contained for its chapter text, styling, scripts and preview images; external source links still need a connection, and linked source projects need the rest of this ZIP.

## Explicitly not performed

No complete Rocket-R executable was built for this documentation. No ROM was requested or bundled. No new in-game launch, Windows/Steam Deck/Android device session, native GPU performance test or official-loader import test of the new lessons was performed. No universal mod-list or cross-mod settings backend was implemented. No repository commit, push or file replacement was made.



---

<a id="sources"></a>

# Sources and provenance

Source facts refer to the pinned files below, not to a moving upstream branch. The locator identifies the relevant function, structure or documentation section. `S` entries are repository sources; `R` entries are supplied reference implementations; `D` entries are recorded diagnostic observations.

The chapter narration distinguishes those sources from recommended designs and tests still to be performed.

<a id="source-s01"></a>
## S01 — Current modding guide
[ThatGuyMcd/Rocket-R / `docs/modding.md`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/docs/modding.md)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** Making a code mod; Profiles and saves; Compatibility limits.

<a id="source-s02"></a>
## S02 — Maintained mod builder
[ThatGuyMcd/Rocket-R / `scripts/build_mod.py`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/scripts/build_mod.py)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** main: compiler flags, top-level *.c scan, additional_files, stable ZIP output.

<a id="source-s03"></a>
## S03 — Rocket API 1 header
[ThatGuyMcd/Rocket-R / `modding/include/rocket/mod.h`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/modding/include/rocket/mod.h)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** ROCKET_CALLBACK, ROCKET_IMPORT, camera packet structures.

<a id="source-s04"></a>
## S04 — Package/profile implementation
[ThatGuyMcd/Rocket-R / `src/mods/mod_library.cpp`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/src/mods/mod_library.cpp)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** inspect_package, check_option, set_option, resolve_locked, prepare_launch.

<a id="source-s05"></a>
## S05 — Mod settings UI
[ThatGuyMcd/Rocket-R / `src/mods/mod_ui.cpp`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/src/mods/mod_ui.cpp)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** settings; running_package versus enabled selection.

<a id="source-s06"></a>
## S06 — Host runtime integration
[ThatGuyMcd/Rocket-R / `src/mods/mod_runtime.cpp`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/src/mods/mod_runtime.cpp)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** configure_library, register_api, game_ready, camera callbacks.

<a id="source-s07"></a>
## S07 — Pinned dependency revisions
[ThatGuyMcd/Rocket-R / `dependencies.lock.json`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/dependencies.lock.json)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** dependencies array.

<a id="source-s08"></a>
## S08 — Protected-function generation
[ThatGuyMcd/Rocket-R / `scripts/generate_recomp_config.py`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/scripts/generate_recomp_config.py)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** write_mod_protection.

<a id="source-s09"></a>
## S09 — Runtime protection/profile patch
[ThatGuyMcd/Rocket-R / `patches/n64-modern-runtime/0013-profile-mod-paths-and-protected-hooks.patch`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/patches/n64-modern-runtime/0013-profile-mod-paths-and-protected-hooks.patch)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** regenerate_with_hooks; replacements; profile paths.

<a id="source-s10"></a>
## S10 — Mod-library regression tests
[ThatGuyMcd/Rocket-R / `tests/mods_tests.cpp`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/tests/mods_tests.cpp)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** live option types, updates, version isolation, recovery.

<a id="source-s11"></a>
## S11 — Runtime architecture
[ThatGuyMcd/Rocket-R / `docs/ARCHITECTURE.md`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/docs/ARCHITECTURE.md)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** Threads and rendering; Audio, saves and input.

<a id="source-s12"></a>
## S12 — Preparing a full development checkout
[ThatGuyMcd/Rocket-R / `docs/BUILDING.md`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/docs/BUILDING.md)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** Windows build; Files produced.

<a id="source-s13"></a>
## S13 — Project development rules
[ThatGuyMcd/Rocket-R / `docs/DEVELOPMENT.md`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/docs/DEVELOPMENT.md)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** Where changes belong; Audio and interpolation.

<a id="source-s14"></a>
## S14 — Testing and platform evidence
[ThatGuyMcd/Rocket-R / `docs/TESTING.md`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/docs/TESTING.md)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** Source checks; Desktop regression tests; Platform coverage.

<a id="source-s15"></a>
## S15 — Project licence notice
[ThatGuyMcd/Rocket-R / `LICENSE.md`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/LICENSE.md)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** Project-authored glue/build code; separate component rights.

<a id="source-s16"></a>
## S16 — Pinned RT64 texture-pack guide
[rt64/rt64 / `TEXTURE-PACKS.md`](https://github.com/rt64/rt64/blob/6f1c2d99a4ea571c139f449c326fd176ba8f3496/TEXTURE-PACKS.md)

Pinned revision: `6f1c2d99a4ea571c139f449c326fd176ba8f3496`.

**Locate:** Configuration; hashing; packing; operations.

<a id="source-s17"></a>
## S17 — Official package producer
[N64Recomp/N64Recomp / `RecompModTool/main.cpp`](https://github.com/N64Recomp/N64Recomp/blob/81213c1831fab2521a6a5459c67b63437d67e253/RecompModTool/main.cpp)

Pinned revision: `81213c1831fab2521a6a5459c67b63437d67e253`.

**Locate:** Manifest parsing; ELF processing; mod.json/mod_syms.bin/mod_binary.bin.

<a id="source-s18"></a>
## S18 — Recompiler section contracts
[N64Recomp/N64Recomp / `include/recompiler/context.h`](https://github.com/N64Recomp/N64Recomp/blob/81213c1831fab2521a6a5459c67b63437d67e253/include/recompiler/context.h)

Pinned revision: `81213c1831fab2521a6a5459c67b63437d67e253`.

**Locate:** Special section names; dependency names; relocation types.

<a id="source-s19"></a>
## S19 — Hook context handling
[N64Recomp/N64ModernRuntime / `librecomp/src/mod_hooks.cpp`](https://github.com/N64Recomp/N64ModernRuntime/blob/ae1ffbb909d9f93c88c41830deb539f7feef5ed2/librecomp/src/mod_hooks.cpp)

Pinned revision: `ae1ffbb909d9f93c88c41830deb539f7feef5ed2`.

**Locate:** run_hook; return accessors; finish_hook_setup.

<a id="source-s20"></a>
## S20 — Per-mod configuration imports
[N64Recomp/N64ModernRuntime / `librecomp/src/mod_config_api.cpp`](https://github.com/N64Recomp/N64ModernRuntime/blob/ae1ffbb909d9f93c88c41830deb539f7feef5ed2/librecomp/src/mod_config_api.cpp)

Pinned revision: `ae1ffbb909d9f93c88c41830deb539f7feef5ed2`.

**Locate:** recomp_get_config_*; free_config_string; register_config_exports.

<a id="source-s21"></a>
## S21 — Game controller reads
[RocketRet/Rocket-Robot-On-Wheels / `src/rocket/codeseg2/codeseg2_169.c`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/src/rocket/codeseg2/codeseg2_169.c)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** read_controller_noblock; get_controller_data placeholder; clear_buttons_pressed.

<a id="source-s22"></a>
## S22 — Native cheat helpers
[RocketRet/Rocket-Robot-On-Wheels / `src/rocket/codeseg2/codeseg2_171.c`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/src/rocket/codeseg2/codeseg2_171.c)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** func_8004EE84, func_8004EE5C; dispatcher placeholder.

<a id="source-s23"></a>
## S23 — Game render submission order
[RocketRet/Rocket-Robot-On-Wheels / `src/rocket/codeseg2/codeseg2_332.c`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/src/rocket/codeseg2/codeseg2_332.c)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** func_8007F22C.

<a id="source-s24"></a>
## S24 — Game structures
[RocketRet/Rocket-Robot-On-Wheels / `include/types.h`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/include/types.h)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** ControllerData; GameObject; MaterialGfx; Texture; GfxContext.

<a id="source-s25"></a>
## S25 — GBI definitions used by game
[RocketRet/Rocket-Robot-On-Wheels / `include/2.0I/PR/gbi.h`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/include/2.0I/PR/gbi.h)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** F3DEX2 commands; texture format/size definitions.

<a id="source-s26"></a>
## S26 — Included camera manifest
[ThatGuyMcd/Rocket-R / `modding/examples/modern-camera/mod.toml`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/modding/examples/modern-camera/mod.toml)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** manifest and config_options.

<a id="source-s27"></a>
## S27 — Maintained guest linker script
[ThatGuyMcd/Rocket-R / `modding/mod.ld`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/modding/mod.ld)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** RAMBASE, extram and discarded sections.

<a id="source-s28"></a>
## S28 — Runtime loading/resolution
[N64Recomp/N64ModernRuntime / `librecomp/src/mods.cpp`](https://github.com/N64Recomp/N64ModernRuntime/blob/ae1ffbb909d9f93c88c41830deb539f7feef5ed2/librecomp/src/mods.cpp)

Pinned revision: `ae1ffbb909d9f93c88c41830deb539f7feef5ed2`.

**Locate:** load_mod_code; resolve_code_dependencies; hooks/replacements.

<a id="source-s29"></a>
## S29 — Game loop ordering
[RocketRet/Rocket-Robot-On-Wheels / `src/rocket/codeseg0/codeseg0.c`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/src/rocket/codeseg0/codeseg0.c)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** func_80001248.

<a id="source-s30"></a>
## S30 — Game startup callback timing
[ThatGuyMcd/Rocket-R / `src/game_registration.cpp`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/src/game_registration.cpp)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** RunRocketEntrypoint; InitialiseEntrypointContext.

<a id="source-s31"></a>
## S31 — Checked host guest-code policy
[ThatGuyMcd/Rocket-R / `runtime-recomp/rocket.us.recomp-policy.json`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/runtime-recomp/rocket.us.recomp-policy.json)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** functionHooks, instructionPatches, stubs.

<a id="source-s32"></a>
## S32 — Native input routing
[ThatGuyMcd/Rocket-R / `src/platform.cpp`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/src/platform.cpp)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** sample_input; pump_runtime_events.

<a id="source-s33"></a>
## S33 — Native overlay/input UI
[ThatGuyMcd/Rocket-R / `src/runtime_ui.cpp`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/src/runtime_ui.cpp)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** draw; handle_runtime_event; shortcut capture.

<a id="source-s34"></a>
## S34 — Graphics commands
[RocketRet/Rocket-Robot-On-Wheels / `src/rocket/codeseg2/codeseg2_150.c`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/src/rocket/codeseg2/codeseg2_150.c)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** draw_rectangle; update_gfx_context; clear_depth_buffer.

<a id="source-s35"></a>
## S35 — Texture material loading
[RocketRet/Rocket-Robot-On-Wheels / `src/rocket/codeseg2/codeseg2_406.c`](https://github.com/RocketRet/Rocket-Robot-On-Wheels/blob/cd1fc6d3f575841a240373d7b8a2baf532da2f9f/src/rocket/codeseg2/codeseg2_406.c)

Pinned revision: `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`.

**Locate:** load_textured_material; load_texture.

<a id="source-s36"></a>
## S36 — Third-party notices
[ThatGuyMcd/Rocket-R / `THIRD_PARTY.md`](https://github.com/ThatGuyMcd/Rocket-R/blob/37b387e38a04673c1ea346fee98cb17b4ed1eba8/THIRD_PARTY.md)

Pinned revision: `37b387e38a04673c1ea346fee98cb17b4ed1eba8`.

**Locate:** Component-specific credits and licences.

<a id="source-r01"></a>
## R01 — Rocket_Colour_Studio_1.0.0
[Rocket_Colour_Studio_1.0.0](reference_projects/Rocket_Colour_Studio_1.0.0/README.md)

Basis: supplied reference implementation. Included path: `reference_projects/Rocket_Cheat_Menu_3.0.1`. Included path: `reference_projects/Rocket_Cheats_1.0.0`. Included path: `reference_projects/Rocket_Colour_Studio_1.0.0`.

**Locate:** Source and tests supplied during this project; not an official SDK API.

<a id="source-r02"></a>
## R02 — Rocket_Cheats_1.0.0
[Rocket_Cheats_1.0.0](reference_projects/Rocket_Cheats_1.0.0/README.md)

Basis: supplied reference implementation. Included path: `reference_projects/Rocket_Cheat_Menu_3.0.1`. Included path: `reference_projects/Rocket_Cheats_1.0.0`. Included path: `reference_projects/Rocket_Colour_Studio_1.0.0`.

**Locate:** Source and tests supplied during this project; not an official SDK API.

<a id="source-r03"></a>
## R03 — Rocket_Cheat_Menu_3.0.1
[Rocket_Cheat_Menu_3.0.1](reference_projects/Rocket_Cheat_Menu_3.0.1/README.md)

Basis: supplied reference implementation. Included path: `reference_projects/Rocket_Cheat_Menu_3.0.1`. Included path: `reference_projects/Rocket_Cheats_1.0.0`. Included path: `reference_projects/Rocket_Colour_Studio_1.0.0`.

**Locate:** Source and tests supplied during this project; not an official SDK API.

<a id="source-d01"></a>
## D01 — Colour Studio 0.4.0 live diagnostic report
[Colour Studio 0.4.0 live diagnostic report](validation/diagnostic_excerpt.txt)

Basis: user-supplied runtime observation. Included path: `validation/diagnostic_excerpt.txt`.

**Locate:** Original report 20260928-113340949, lines 18-21, 79-82 and 132-140; non-atomic samples.

## How to update the source baseline

Record the new host commit and dependency lock first. Diff the public header, export registration, package/schema validation, live-settings routing, protected policy and relevant rendering/input paths. Re-run the examples and add dated gameplay results where available. Update the capability matrix and explanations before changing the edition number. A README version bump alone does not prove every runtime behaviour changed.

