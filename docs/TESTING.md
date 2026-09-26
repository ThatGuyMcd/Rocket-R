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
cmake --build build/windows --target RocketR RocketPresentationTests RocketRuntimeLogTests RocketControlsTests RocketControlsUiTests --parallel
ctest --test-dir build/windows --output-on-failure
```

On Linux, substitute the configured Linux build directory. The standard builders
run these same four suites:

| Suite | What it checks |
| --- | --- |
| `presentation_identity` | 20 cases covering ownership, culling, rotation, projected shadows and secondary transforms. |
| `runtime_log` | Output capture, concurrent writers, bounded history, rotation and shutdown. |
| `controls` | 49 checks for capture, conflicts, swaps and SDL controller polling. |
| `controls_ui` | 78 checks for Controller Studio, Guided Setup, keyboard/controller navigation and responsive layout. |

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

| Platform | Checked so far |
| --- | --- |
| Windows x64 | Release builds and all four suites pass. Gameplay, audio, interpolation and the launcher have been tested. |
| Linux x64 | Release builds and all four suites pass. Gameplay runs smoothly on Steam Deck. |
| Linux ARM64 | Release builds and all four suites pass. Gameplay still needs checking on native hardware. |
| Android ARM64 | Gameplay, speakers and touch controls work on Honor Magic V5. Emulator checks cover startup, gameplay, resume and settings. |

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
