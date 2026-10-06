# ROCKET-R

<img width="1254" height="1254" alt="Rocket-R-green-full-resolution" src="https://github.com/user-attachments/assets/6f59e8e5-6d00-424c-81e1-7b93c6fee169" />

Rocket: Robot on wheels Recompiled

Rocket-R brings Rocket: Robot on Wheels to Windows, Linux and Android. You can
play at higher resolutions, use smoother frame rates and set up the controls how
you like. You'll need your own unmodified US ROM to play. The game is not included.

Recompilation project by **ThatGuyMcd**.

To keep up to date with news, Join the Discord!
<p align="center">
  <a href="https://discord.gg/39aGEGqsX3">
    <img src="https://img.shields.io/badge/JOIN%20OUR%20DISCORD-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Join our Discord">
  </a>
</p>

## Getting started

The current release is **1.0.2**, with SDK 2 and support for existing SDK 1 mods.
See the [SDK 2 guide](docs/SDK2.md) for making mods and the Workshop example.
Use the package for your device below. See [testing notes](docs/TESTING.md) for
which builds have been checked on hardware.

Download the package for your device, then follow the steps below:

| Device | Package | Start the game |
| --- | --- | --- |
| Windows x64 | `Rocket-R-1.0.2-Windows-x64.zip` | Extract the ZIP and open `Rocket-R.exe`. Keep its DLLs and assets alongside it. |
| Steam Deck | `Rocket-R-1.0.2-Linux-x86_64-SteamDeck.tar.gz` | Extract the archive and run `START-ROCKET-R.sh`. |
| Linux x64 | `Rocket-R-1.0.2-Linux-x86_64.AppImage` | Make the AppImage executable, then open it. |
| Linux ARM64 | `Rocket-R-1.0.2-Linux-aarch64.AppImage` | Make the AppImage executable, then open it. This build still needs gameplay testing on an ARM64 Linux device. |
| Android ARM64 | `Rocket-R-1.0.2-Android-arm64-v8a.apk` | Install the APK, open Rocket-R and choose your ROM. |


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
choose its keyboard, mouse or controller input. **Guided Setup** takes you through each
control, and **Test Inputs** lets you check your setup. See the
[controls guide](docs/CONTROLS.md) for the default bindings and touch controls.

On Android, tap **Settings** or press Android Back to open the overlay.
**Hide Controls** hides the gameplay buttons when you're using a controller.
The Settings and Show Controls buttons remain available.

Graphics settings include widescreen, resolution, frame rate and filtering.
**Distant texture detail** lets you keep the original texture changes at 0%, use
the highest available detail at 100%, or choose something in between.
**Sky dithering reduction** softens the sky pattern without blurring the level or
HUD. Leave it at 0% for the original texture, or raise it for a smoother sky.

Comic Sans is used where it is installed. A shared fallback font is used where
it isn't available; Microsoft font files are not included in the packages.

## Mods

The **Mods** tab lets you add code mods and texture packs, choose a profile and
adjust each mod's settings. **Browse** includes Modern Analogue Camera by
ThatGuyMcd. It adds right-stick and mouse look, adjustable momentum, both axis
inversions, the three original zoom levels and first-person controls. Its
settings and on/off switch work during gameplay.

Modded profiles have separate saves. **Original Game** uses your existing save
and starts without mods. The catalogue is currently offline; add downloaded
packages with **Add Mods**. See the [modding guide](docs/modding.md) for installing
mods and making your own, and the [release notes](CHANGELOG.md) for v1.0.2.

## Platform notes

Windows and Steam Deck have been tested in gameplay. Android gameplay, speakers
and touch controls have previously been tested on the Honor Magic V5. Users have
reported graphical faults and poor performance on other Android devices. The
current optimisation candidate still needs checks on affected GPUs. The Android
target is lower-cost phones with 3 GB RAM, but that hardware still needs testing.
The current renderer needs an ARM64 device with the required Vulkan 1.2 features.

Linux ARM64 builds and automated tests pass, but gameplay on native ARM64 Linux
hardware has not been confirmed. See [testing](docs/TESTING.md) for the details.

If something goes wrong, **Graphics > Diagnostics** shows the live log.
On Android, select **Low power** in Graphics and restart for the lightest settings.
Existing installs keep their saved choices. Enable **Record performance log**
before reproducing a fault, then use **Save diagnostics** in the Android launcher
to export a ZIP for your report. ROMs and saves are excluded.
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
