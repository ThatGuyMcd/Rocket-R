# Runtime architecture

Rocket-R translates the original US game's MIPS code with N64Recomp. RocketRet's
matching decompilation supplies the ELF and symbols used for that translation.
RSPRecomp handles the game's Nintendo n_audio microcode. N64ModernRuntime provides
the N64 runtime, SDL2 handles host input/audio/windows, and RT64 renders F3DEX2
graphics tasks.

## Startup and game data

The builder checks the 12 MiB US NSUE ROM, builds a matching `NSUE.elf`, and
generates CPU/RSP sources plus ROM identity and bootstrap headers. All platform
builds use the same generated game code. Players supply their own ROM at runtime.

The retail load address is `0x80000400`. The callable game entry is `game_init`
at `0x80000E64`, with the retail caller stack restored to `0x803FFFF0`. Keeping
the load address separate avoids executing the original bootstrap's final break
instruction under the static runtime.

The audio microcode begins at ROM `0x2160`, has length `0xC60`, and loads at
IMEM `0x04001080`. The game uses standard F3DEX2 graphics tasks; it does not need
DKR's custom graphics microcode interpreter.

## Threads and rendering

The main thread owns SDL window events and input sampling. The runtime runs on
a worker thread, and game startup waits for the renderer's first safe VI
presentation. Graphics tasks carry an immutable 8 MiB RDRAM snapshot so later
game writes cannot change data that the renderer is still reading.

The original game authors frames at 30 Hz. Higher frame-rate settings ask RT64
to interpolate presentation frames while leaving the game clock unchanged.
Original 30 FPS remains available. Widescreen adjusts the game's horizontal
visibility frustum to match the expanded view.

## Presentation identities

The host presentation bridge identifies individual model draws using verified
callsite information, persistent owners and submodel roles. A model pointer alone
cannot distinguish two objects that share an asset. Scratch stack positions and
draw order are not treated as persistent identities.

Rigid transforms use rotation decomposition and shortest-arc quaternion
interpolation. Projected shadow matrices can be singular or sheared, so they
retain component interpolation. Secondary matrices in vehicle attachment draws
have their own role within the owning model invocation. Ambiguous identities,
conflicting matrix uses, cull gaps and teleports are handled conservatively.

Rocket's rolling wheel is identified by its checked draw call and the player's
wheel-model pointer. Its squash and rotation matrices can combine into a sheared
basis, so the final rolling transform also interpolates scale and shear. Other
sheared models and projected shadows keep their existing handling.

The CPU regression suite checks these cases against the host bridge and patched
RT64 transform maths. Visual checks in the game remain necessary for new paths.

The sky uses a separate textured rectangle. Its vertical movement comes from the
texture rows selected by `func_8008AEA0`, rather than a rotating sky model. Checked
guest hooks capture the source rows and keep enough neighbouring rows loaded for
RT64 to sample between frames. That information travels with the graphics task.
Camera cuts and changes of sky texture start fresh history.
The row displacement uses the rendered camera FOV and perspective projection,
so widening the view does not make the horizon scroll faster than the scenery.

Graphics → Image has a Sky dithering reduction slider. It filters the sky texture
before compositing, so it does not blur the level, Rocket or the HUD. The filter
averages all eight columns of Rocket's narrow sky gradient at full strength.
Wider textures use a 4×4 filter that limits blending across strong colour edges.
Both preserve alpha. The default is 0%; changing it applies immediately and
saves with the other graphics settings. Replacement textures bypass the filter.

## Audio, saves and input

The guest audio manager keeps its original AI timing and FIFO model. Desktop
output applies the master-volume setting and a 0.70 maximum gain. Android converts
the continuous output stream to the device's preferred sample rate and requests
media audio focus.

The game uses 4 Kbit EEPROM and one N64 controller port with Rumble Pak support.
Keyboard, gamepad and Android touch inputs feed the same N64 input state.
Bindings and shortcuts use stable saved identifiers. Controller Studio input
testing does not send its preview inputs to the game.

## Platform UI

Desktop builds use an SDL/ImGui launcher. The in-game overlay is drawn through
RT64 and shares the same pages. Android uses a Java ROM picker, then starts the
native runtime directly. Its touch layer provides gameplay buttons and access
to the shared overlay.

Windows captures its console output for Graphics > Diagnostics and session logs.
Linux keeps terminal output, and Android forwards diagnostics to logcat.
See [development](DEVELOPMENT.md) for the editing rules and
[testing](TESTING.md) for current platform coverage.
