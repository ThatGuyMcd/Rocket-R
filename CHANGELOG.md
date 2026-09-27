# Release notes

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
