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
cmake --build build/windows --target RocketR RocketPresentationTests RocketRuntimeLogTests RocketControlsTests RocketControlsUiTests RocketModsTests RocketCameraModTests RocketGraphicsCameraTests --parallel
ctest --test-dir build/windows --output-on-failure
```

On Linux, substitute the configured Linux build directory. The standard builders
run these same seven suites:

| Suite | What it checks |
| --- | --- |
| `presentation_identity` | Ownership, culling, rotation, projected shadows, secondary transforms, the player's spring-mounted wheel, sky FOV alignment and safe scrolling texture rows. |
| `runtime_log` | Output capture, concurrent writers, bounded history, rotation and shutdown. |
| `controls` | Capture, conflicts, swaps, SDL controller polling and camera input ownership. |
| `controls_ui` | 78 checks for Controller Studio, Guided Setup, keyboard/controller navigation and responsive layout. |
| `mods` | Package validation, dependencies, profiles, live settings, release upgrades, save separation and crash recovery. |
| `camera_mod` | Guest pointer handling, callback timing, temporary zoom presets, obstacle-corrected headings and returning controls to the original camera. |
| `graphics_camera` | Close-wall near clipping across FOV/aspect settings, window resizing, restoration of the original camera fields and sky slider values. |

On Windows, the UI test also accepts a font path:

```text
build\windows\RocketControlsUiTests.exe C:\Windows\Fonts\comic.ttf
```

The Android touch-model test runs with JDK 17:

```text
javac -d build/touch-tests packaging/android/app/src/main/java/com/rocketret/rocketr/TouchControlsModel.java tests/TouchControlsModelTest.java
java -cp build/touch-tests com.rocketret.rocketr.TouchControlsModelTest
```

## Release packages

`scripts/scan_release.py` rejects ROM files and N64 ROM headers in a staged package.
`scripts/verify_release.py` checks the final archives, CPU architectures, current
documentation, Windows GUI subsystem, Android library alignment, and matching
AppImages inside the portable archives. It writes the package/source hashes
to `dist/Rocket-R-<version>-build-info.json` and a SHA-256 file list.
APK signing is verified by the Android builder with `apksigner`.

## Platform coverage

The approved Windows build used for 1.0.1 has been tested in gameplay, including
the modern camera, live settings, ground and corridor behaviour, sky movement,
dithering reduction and Rocket's wheel. The release changes its version and
package metadata without changing the approved camera or rendering behaviour.

Windows and both Linux architectures run seven regression suites. Android is
built and signature-checked, with a separate Java touch-input test. Package
checks are separate from the device results below.

| Platform | Checked so far |
| --- | --- |
| Windows x64 | Gameplay, audio, interpolation, the launcher and the final camera/sky/wheel changes have been checked on Windows. |
| Linux x64 | The earlier release ran smoothly on Steam Deck. The 1.0.1 package needs a fresh check on the device. |
| Linux ARM64 | Builds and CPU tests pass under QEMU. Gameplay still needs checking on native ARM64 hardware. |
| Android ARM64 | Earlier builds passed gameplay, speaker and touch checks on Honor Magic V5, plus emulator startup/resume checks. The 1.0.1 package needs a fresh device check. |

The 1.0.0 header correction was checked in the Windows launcher and Android
overlay. All five overlay pages and all eight Graphics/Controls sections were
checked at 1280x720. The page headers stayed aligned and the tab rows wrapped
consistently. Android Back returned to the game.

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
or colours wrapping from the opposite edge. In Graphics → Image, compare Sky
dithering reduction at 0%, 50% and 100%. Check the dark blue sky, lighter bands,
stars and cloud edges, then confirm Rocket, the level and the HUD stay sharp.
At 100%, move the camera quickly up and down and check for flashes around the
colour bands. Repeat at 0% to distinguish source dithering from scrolling errors.
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
