# Mods

Rocket-R 1.0.1 supports code mods, RT64 texture packs and separate mod profiles. Modern Analogue Camera 1.0.0 is included. Keep a copy of any save you care about before trying a new gameplay mod.

## Adding a mod

Open **Mods → Installed → Add Mods**, then choose an `.nrm` code mod, an `.rtz` texture pack, or a ZIP containing either format. On Windows and Linux you can also drop the file onto the launcher. Android has a Mods button on its startup screen and uses the Android file picker.

**Browse** includes Modern Analogue Camera. It installs through the same package loader as other mods. The current catalogue is offline; community downloads and automatic updates are not connected yet.

Upgrading from the approved 1.1.0-dev camera package (0.1.13) switches its profiles to the included release package. Profile names, settings, enabled state and saves stay in place; the old package is kept. Other package versions are not switched automatically.

Browse shows the version included with your Rocket-R build. Installing a newer game build does not replace a version chosen in your mod profile. Use **Update Camera Mod**, or **Use Included** in Installed, to switch that profile to the included package. This keeps its settings and saves. During gameplay, Installed also shows the version that is actually running.

Remap camera inputs in **Mods → Installed → Modern Analogue Camera → Details and settings**, alongside the mod's camera options. Look up, down, left, right, recenter, cycle zoom and toggle first person each accept a keyboard or mouse input and a controller button, trigger or stick direction. These bindings apply immediately and are saved on this device. Look inputs take priority over matching N64 bindings while the mod controls the camera. The two mode buttons remain available while the mod is enabled, including when leaving first person. Disabling the mod restores the original controls.

Mouse look has its own sensitivity setting. The middle mouse button recenters by default; choose another button or disable it in the same mod settings. Opening settings or switching away from the game releases the cursor. Horizontal and vertical inversion are separate options and apply to the mouse, stick and keyboard in both camera views.

Default keyboard controls are I/J/K/L to look, U to recenter, O to cycle zoom and P to toggle first person. On a controller, use the right stick to look, right-stick click to recenter, left-stick click to cycle zoom and the top face button to toggle first person. Tap and release a mode button to switch. Zoom uses the game's three original distances and still respects restrictions in tight areas. First person keeps the original viewpoint, look limits and left-stick controls, with right-stick and mouse look added. Android shows a LOOK stick plus ZOOM and VIEW buttons while the mod controls the camera.

Installing your first mod creates **My Mods**. Enable or disable packages in Installed. Open **Details and settings** to adjust a mod's options, select an installed version, or change its order. Dependencies load before the mods that require them. A missing dependency, incompatible version, declared conflict or dependency cycle prevents launch and appears in the Mods page.

Installing or changing packages needs a restart. The included Modern Analogue Camera 1.0.0 has an **Active in this game** switch, including after starting with it disabled. Install it in the selected profile before launching. It waits in standby until you enable it, and returns control to the original camera when disabled. The switch addresses the running package even if you select a different version or profile for next launch. Speed, response, deadzone, momentum and inversion changes apply immediately in either camera view.

**Camera momentum** controls both input glide and the original camera's follow smoothing. At 0 the view follows directly, without added glide. Higher values soften its start and stop; 100 keeps the original follow smoothing and adds the most input glide. **Stick response** separately controls how quickly stick or keyboard look reaches full speed. Set it to 30 for the quickest response. Fast mouse input is bounded before reaching the original camera solver. Keyboard / mouse bindings accept clicks and wheel directions. The input prompt has a separate option for capturing mouse movement.

The in-game overlay shows whether the camera is on or ready in standby. Other packages show **Enabled next launch** when they need a restart. The overlay also identifies the profile that is actually running, even if you select another profile for next time. Options for a different profile or package version are saved for its next launch.

## Profiles and saves

**Original Game** always starts without mods and uses your existing save folder. Each other profile has its own save folder. Creating or duplicating a profile does not copy your original save.

