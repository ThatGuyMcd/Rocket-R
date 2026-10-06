# Runtime architecture

## Mod SDK

SDK 1 retains its imports, packet layouts and original event order. SDK 2 adds
versioned modules in owned `src/mods` code. The lifecycle bridge dispatches MIPS
callbacks on authored game updates and restores the caller's CPU context.
Native observations expire each update; managed actor handles persist until
removal, scene exit or disable. Host UI changes queue activation, settings and
commands for the game thread.

`sdk_services` owns resources and schema-tagged profile saves. `sdk_world` owns
static meshes, actor transforms and custom box collision. `sdk_render` appends
bounded F3DEX2 draws before the game's render sort and registers actor lifetime
identities with the existing interpolation path. `sdk_audio` mixes owned WAV
voices into the existing output blocks without changing native audio timing.
`asset_layers` combines disjoint SDK-generated BPS edits in private launch
staging; installed packages remain immutable.

Managed live packages cannot contain raw hooks, replacements or ROM patches.
Restart mods retain the native code path and protected port-function checks.
These are ownership and compatibility rules, not a sandbox for guest code.
See [SDK 2](SDK2.md) for the public contract and current resource budgets.

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

The desktop launcher uses SDL's accelerated renderer with a software fallback.
On Linux it starts in a window suitable for OpenGL. After the launcher releases
its renderer, the main thread recreates that window for Vulkan, preserving its
size and window mode. Direct game launches create the Vulkan window immediately.
The launcher waits for events between frames, updates at most 60 times a second
while active, and reduces its refresh rate when idle, unfocused or minimised.
Immutable mod-library views are rebuilt after changes, rather than copied and
resolved on every UI frame. File lists are cached and only visible rows are drawn.

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

Matching indexes the eligible previous-frame tracks by semantic key. Candidates
within a key retain their original order and use the same distance and ambiguity
checks. This removes scans through unrelated objects and older history without
changing which owners qualify for interpolation.

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

Graphics → Image has a Sky dithering reduction slider. The host filters the
verified eight-column CI8 sky at its native size, then caches an RGBA texture.
The game declares 1,024 wrapping rows but loads a 256-row slab into TMEM.
The filter decodes that physical slab and keeps its 256-row wrapping period;
other raw-TMEM layouts retain the original sampler.
Full strength averages every column and smooths the neighbouring rows. The
cache key includes the source pixels, palette and slider strength; scrolling
can reuse a cached result. Rendering takes the normal texture-sampling path,
without extra per-pixel filter taps or a full-screen pass. Unsupported formats,
transparent artwork and replacement textures keep their normal handling.
The default is 0%; changing it applies to the next game frame and saves with
the other graphics settings. Rocket, the level and the HUD are not filtered.

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
