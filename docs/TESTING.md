# Testing

## Source checks

Run these from the repository root with Python 3.11 or newer:

```text
python scripts/self_check.py --root .
python scripts/verify_recomp_policy_v42.py --root .
python scripts/verify_interpolation_v35.py --root .
python scripts/verify_global_interpolation_ui_v36.py --root .
python scripts/verify_shared_mode0_v43_3.py --root .
python scripts/verify_presentation_policy.py --root .
python -m unittest discover -s tests -p "test_*.py"
```

These checks validate the source layout, pinned dependencies, patch hashes and
recompilation policy without needing a ROM. The old version numbers in some
filenames identify checks still used by the current builder.
Add `--with-generated` to the recomp-policy and presentation-policy checks after
the builder has generated the game code. They inspect that output without editing it.

## Desktop regression tests

After configuring the native build, compile all test executables before CTest:

```text
cmake --build build/windows --target RocketR RocketPresentationTests RocketRuntimeLogTests RocketAndroidSupportTests RocketControlsTests RocketControlsUiTests RocketModsTests RocketCameraModTests RocketGraphicsCameraTests RocketSdkServicesTests RocketSdkRuntimeTests RocketSdkWorldTests RocketSdkAudioTests RocketAssetLayersTests RocketSdkCompatibilityTests --parallel
ctest --test-dir build/windows --output-on-failure
```

On Linux, substitute the configured Linux build directory. The standard builders
run these suites. `sdk1_compatibility` is registered when the three published
packages are present in `build/sdk1-compatibility`:

| Suite | What it checks |
| --- | --- |
| `presentation_identity` | Ownership, culling, rotation, projected shadows, secondary transforms, the player's spring-mounted wheel, sky FOV alignment and safe scrolling texture rows. |
| `runtime_log` | Output capture, concurrent writers, bounded history, rotation and shutdown. |
| `android_support` | Vulkan mapped ranges, distinct queue families, snapshot reuse, concurrent task isolation and bounded idle memory. |
| `controls` | Capture, conflicts, swaps, SDL controller polling and camera input ownership. |
| `controls_ui` | 86 checks for Controller Studio, Guided Setup, keyboard/controller navigation, responsive layout, rounded reset confirmation and slider dragging/direct entry. |
| `mods` | Package validation, dependencies, profiles, live settings, release upgrades, save separation and crash recovery. |
| `camera_mod` | Guest pointer handling, callback timing, temporary zoom presets, obstacle-corrected headings and returning controls to the original camera. |
| `graphics_camera` | Close-wall near clipping across FOV/aspect settings, window resizing, restoration of the original camera fields and sky slider values. |
| `sdk_services` | Version requirements, resource ownership, input metadata, saves and cleanup. |
| `sdk_runtime` | MIPS callback descriptors, guest packets, scene/activation boundaries, input edges, events and native setters. |
| `sdk_world` | Meshes, UVs, poses, scene rollback, ownership, collision sweeps/rays and native display-list encoding. |
| `sdk_audio` | PCM decoding, ownership, loops, pan, pause, gain and resampling continuity across output blocks. |
| `asset_layers` | Independent BPS edits, composed target checksum, overlaps and private launch staging. |
| `sdk1_compatibility` | Frozen API 1 and unchanged published camera, cheat-menu and colour packages. |

On Windows, the UI test also accepts a font path:

```text
build\windows\RocketControlsUiTests.exe C:\Windows\Fonts\comic.ttf
```

For native launcher and overlay layout previews without a visible window, use a
separate config folder. In PowerShell:

```powershell
$env:SDL_VIDEODRIVER = 'dummy'
$env:ROCKET_UI_LAYOUT_CHECK = 'D:\Rocket-R\build\ui-native-previews'
& .\build\windows\bin\Release\Rocket-R.exe --config D:\Rocket-R\build\private\ui-check-profile
Remove-Item Env:SDL_VIDEODRIVER, Env:ROCKET_UI_LAYOUT_CHECK
```