Exported profiles contain package IDs, SHA-256 hashes and settings. They do not include packages or saves. Import one with Add Mods on desktop, then install any missing packages.

If Rocket-R did not close normally, the next launch skips mods and uses Original Game. Review the selected profile in Mods before choosing **Try My Mods Again**. A power cut or forced quit can trigger this too; the message does not mean a mod definitely caused a crash.

Desktop command line options:

```text
Rocket-R --install-mod path/to/mod.nrm
Rocket-R --rom path/to/rocket.us.z64 --launch --without-mods
```

The command line also accepts `--config` to keep a test installation's settings and saves in a separate folder.

## Making a code mod

Start with `modding/examples/modern-camera`. The source, manifest and settings are included. `modding/include/rocket/mod.h` defines Rocket API version 1. It uses the N64 O32 ABI and compiles to MIPS code; N64ModernRuntime translates the package for the host when it loads. A single `.nrm` can be used on x64 and ARM64.

You need Python 3.11 or later, a full Clang build with the MIPS target, LLD, the pinned RecompModTool and a Rocket symbol dump produced by the normal build pipeline. Clang 18 in Ubuntu 24.04 is the tested guest compiler. Visual Studio's bundled Clang does not include the MIPS backend.

From a prepared Windows checkout:

```powershell
python scripts/build_mod.py modding/examples/modern-camera --wsl `
  --tool build/windows/N64ModernRuntime/librecomp/N64Recomp/RecompModTool.exe `
  --symbols build/generated/rocket.functions.dump.toml
```

On Linux, omit `--wsl` and pass the path to your native RecompModTool. `--clang` and `--linker` can select other compiler installations. The package is written to `build/mods`. No ROM is included in the package.

Use a unique lowercase mod ID with letters, numbers, underscores or hyphens. `game_id` must be `rocket`. Give the mod its own version and set `minimum_recomp_version` to `1.0.1` or a later release that provides the features it needs. Required dependencies use `mod_id` or `mod_id:1.2.3`.

Manifest options support Number, Enum and String settings. The manager draws their controls and writes the chosen values to the runtime configuration before launch. It also sends changes to the loaded runtime when the selected profile and package match the running game. The camera example reads its settings during setup and each camera update, so changes take effect without restarting. Read settings again at an appropriate point if your mod supports live changes; a value cached only at startup will stay unchanged until the next launch. Add `"live_settings": true` to `rocket.json` to tell the settings page that your mod applies changes immediately.

Optional `rocket.json` metadata declares the Rocket API version, category, conflicts and exclusive resources. For example, two mods that both own `camera.orbit` cannot be enabled together. These declarations help catch conflicts; they cannot detect every possible interaction between arbitrary code mods.

Live on/off is currently a host feature for the exact camera package included with the build. Its code stays loaded and the host switches its camera hooks and inputs at a safe update boundary. Disabled third-party packages are not preloaded. The camera cannot wait in standby alongside a conflicting camera mod, and a mod required by another running mod cannot be switched off live. Original Game and recovery mode still load no mods.

## Rocket API version 1

| Event or import | Purpose |
| --- | --- |
| `rocket_on_game_ready` | Runs after mod code, the recomp heap and save handling are ready, before the game entrypoint. |
| `rocket_on_camera_update` | Supplies a temporary `RocketCamera` packet for the supported main-camera update. |
| `rocket_on_mouse_look` | Supplies a `RocketMouseLook` packet with angular deltas immediately before the camera callback. It does not change the original camera packet. |
| `rocket_on_first_person_update` | Supplies a separate 48-byte `RocketFirstPersonCamera` packet during the original first-person input update. |
| `rocket_enable_mouse_look` | Enables mouse capture for a mod that handles the mouse event. Capture still respects the user's setting, focus and the overlay. |
| `rocket_set_camera_smoothing` | Sets native follow smoothing from 0 (direct) to 100 (original). Optional; older mods retain the original smoothing. Applies only while the mod owns a supported camera view. |
| `rocket_enable_first_person_look` | Opts into the first-person event. Mods that do not call this keep the original first-person input. |
| `rocket_claim_analogue_camera` | Requests the dedicated camera input path and mode buttons. Look inputs replace matching N64 inputs after an orbit update is accepted or during supported first-person look. |
| `recomp_get_config_double` | Reads a numeric option for the calling mod. |
| `recomp_get_config_u32` | Reads an enum's zero-based selection index or an integer value. |

