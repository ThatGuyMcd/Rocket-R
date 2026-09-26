# ROCKET-R

Rocket: Robot on wheels Recompiled

Rocket-R brings Rocket: Robot on Wheels to Windows, Linux and Android. You can
play at higher resolutions, use smoother frame rates and set up the controls how
you like. You'll need your own unmodified US ROM to play. The game is not included.

<<<<<<< Updated upstream
Double-click:
=======
Recompilation project by **ThatGuyMcd**.

## Getting started
>>>>>>> Stashed changes

Download the 1.0.0 package for your device and extract it where needed.

<<<<<<< Updated upstream
The builder now starts with a **multi-select platform question**. Pick any combination of:
=======
| Device | Package | Start the game |
| --- | --- | --- |
| Windows x64 | `Rocket-R-1.0.0-Windows-x64.zip` | Extract the ZIP and open `Rocket-R.exe`. Keep its DLLs and assets alongside it. |
| Steam Deck | `Rocket-R-1.0.0-Linux-x86_64-SteamDeck.tar.gz` | Extract the archive and run `START-ROCKET-R.sh`. |
| Linux x64 | `Rocket-R-1.0.0-Linux-x86_64.AppImage` | Make the AppImage executable, then open it. |
| Linux ARM64 | `Rocket-R-1.0.0-Linux-aarch64.AppImage` | Make the AppImage executable, then open it. This build still needs gameplay testing on an ARM64 Linux device. |
| Android ARM64 | `Rocket-R-1.0.0-Android-arm64-v8a.apk` | Install the APK, open Rocket-R and choose your ROM. |
>>>>>>> Stashed changes

Linux portable archives are also available. Extract one and run
`Launch-Rocket-R.sh` if your system cannot launch the AppImage normally.
Steam Deck uses the **x86_64** package, not ARM64.

The desktop launcher accepts `.z64`, `.n64` and `.v64` files. Choose your ROM or
drag it onto the launcher, then select **Play Rocket-R**. Android keeps a private
copy of the ROM you select so you don't need to choose it again each time.

Only the US release is supported: game code `NSUE`, 12 MiB, canonical SHA-1
`622D71A44DA0B81EA68092CAC9198C66154A4F4A`.

## Settings and controls

Press **F1** or **Escape** to open settings during play. **F11** or **Alt+Enter**
switches fullscreen. F1 and F11 can be rebound in **Controls > Shortcuts**;
Escape and Alt+Enter remain available.

**Controls > N64 Controls** opens Controller Studio. Select an N64 button and
choose its keyboard or controller input. **Guided Setup** takes you through each
control, and **Test Inputs** lets you check your setup. See the
[controls guide](docs/CONTROLS.md) for the default bindings and touch controls.

On Android, tap **Settings** or press Android Back to open the overlay.
**Hide Controls** hides the gameplay buttons when you're using a controller.
The Settings and Show Controls buttons remain available.

Graphics settings include widescreen, resolution, frame rate and filtering.
**Distant texture detail** lets you keep the original texture changes at 0%, use
the highest available detail at 100%, or choose something in between.

Comic Sans is used where it is installed. A shared fallback font is used where
it isn't available; Microsoft font files are not included in the packages.

## Platform notes

Windows and Steam Deck have been tested in gameplay. Android gameplay, speakers
and touch controls have been tested on the Honor Magic V5. The Android target
is lower-cost phones with 3 GB RAM, but that hardware still needs testing.
The current renderer needs an ARM64 device with the required Vulkan 1.2 features.

Linux ARM64 builds and automated tests pass, but gameplay on native ARM64 Linux
hardware has not been confirmed. See [testing](docs/TESTING.md) for the details.

If something goes wrong, **Graphics > Diagnostics** shows the live log.
The [troubleshooting guide](docs/TROUBLESHOOTING.md) covers log locations and
common build and launch problems.

## Building and development

Run `ONE-CLICK-BUILD.cmd` to build from source on Windows. It can produce Windows,
Linux x64, Linux ARM64 and Android ARM64 packages. Windows builds also produce
a matching Linux x64 AppImage.

- [Build instructions](docs/BUILDING.md)
- [Development rules](docs/DEVELOPMENT.md)
- [Runtime architecture](docs/ARCHITECTURE.md)
- [Custom shaders](docs/CUSTOM_SHADERS.md)

## Credits and licences

Rocket-R uses RocketRet's matching decompilation, N64Recomp, RSPRecomp,
N64ModernRuntime, RT64 and SDL2. Thanks to their authors and contributors.

See [the project licence notice](LICENSE.md), [GPL licence text](LICENSE) and
[third-party components](THIRD_PARTY.md). Rocket: Robot on Wheels and its original
game data remain the property of their respective owners.