This writes 48 BMP previews at desktop, Steam Deck and narrow-window sizes using
the actual native page functions and SDL's software renderer. The Play page uses
an example filename and does not load a ROM. Add `--install-mod <package>` to
include that package's expanded settings in this isolated profile. This checks
layout, not game rendering or device performance. Also check input capture, mod
import and profile management popups in the interactive launcher and overlay.

The Android touch-model test runs with JDK 17:

```text
javac -d build/touch-tests packaging/android/app/src/main/java/com/rocketret/rocketr/TouchControlsModel.java tests/TouchControlsModelTest.java
java -cp build/touch-tests com.rocketret.rocketr.TouchControlsModelTest
javac -d build/touch-tests packaging/android/app/src/main/java/com/rocketret/rocketr/DiagnosticsReport.java tests/DiagnosticsReportTest.java
java -cp build/touch-tests com.rocketret.rocketr.DiagnosticsReportTest
```

## Release packages

`scripts/scan_release.py` rejects ROM files and N64 ROM headers in a staged package.
`scripts/verify_release.py` checks the final archives, CPU architectures, current
documentation, Windows GUI subsystem, Android library alignment, and matching
AppImages inside the portable archives. It writes the package/source hashes
to `dist/Rocket-R-<version>-build-info.json` and a SHA-256 file list.
APK signing is verified by the Android builder with `apksigner`.

## Platform coverage

### Performance checks

Use a separate config folder and the same resolution, frame-rate target, mods and
level for before/after measurements. Let startup and shader compilation settle
before recording CPU use. Check Play, Graphics, Controls and Mods, including
controller navigation, binding capture, drag-and-drop, resizing and fullscreen.
Check that clicks still work after leaving the launcher idle and that closing
an idle or minimised launcher responds promptly.

On Linux, check the log's launcher renderer line, then press Play and check the
Vulkan preflight and first game presentation. Repeat with SDL_RENDER_DRIVER set
to `software` to exercise the fallback. WSL's llvmpipe renderer is useful for
startup and CPU tests; it cannot establish native Intel or Steam Deck GPU speed.

Set `ROCKET_PERFORMANCE_TRACE=1` before starting the game to record frame rate,
target rate, resolution scale and average/maximum display-list decode time every
four seconds. You can also enable **Record performance log** under Graphics →
Diagnostics while playing. That page shows the actual graphics device.
CPU software rendering needs a working GPU driver before resolution comparisons
can serve as hardware performance measurements.

### Android graphics candidate

The current Android changes address missing cache maintenance on non-coherent
Vulkan memory and resource sharing between distinct GPU queue families. The
native tests exercise the range and ownership policy helpers, but cannot prove
that every driver's rendering faults are fixed. Compare a device's current
release against the candidate in the same level after shader compilation settles.

For an existing install, select **Low power** in Graphics and restart the game.
Updates keep saved choices, so installing the APK alone does not select the new
preset. Check native resolution first, then Original 2X and 60 FPS separately.
Check shadows, transparent surfaces, pickups, sky scrolling, Workshop, camera
movement, opening Settings, rotation, background/resume and sound. Record FPS
and CPU/GPU load after a few minutes, including whether performance worsens as
the device heats up.

To report a fault without USB debugging, enable **Record performance log**,
reproduce it for around 30 seconds, then return to the Android launcher and tap
**Save diagnostics**. Attach that ZIP, a screenshot or short clip, the level and
reproduction steps to the bug report. The ZIP contains device details, graphics
settings and up to three recent logs; it excludes ROMs and saves. Older logs over
1 MiB contribute their latest 1 MiB. Graphics → Diagnostics → Copy log also works
from the overlay. Vulkan startup messages include the device, driver/API version
and memory properties.

There is no affected physical Android device connected to the development PC.
Desktop tests, cross-compilation and APK checks do not establish mobile GPU
performance or confirm that the users' visual faults are gone.

