# Troubleshooting

## Finding the log

Open **Graphics > Diagnostics** in the launcher or overlay. You can pause the
view, copy it, clear the visible history or let it follow new output.

Desktop session logs are in the configuration folder's `logs` directory:

- Windows: `%APPDATA%/Rocket-R/logs`
- Linux: `${XDG_CONFIG_HOME:-$HOME/.config}/rocket-r/logs`
- A custom `--config` folder: its `logs` directory

Windows crash reports and minidumps also go under the log directory. Include
the Rocket-R version, your OS/GPU and what you were doing when reporting a fault.

## ROM rejected

Use an unmodified US copy of Rocket: Robot on Wheels, game code `NSUE`.
The expected size is 12 MiB and the canonical SHA-1 is
`622D71A44DA0B81EA68092CAC9198C66154A4F4A`.
The launcher accepts `.z64`, `.n64` and `.v64` byte orders. Other regions,
modified ROMs and incomplete dumps are not supported.

## Steam Deck or Linux will not start

Steam Deck needs the x86_64 package. Extract the Steam Deck archive and run
`START-ROCKET-R.sh`. It does not need FUSE. For the other Linux packages, try the
portable archive's `Launch-Rocket-R.sh` if the AppImage cannot mount.

Launch-helper logs are in `${XDG_STATE_HOME:-$HOME/.local/state}/rocket-r/`.
Look for `steamdeck-launch.log`, `portable-launch.log` or `appimage-launch.log`.
Keep the whole extracted package together. Its libraries and assets are required.

Linux tests under WSL may use llvmpipe software rendering. Their performance does
not represent a Steam Deck or a native Linux GPU. Check the renderer named in
the log before treating low frame rates there as a game regression.

## Android launch or sound problems

The APK needs an ARM64 device with the Vulkan features used by the renderer.
Vulkan 1.0 support alone is not enough. Check media volume and the active audio
output if the game is silent. Android Back and the Settings touch button open
the overlay.

For low frame rates, choose **Low power** in Graphics and restart the game. It
uses native N64 resolution and 30 FPS. You can then raise resolution and frame
rate one at a time. Installing an update keeps your previous settings.

For graphical faults, enable **Graphics > Diagnostics > Record performance log**,
reproduce the problem for around 30 seconds, then return to the Android launcher
and choose **Save diagnostics**. Attach the ZIP and a screenshot or clip to your
report, with the level and steps to reproduce it. This export includes device
details, graphics settings and recent logs. It does not include your ROM or saves.

For a crash log, connect the device with USB debugging enabled and run
`CAPTURE-ANDROID-CRASH.cmd` from the source folder. It writes the captured logs
under `build/logs/`. Review them before sharing, as Android logs can contain
information from other apps.

## Controls seem wrong

Select the intended device in **Controls > Controller**, then use **Test Inputs**
in N64 Controls. Release held buttons and centre the stick before assigning
a new input. See the [controls guide](CONTROLS.md).

## Builder cannot continue

If WSL needs first-run setup, open Ubuntu once and create its account, then rerun
the builder. If newly installed Visual Studio tools are still unavailable,
restart Windows and try again.

Read the failed stage's log in `build/logs/`. The matching decompilation must
rebuild the ROM byte for byte. A mismatch stops the build. Do not bypass it.
For a missing translated function or indirect target, fix the recompilation
policy using the matching ELF; do not edit RecompiledFuncs.

## A font or shader looks different

Comic Sans is loaded when it is installed. Otherwise Rocket-R uses its shared
fallback font. Custom shaders need a binary for the active graphics backend and
a game restart. See [custom shaders](CUSTOM_SHADERS.md).
