# Building Rocket-R

## Windows build

Run `ONE-CLICK-BUILD.cmd` from the repository folder. Use a normal Windows session;
the builder will tell you if a prerequisite needs installation or a restart.

Choose Windows x64, Linux x64, Linux ARM64, Android ARM64, or all four. Selecting
Windows also selects Linux x64 so a matching AppImage is always produced.
You'll be asked for your own unmodified US ROM.

The builder prepares the pinned dependencies, validates the ROM, builds the
matching ELF, generates the CPU and audio code, then compiles each platform.
The generated game code is shared between platforms. Desktop builds compile
and run all seven CTest suites before packaging.

For a specific set of platforms, use PowerShell:

```powershell
& .\scripts\OneClickBuild.ps1 -Platforms Windows-x64,Linux-x86_64,Linux-aarch64,Android-arm64 -NoLaunch
```

The Windows build uses Visual Studio C++ tools, the Windows SDK, clang-cl,
CMake, Ninja and Python. The matching game build and Linux packages use WSL
Ubuntu. If WSL is new, open Ubuntu once and finish its account setup before
running the builder again.

Build logs are saved under `build/logs/`. Keep the log from a failed stage;
it contains the command and error needed to investigate it.

## Linux packages

Once the shared game code has been generated, the Linux helpers can be run
from WSL or an appropriate Linux build environment:

```bash
./Setup-Linux.sh
./Build-Linux.sh --arch x86_64
./Build-Linux.sh --arch aarch64
```

The helpers need Docker, rsync and the generated CPU/RSP sources and headers.
They build in Ubuntu 22.04 containers. ARM64 uses QEMU/binfmt on an x64 host.
`Setup-Linux.sh` installs the helper prerequisites on apt-based systems.

Both architectures produce an AppImage and a portable archive. The x64 build
also produces a Steam Deck archive that starts directly from its extracted files
and does not need FUSE. An AppImage's filename identifies its CPU architecture.

## Android package

The Android builder uses Java 17, SDK/build-tools 34, NDK 26.1.10909125,
CMake 3.22.1 and Gradle 8.7. It packages `arm64-v8a` only, using the pinned
SDL2 2.26.3 source. The manifest targets SDK 34 with a minimum SDK of 24;
the device must also meet the renderer's Vulkan requirements. The APK version
name follows `VERSION`; its numeric update code is major × 10000 + minor × 100
+ patch. Release 1.0.1 uses 10001, above the earlier development APKs' 10000.

The signing key is stored in `build/private/android-signing/`. Keep this key
backed up and private. Updates need the same signing identity to install over
an existing build. The local builder does not publish to an app store.

## Files produced

Release packages go in `dist/`, using the version from `VERSION`:

```text
Rocket-R-1.0.1-Windows-x64.zip
Rocket-R-1.0.1-Linux-x86_64.AppImage
Rocket-R-1.0.1-Linux-x86_64-Portable.tar.gz
Rocket-R-1.0.1-Linux-x86_64-SteamDeck.tar.gz
Rocket-R-1.0.1-Linux-aarch64.AppImage
Rocket-R-1.0.1-Linux-aarch64-Portable.tar.gz
Rocket-R-1.0.1-Android-arm64-v8a.apk
rocket_modern_camera.nrm
```

The Windows executable is in `build/windows/bin/Release/`. The private working
ROM and its identity record are in `build/private/`; they are never packaged.
Release scans reject ROM files and N64 ROM headers. The final verification step
also writes `Rocket-R-1.0.1-build-info.json` and `Rocket-R-1.0.1-SHA256SUMS.txt`
with the source and package hashes.

`-NoLaunch` skips the final launch prompt. `-NoPackage` skips the Windows ZIP;
Linux and Android selections still produce their native packages.

## After changing the recompilation policy

Run the builder again to regenerate from
`runtime-recomp/rocket.us.recomp-policy.json`. `Diagnose-Rocket-Recompile.cmd`
runs the builder with `-NoPackage -NoLaunch`.
Do not edit the generated CPU/RSP files or dependency checkouts.
See [development](DEVELOPMENT.md) and [testing](TESTING.md).