The matching benchmark compares the original and optimised bridge using the same
120-frame streams at 64, 256 and 1,024 objects. It includes cull gaps, repeated
owners, ambiguous matrices and direct/specific transforms. Matching digests must
agree, along with the existing wheel, shadow and sky regression scenarios.
This measures matching cost; it is not a whole-game frame-rate benchmark.

### SDK 2 development checks

Published fixture hashes are pinned in `tests/sdk_compatibility_tests.cpp`.
Keep those three original packages in `build/sdk1-compatibility`; do not rebuild
them for the compatibility check. Run the compatibility executable with the
fixture folder and an optional Workshop `.nrm` to stage a mixed profile.
The Windows mixed test session has loaded all four packages. This confirms the
loader/import path; it does not confirm every gameplay interaction.

For Workshop, test the normal marker/boost mode, then enable Workshop arena in
a real level. Walk around both rooms, collect gears, collide with walls and the
patrol, reset progress, change settings, disable/enable live, change levels and
restart. Check that native rendering/music return and that Original Game saves
remain separate. Run this on Windows, Steam Deck, native ARM64 Linux and Android
before promoting SDK 2 to a stable release. Custom arena gameplay and lower-end
device performance are not yet confirmed.

### Earlier release coverage

Windows gameplay checks cover the modern camera, live settings, ground and
corridor behaviour, sky movement and Rocket's wheel. The camera code is unchanged
from the approved development package. The native sky filter was checked at 0%
and 100% on Windows: the sky stays smooth during fast camera movement and the
previous GPU-load increase is gone. Its performance and fast-motion behaviour
still need a Steam Deck check.

The current desktop builders run the expanded SDK regression suites. Android is
built and signature-checked, with a separate Java touch-input test. Package
checks are separate from the device results below.

| Platform | Checked so far |
| --- | --- |
| Windows x64 | Gameplay, audio, interpolation, the launcher and the final camera/sky/wheel changes have been checked on Windows. |
| Linux x64 | The earlier release ran smoothly on Steam Deck. The 1.0.2 package needs a fresh check on the device. |
| Linux ARM64 | Builds and CPU tests pass under QEMU. Gameplay still needs checking on native ARM64 hardware. |
| Android ARM64 | Earlier builds passed gameplay, speaker and touch checks on Honor Magic V5, plus emulator startup/resume checks. The 1.0.2 package needs a fresh device check. |

The 1.0.0 header correction was checked in the Windows launcher and Android
overlay. All five overlay pages and all eight Graphics/Controls sections were
checked at 1280x720. The page headers stayed aligned and the tab rows wrapped
consistently. Android Back returned to the game.

The 1.0.2 blue launcher layout was approved on Windows. The shared native
launcher and overlay also have 48 offscreen previews covering all six pages at
four window sizes. The overlay panels composite to 75% opacity, with opaque
text, controls and modals. Sliders retain dragging, navigation and direct entry.
These layout checks do not establish GPU performance on another device.

Budget Android phones with 3 GB RAM remain a target, not a tested minimum.
Linux ARM64 under QEMU reached Vulkan initialization but did not show a playable
scene. WSL software-rendered performance is not a native Linux GPU result.

CPU tests do not replace visual gameplay or listening checks. For rendering
changes, check the Hot Dog car in Clowney Island, Rocket's and vehicle shadows,
collectibles, camera movement and scene transitions. For audio changes, listen
at full volume and check that playback stays smooth.