Do not retain the camera packet pointer after its callback returns. Check its API version and size. Input values and timing are read-only. Write the output angles and distance, then set `apply` to request a change. Angles are radians; Rocket's vertical axis is Z.

The host passes the requested angles to Rocket before its original obstacle checks. If a low view is blocked, it first tries the last safe elevation and then higher candidates without changing zoom. If those do not fit, it tries a closer position at the requested angle before letting the game try other angles. Every attempt uses the original checks. The camera holds its shorter distance briefly, then eases back out when there is room. The game still checks and chooses the final position. The optional smoothing API can reduce the native spring lag before the final geometry checks; it does not replace the published camera position.

Each callback receives the game's checked yaw and pitch, including avoidance corrections; use those angles as the starting point for movement. Heading locks within the normal third-person camera keep analogue input active. The example handles deadzone, sensitivity, stick response, inversion and vertical angle.

Normal third-person and camera-volume views use the same original geometry checks. A corridor camera can enter its own camera region; other exclusion regions still apply. Entering or leaving a camera volume keeps the checked heading and analogue input. If no orbit fits, the corridor uses its original moving view until an orbit is possible again. Fixed placements and unsupported camera paths retain their original look controls. Test your mod in open areas, near the ground and in tight corridors, as each uses different camera restrictions.

When `recenter` is set, the orbit packet supplies a yaw behind Rocket and the original pitch for the selected zoom. Apply those angles without adding look input if you want a full recenter. The first-person packet supplies Rocket's facing direction and its original entry pitch instead.

First-person look has its own packet so existing orbit mods keep their original ABI. Call `rocket_enable_first_person_look` during game setup, then handle `rocket_on_first_person_update`. Its yaw increases in the same rotation direction as the orbit packet, with positive pitch looking up. Start from the supplied angles, write the output angles and set `apply`. The host limits pitch to the game's original range of -1 to +1 radians. It changes the player's look angles before the original input routine; the game still applies left-stick input and chooses the first-person position and transitions. Camera mode buttons feed the original C-down and C-up actions instead of replacing the game's mode logic.

## Texture packs

The manager accepts RT64 `.rtz` archives containing `rt64.json` and replacement textures. It mounts enabled packs at renderer startup. A pack can include `mod.json` to provide a name, author, game compatibility and version; without one, it receives a hash-based ID and a generic name.

The replacement cache is capped at 128 MiB on Android and 512 MiB on desktop. This is the replacement pool budget, not a cap on every allocation made while loading a pack. Keep mobile packs small and test on the intended device.

Models, collision, level data, music and arbitrary files do not become replacement assets just by placing them in a package. Those need game-specific importers and runtime adapters. Rocket-R does not currently include those tools.

## Compatibility limits

Rocket-R generates a protected-function inventory from its checked recomp policy. Mods cannot hook or replace those functions through the runtime's raw-ROM regeneration path. This preserves the port's interpolation, rendering and other built-in fixes. Use the public events where available.

Native DLL, SO and executable payloads are rejected. Packages are checked for unsafe paths, duplicate names, symlinks, excessive sizes and malformed metadata. SHA-256 identifies installed content; it is not an author's signature. Code mods execute inside the game process and are not a security sandbox.

Keep mod source outside `extern`, `RecompiledFuncs` and `RecompiledPatches`. Changes to Rocket-R itself still go through the checked patch pipeline. Do not distribute the original ROM or extracted game assets in a mod or SDK.
