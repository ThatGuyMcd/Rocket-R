# Release notes

## Unreleased — Android optimisation candidate

- Fixed missing Vulkan upload flushes and readback invalidation for non-coherent
  memory, and resource access across distinct graphics/compute/transfer families.
- Reused immutable graphics-task snapshots and removed their redundant clearing.
  Task completion and audio timing are unchanged.
- Reduced Android input-loop wakeups and gave background shader compiler threads
  a lower CPU priority. Added an advisory display frame-rate hint on Android 11+.
- Added Low power, with native N64 resolution, 30 FPS and minimal enhancements.
  New Android profiles use it; existing saved graphics choices are kept.
- Added live performance logging and Android diagnostics export for user reports.
  These changes still need checks on affected Android GPUs before release.

## 1.0.2

- Updated the launcher and overlay with blue panels, solid raised buttons and
  rounded popups. Settings use grouped cards and sliders with round thumbs and
  separate value readouts. Smaller windows use a compact navigation row. Mod settings
  remain with each mod, and profile management opens in its own popup.
- Kept Comic Sans, shared page-header spacing and the existing controls. The
  spinning logo uses one cached image; the UI still waits for events between frames.
  Overlay backgrounds show the game through panels at 75% opacity.
- Kept SDK 2 preview packages compatible with the release version. Android
  updates also install over the SDK 2 preview APK without needing an uninstall.

- The desktop launcher now uses accelerated drawing where available, caps active
  updates at 60 Hz and slows down while idle or in the background. Linux releases
  its launcher graphics context before creating the game's Vulkan window.
- Cached the mod library, dependency results and file lists used by the UI.
  Installing mods, changing profiles and live settings still update immediately.
- Reduced CPU work in interpolation matching by searching only matching owners
  from the previous frame. The existing ambiguity and collision rules are kept.
- New profiles start at Original 2X with standard framebuffer precision. Saved
  graphics choices are kept. Performance now selects Original 2X, 30 FPS, no MSAA
  and low-cost texture sampling, without forcing a driver workaround off.
- Graphics diagnostics now identify the rendering device and report CPU software
  rendering. Optional performance traces include frame rate and display-list
  decode timings for investigating slow devices.

- Added optional, versioned SDK modules while keeping SDK 1 imports, events,
  packets and existing packages unchanged. The included camera remains 1.0.0.
- Added managed gameplay, scene, activation and settings callbacks, with live
  on/off and cleanup. Raw hooks and replacements still require a restart.
- Added scoped package resources, schema-tagged saves, remappable actions,
  Android touch fallbacks, checkbox settings and mod action buttons.
- Added queued mod events, cooperative system claims, HUD text and rectangles,
  native effects and PCM WAV playback through the existing audio output.
- Added an OBJ importer, textured rigid actors, lifetime interpolation IDs,
  custom scenes, box sweeps, ray queries and temporary native-world hiding.
- Added SDK 2 asset-patch composition for separate cartridge edits. Overlaps
  are rejected and installed packages stay unchanged.
- Added the standalone SDK builder, validator and matching symbol dumps, plus
  Rocket Workshop as a small custom-scene example. See `docs/SDK2.md` for the
  supported formats, limits and remaining authoring tools.
- Corrected custom draw counts, camera transforms, vertex colours and graphics
  buffer allocation. Workshop 0.2.2 uses gameplay units for its rooms and player
  model, with the arena placed clear of the original level's camera collision.

This is a development build. Published SDK 1 packages have passed import/staging
checks and loaded alongside Workshop on Windows. Custom arena gameplay and
SDK 2 performance still need hands-on testing across the target devices.

- Reworked Sky dithering reduction to filter and cache the small sky texture.
  This removes the extra sampling work that made the setting so heavy on the GPU.
- Kept smooth sky movement and the full 0–100% adjustment, including fast
  up/down camera movement. The corrected filter has been checked on Windows;
  Steam Deck testing is still pending.

Modern Analogue Camera remains at 1.0.0. Its controls and behaviour are unchanged.

## 1.0.1

- Added a Mods tab for installing code mods and texture packs, choosing profiles
  and changing mod settings. Modded profiles use their own saves.
- Included Modern Analogue Camera 1.0.0 by ThatGuyMcd. Use a right stick, mouse
  or keyboard, adjust momentum, invert either axis and remap the camera inputs.
  The three original zoom levels and first-person view are supported. Settings
  and the camera's on/off switch work during gameplay.
- Kept the game's camera obstacle checks, with corrections for tight corridors
  and ground contact.
- Added mouse buttons, wheel directions and optional mouse movement bindings
  to Controller Studio and camera mod settings.
- Fixed close-wall clipping when using a wider field of view.
- Smoothed the sky's movement and corrected its alignment at wider FOV settings.
  Sky dithering reduction is under Graphics → Image.
- Fixed interpolation of Rocket's rolling wheel, including its squash movement.

The camera code is unchanged from the approved 0.1.13 development package.
Its 1.0.0 package metadata targets Rocket-R 1.0.1. Profiles using that exact
development package move to the release version automatically, keeping their
settings and saves. Other mod versions keep their current selection.

Windows x64, Linux x64, Linux ARM64, Steam Deck and Android ARM64 packages are
available. Both Linux architectures include AppImages. See
[testing](docs/TESTING.md) for the checks run and the remaining device coverage.

## 1.0.0

First release for Windows, Linux and Android, with the launcher and in-game
settings, Controller Studio, Android touch controls, graphics options and
interpolated rendering.