For the sky, compare Original 30 FPS with Display refresh rate while slowly
tilting the camera up and down. Try faster turns, scene changes and both ends of
the camera's pitch range. The background should follow smoothly without seams
or colours wrapping from the opposite edge. In Graphics → Image quality, compare Sky
dithering reduction at 0%, 50% and 100%. Check the dark blue sky, lighter bands,
stars and cloud edges, then confirm Rocket, the level and the HUD stay sharp.
At 100%, move the camera quickly up and down and check for flashes around the
colour bands. Repeat at 0% to distinguish source dithering from scrolling errors.
Compare GPU load and frame pacing at 0% and 100%, especially on Steam Deck. The
filter runs on the small source texture and should not add a resolution-dependent
sampling cost. Check changes while paused, resumed and moving between levels.
Earlier Windows visual approval did not catch the expensive per-pixel filter's
performance and flicker on Steam Deck; the cached replacement needs a device check.
Repeat at different resolutions and FOV settings, and check that the slider
value survives restarting. In the shore area, slowly tilt the camera at the
original FOV and at +40: the sky horizon should move with the distant scenery.
Test Rocket's wheel while rolling slowly, accelerating,
turning, stopping and jumping; also recheck the Hot Dog car and both shadows.

For close-wall clipping, check the Clowney Island interior at the original FOV
and at +40, in 4:3 and widescreen. Approach walls and ceilings, including corners,
with the camera mod both on and off. Check first person and all three zoom levels.
The wider view moves the near clipping plane closer without changing camera
movement. CPU tests cover the projection geometry, and the reported wall-clipping case
was checked in gameplay. When changing this path, check other rooms for holes,
flickering surfaces and distant depth precision.

For the camera mod, test in a level rather than the save-selection area. Orbit
in both directions, look up and down, and try walls, corners, low ceilings and
moving objects. The camera should remain stable when the right stick is released.
Also check zoom changes, first-person mode, vehicles, scripted scenes and
returning to normal gameplay. Check that entering and leaving a corridor keeps
the camera on the same side of Rocket. Fixed views should retain their original
look controls. Walk through the full length of a tight corridor
and check that the camera follows Rocket instead of remaining at the entrance.
The bridge tests check how the mod and game exchange data; they do not prove
that every obstacle behaves correctly in a real level.

Lower the camera into flat ground and slopes, keep turning downward, then move away. It should hold a safe elevation without zooming into Rocket or holding its position behind him. Repeat under a low ceiling and at all three zoom levels.

With Stick response at 30, compare Camera momentum at 0, 25, 50 and 100. Low values should start and stop promptly; high values should add glide. Change it during gameplay, try both camera views, and orbit near walls and through tight corridors at each setting. Switch the mod off to check the original camera still behaves normally.

Check that the same horizontal input turns consistently in third and first person, with horizontal inversion both off and on. Orbit away from Rocket's back, then recenter with U, right-stick click and middle mouse. It should return behind him at the current zoom, still avoiding walls. Repeat while moving the mouse and with momentum enabled.

With the included camera mod, check all three zoom distances in an open area and switch
into and out of first person using both default and remapped buttons. Holding
zoom should not cycle repeatedly. Test right-stick and mouse look in both views,
including all four combinations of horizontal and vertical inversion. Check
that first-person pitch stops at the original limits and that returning to
third person resumes smooth, collision-aware movement. On Android, check LOOK,
ZOOM and VIEW while moving with the left touch stick. These mode changes need
in-game testing as well as the automated input, mod logic and bridge checks.
Change inversion, speed, response, momentum and deadzone in the overlay without restarting;
the next camera movement should use the new values in either view. Options for a
different profile or package version must not change the currently running game.
Start with the included camera mod installed but disabled. Enable it in a level,
then turn it off and on again in third person, first person and a corridor.
Original controls should return while it is off, and re-enabling should resume
from the current view. Change its settings while off and check that they are
used when enabled. Check that the switch choice is kept after restarting.

Sweep the mouse quickly in one direction for several full turns, then reverse
and release it. Check both camera views with momentum at 0, 50 and 100. At 0,
there should be no queued mouse spin after release. Higher values should glide
without fighting a deliberate change of direction. Check rapid off/on requests
while settings are open and verify the displayed state when returning to play.

Bind mouse clicks and wheel directions in both camera settings and Controller
Studio. Enable movement capture before assigning a mouse direction. The click
used to open a prompt must not bind itself, and Cancel/Remove must stay clickable.
Check persistence after restarting, conflict choices, focus loss and Test Inputs.
