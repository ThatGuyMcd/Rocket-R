# Building Rocket-R

## Windows host — recommended multi-platform builder

Run `ONE-CLICK-BUILD.cmd` from Explorer or a normal Command Prompt. Do **not** start it from an elevated shell unless your normal Windows setup specifically requires that.

The builder handles the complete sequence and writes a transcript to `build/logs/`. Stage 5 also writes a dedicated `rocket-decomp-YYYYMMDD-HHMMSS.log` that records every Splat, legacy-tool and matching-ROM build command in full.

At startup, choose one or more targets: Windows x64, Linux x86-64 AppImage, Linux ARM64 AppImage, Android ARM64 APK, or all four. Static recompilation is shared across the selections; it is **not** rerun separately for each platform.

### Actions the builder may legitimately ask you to perform

- Approve installation of a missing prerequisite through `winget`.
- Restart Windows after first-time WSL/Build Tools installation if Windows requires it.
- Launch Ubuntu once and create its Linux username/password after a brand-new WSL install.
- Enter that Linux password when `sudo` installs MIPS/decomp build packages.
- Select your own unmodified Rocket USA ROM.
- Optionally launch the freshly built executable at the end.

Those are the only expected manual actions. The builder does not require you to copy ELF files, edit TOML, run N64Recomp manually, calculate hashes or hunt for DLLs.

## Regeneration after a policy change

`ONE-CLICK-BUILD.cmd -NoPackage -NoLaunch` is intentionally idempotent. It regenerates dependencies and translated output from clean pinned states. `Diagnose-Rocket-Recompile.cmd` is a convenience alias for that diagnostic path.

## Android ARM64 APK

When Android is selected, `scripts/Build-Android.ps1` provisions Java 17, Android API 34/build-tools, NDK 26.1, CMake 3.22.1 and Gradle 8.7 when missing. The APK contains only `arm64-v8a` native libraries. SDL2 is pinned to 2.26.3 and RT64 uses Vulkan/`ANativeWindow`.

The Java launcher uses Android's Storage Access Framework to let the user select their own ROM and copies it to the application's private storage. The release scan opens the final APK and rejects ROM-like content before the build is accepted. A local release signing key is generated under `build/private/android-signing`; it is a local/development signing identity, not a store publishing key.

## Build outputs

Windows native executable and required DLLs:

```text
build/windows/bin/Release/
```

ROM-free distributables (only the selected targets are emitted):

```text
dist/Rocket-R-0.1.0-dev-Windows-x64.zip
dist/Rocket-R-0.1.0-dev-Linux-x86_64.AppImage
dist/Rocket-R-0.1.0-dev-Linux-aarch64.AppImage
dist/Rocket-R-0.1.0-dev-Android-arm64-v8a.apk
```

Private canonical ROM and identity metadata:

```text
build/private/rocket.us.z64
build/private/rocket.us.json
```

The private files are ignored and are not copied into `dist`.

## Linux AppImage helpers

`Build-Linux.sh` is now the platform stage used by the Windows One Click Builder after the shared CPU/RSP generation has completed. It accepts:

```text
./Build-Linux.sh --arch x86_64
./Build-Linux.sh --arch aarch64
```

The helper builds in an Ubuntu 22.04 Docker userspace and creates a ROM-free type-2 AppImage. On x86-64 hosts, the aarch64 selection uses QEMU/binfmt to execute the ARM64 container, then verifies that the produced ELF is genuinely ARM64 before packaging. `Setup-Linux.sh` installs the Docker/rsync/QEMU helper prerequisites on apt-based systems.

The helper expects `generated/*.generated.hpp`, `RecompiledFuncs` and `RecompiledRSP` to already exist. That is deliberate: all selected platforms consume exactly the same translation generated once by the main builder.

The Rocket-R root CMake minimum is intentionally 3.22.1 so it matches the pinned Android SDK CMake used by Gradle. Do not raise the root minimum without raising the Android package/version in the same revision.
