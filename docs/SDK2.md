# Rocket SDK 2

SDK 2 is available in Rocket-R 1.0.2. It adds gameplay callbacks, packaged resources, custom models and scenes, collision queries, music, HUDs and mod settings without replacing SDK 1. The same MIPS `.nrm` package runs on Windows, Linux x64, Linux ARM64 and Android ARM64. Test on the devices you intend to support; a portable package does not guarantee equal performance.

SDK 1's `rocket/mod.h`, camera packets and four original events are unchanged. Existing packages do not need rebuilding or an `api: 2` declaration. The published Modern Analogue Camera 1.0.0, Rocket Cheat Menu 3.0.1 and Rocket Colour Studio 1.0.0 packages are compatibility fixtures. The new managed services are optional.

SDK 2 packages made for the earlier 1.1.0-dev previews also work on 1.0.2.
Their installed files stay unchanged; Rocket-R adjusts the host-version field in
a private launch copy. Unsupported module versions still fail validation.

## Start a mod

Extract the SDK archive. Install Python 3.11+, Clang with the MIPS backend and LLD. On Ubuntu 24.04, Clang 18 and LLD 18 work; select them as `clang` and `ld.lld` on PATH. Python 3.10 also works with `python -m pip install tomli`. On Windows use Ubuntu-24.04 in WSL for guest compilation. Visual Studio's Clang cannot compile the guest MIPS code.

```text
python scripts/rocket_sdk.py init my_mod --id my_mod
python scripts/rocket_sdk.py build my_mod --output output
python scripts/rocket_sdk.py validate output/my_mod.nrm
```

Add `--wsl` to the build command on Windows. `--tool`, `--symbols` and `--data-symbols` select explicit tools or matching symbol dumps. The standalone SDK supplies RecompModTool and both symbol dumps; no game ROM or game build is needed to compile a managed mod. A player supplies their own ROM to run Rocket-R.

Install the resulting package through **Mods → Installed → Add Mods**. Use a separate profile while developing. Source files belong in your mod project, never in Rocket-R's generated folders or dependencies.

## Choose how the mod runs

Set these fields in `rocket.json`:

```json
{
  "schema": 1,
  "api": 2,
  "activation": "managed",
  "live_settings": true,
  "requires": {"lifecycle": 1, "input": 1},
  "input_actions": {
    "jump": {"name": "Jump", "keyboard": 44, "controller": 0, "n64": 32768}
  }
}
```

**Managed** mods use registered callbacks and host services. Their code can remain loaded in standby and switch on or off at a game update boundary. They must not contain raw hooks, function replacements, a ROM patch or `exclusive_resources`. Installed disabled managed packages may be staged in standby when they add no dependencies or conflicts. A package required by another running mod cannot be switched off live. Installing a different package or version still requires a restart.

**Restart** mods can also register SDK callbacks, but their raw hooks, replacements or asset patch remain installed for that game session. The switch selects the next launch. Use restart activation for fundamental native code changes. Native code is not unloaded during gameplay.

SDK 1 keeps its original behavior. The exact included camera package retains its existing host-approved live switch. Other SDK 1 packages are not silently treated as managed mods.

## Callbacks and lifetime

Include `rocket/sdk.h`. Register once from `rocket_on_game_ready`:

```c
#include "rocket/sdk.h"

static void update(const RocketTick *tick) {
    if (tick->state != ROCKET_PLAYING || tick->delta_time <= 0) return;
    RocketInputState input = {2, sizeof(input), 0, 0, 0};
    if (rocket_input_action("jump", &input) != ROCKET_OK || !input.pressed) return;
    RocketObjectState player = {0};
    player.api = 2; player.size = sizeof(player);
    if (rocket_object_read(tick->player, &player) != ROCKET_OK) return;
    player.velocity.z = 180.0f;
    rocket_object_velocity(tick->player, &player.velocity);
}

static const RocketCallbacks callbacks = {
    2, sizeof(RocketCallbacks), update, 0, 0, 0, 0, 0
};
ROCKET_CALLBACK(rocket_on_game_ready)
void ready(void) {
    rocket_sdk_register(&callbacks);
}
```

The callback slots are `tick`, `scene_enter`, `scene_leave`, `enabled`, `disabled` and `settings_changed`. They run on the game thread with a temporary `RocketTick`. Keep the descriptor in static storage. Never retain a callback packet pointer. Guest pointers use N64 O32, even when the host is 64-bit.

Ticks run once per authored game update, rather than once per interpolated display frame. `delta_time` uses the game's clock, is capped at 0.1 seconds and is zero when the clock is stopped. Skip movement when it is zero. Registration and activation notifications can occur outside a level. Gameplay ticks only run when a valid player and level are available; this SDK does not supply a launcher or title-screen update loop.

On scene exit, the host invalidates native observation handles before `scene_leave`. That callback is for clearing cached IDs, not modifying the old player. On disable within a level, `disabled` can restore the player's position before cleanup. The host then releases that mod's resources, actors, collision, sounds, HUD and system claims. On re-enable within a level, `scene_enter` runs again to rebuild them. Settings notifications can arrive while disabled: check `rocket_sdk_enabled()` before changing the game.

Every packet you pass to a typed import must have `api = 2` and `size = sizeof(packet)`. Handle-returning functions return zero on failure. Integer result functions return zero or a positive byte count on success, and negative values on failure. Check the result rather than assuming a missing resource succeeded. The current bridge maps some validation and I/O exceptions to `ROCKET_INVALID`; do not rely on a separate error code for every possible failure.

## Modules

Declare the modules your mod needs in `requires`. Import rejects a requirement newer than the host provides. `rocket_sdk_module(name)` returns the provided module version, or zero for an unavailable module. All modules below currently have version 1.

| Module | What it provides |
| --- | --- |
| `core`, `diagnostics` | SDK version, enabled state and diagnostic logging |
| `lifecycle` | Registration and gameplay/scene/activation/settings callbacks |
| `input` | Named actions, remapping and press/release counts |
| `commands` | Buttons in mod settings, consumed on the game thread |
| `resources`, `saves` | Scoped package files and schema-tagged profile saves |
| `native_objects` | Player/native object observations and native transform setters |
| `events`, `systems` | Queued mod events and cooperative ownership |
| `hud` | Text and rectangles in the shared UI layer |
| `native_audio` | Original sound effects and native music attenuation |
| `custom_audio` | PCM WAV clips, loops, gain and pan |
| `meshes`, `actors` | Authored static geometry and persistent actor transforms |
| `custom_scenes`, `custom_collision` | Scene descriptions, ray queries and box sweeps |
| `native_render` | Temporarily hide the original 3D render queue |
| `asset_layers` | Compose separate SDK 2 cartridge asset edits |

The declarations describe requirements. They are not a security sandbox for raw guest code.

## Input, settings and buttons

Actions appear in the mod's **Details and settings**, rather than cluttering the N64 controls page. Desktop users can bind keyboard keys, mouse buttons, wheel directions, mouse movement, controller buttons, triggers or stick directions. Android can use the action's N64 touch fallback. One action has one keyboard/mouse source, one controller source and an optional N64 mask. These defaults and remaps are saved separately from the mod's config options.

`RocketInputState.value` reports the action value. `pressed` and `released` are counts since that mod last read the action, so a short press between game updates is retained. Read each action at the point where you consume it. Reserved settings shortcuts stay available. Generic actions can coexist with native bindings; they do not automatically suppress a matching N64 input. The camera's dedicated input arbitration remains separate.

Metadata uses SDL scancodes 0–511, or -1 for unbound. Mouse buttons use 2001–2005; wheel up/down/left/right use 2100–2103; movement left/right/up/down uses 2200–2203. Controller buttons use 0–20 and positive/negative axis directions use 1000–1011. Prefer the input capture prompt to entering numeric bindings by hand.

Keep Number, Enum and String options in `mod.toml`, as in SDK 1. Read them through `recomp_get_config_double`, `recomp_get_config_u32` and the existing configuration imports. To display a two-choice Enum as a checkbox, add `"settings_ui": {"arena": "toggle"}` in `rocket.json`. The checkbox selects option index 1 when checked. Both the desktop and Android settings pages use the same declaration. The host sends live changes to the matching loaded package; your mod must reread them in `settings_changed` or its update.

Commands add explicit action buttons:

```json
"commands": [{"id": "reset", "name": "Start again"}]
```

Call `rocket_command_take("reset")` inside your update callback. It returns and clears pending presses. The UI never calls guest functions directly.

## Resources and saves

Declare package resources by logical name:

```json
"resources": {
  "room": {"file": "content/room.json", "type": "scene/json"},
  "music": {"file": "content/music.wav", "type": "audio/wav"}
}
```

Use `[files]` in `mod.toml` to add authored files while retaining their package paths. `[assets."content/model.rrm"]` can compile an OBJ during the build. Workshop shows both forms. Resources are scoped to the calling mod: another mod cannot read your files with your handles.

`rocket_resource_open`, `rocket_resource_size`, `rocket_resource_read` and `rocket_resource_close` copy raw bytes into caller-owned buffers. Reads can be chunked, but opening a resource currently decompresses the whole selected ZIP entry into memory. Close files when finished. Limits are 16 MiB per resource, 32 MiB of open resources per session and 256 open handles. A resource index can declare 4,096 entries. Reads through the guest bridge are limited to 256 KiB per call.

`rocket_save_write(name, schema, bytes, length)` writes into this mod's save folder inside the current profile. `rocket_save_read` returns the stored schema through `schema_out`. You decide whether to migrate or reject an older format. Writes use a temporary file and retain a `.previous` backup. Save names are confined to the mod's folder; each payload is limited to 256 KiB. Save I/O is synchronous, so save after an event or at a checkpoint, not every frame. Workshop migrates its first counter-only save to its newer progress format.

## Native gameplay and full code changes

`tick->player` and `rocket_player_handle()` provide a one-update observation handle. `rocket_object_observe(address)` can observe another valid native object. Read position, velocity and its verified rotation prefix with `rocket_object_read`. Native handles expire at the next update boundary; obtain them again each update.

`rocket_object_position` and `rocket_object_velocity` call the game's virtual setters so native spatial updates are retained. They are accepted during active gameplay callbacks, including a disable callback restoring the player. They are not a universal actor factory or a typed API for every enemy, puzzle or vehicle.

For larger gameplay rewrites, include `rocket/recomp_mod.h`, use **restart** activation, and compile C hooks/replacements against the supplied function and data symbols. `ROCKET_HOOK("function")` runs on entry, `ROCKET_HOOK_RETURN("function")` runs on return, and `ROCKET_PATCH` marks a replacement with the original function name. `ROCKET_EXPORT` exposes a function to other code mods. Use the actual function signature. The symbol dump supplies names and addresses, not trustworthy C declarations. `rocket/game.h` only contains a small verified native object prefix and setters; the Modding Bible is research material, not a guarantee that every proposed structure is correct.

`python scripts/rocket_sdk.py symbols camera --kind function` searches the supplied
symbols and marks protected functions. Use `--kind data` for globals, or an
exact function-name fragment to narrow the result. The tool does not infer a
function signature or generate safe native bindings from an address alone.

Rocket-R's checked policy protects the functions that implement its port fixes. Raw hooks or replacements targeting them are rejected. SDK 2 has not widened that protected set or blocked the existing camera/colour/cheat hook targets. Use SDK callbacks and services for those protected paths. Code changes to Rocket-R itself still belong in the maintained patch pipeline.

## Models and actors

Import an authored OBJ:

```text
python scripts/rocket_sdk.py mesh cube.obj cube.rrm --colour FFD866FF
python scripts/rocket_sdk.py mesh cube.obj tiles.rrm --texture tiles.png
```

Coordinates use Rocket's Z-up world. `--y-up` converts a Y-up OBJ. The importer triangulates polygon fans, supports negative OBJ indices and preserves UV seams for textured geometry. Triangulate complex concave faces in your modeling tool before export. It ignores materials, normals, bones and animations. The current material is vertex colour, optionally multiplied by one opaque texture. There is no skeletal animation importer, multi-material mesh or native Rocket model converter in this release.

Author meshes, scene positions and collision boxes in gameplay units. Workshop's cube runs from -1 to +1 on each axis. The renderer handles the conversion to the game's graphics units; multiplying the authored cube by sixteen would put the native camera inside its player model.

Textures must be 8, 16 or 32 pixels in each dimension. They become RGBA16 and fit native TMEM; PNG input needs Pillow, while text P3 PPM needs no extra dependency. Use `.rtz` packs for high-resolution replacements of original textures. These small custom mesh textures serve a different purpose.

Load a mesh with `rocket_mesh_load`, then create/update/remove actors with `RocketActorState`. Position, velocity, scale and Z-up yaw are supported. `rocket_actor_pose` adds a normalized x/y/z/w quaternion for full rigid orientation. Actor handles persist until removal, scene exit or disable. Their presentation identity follows the actor's lifetime, so matrix arena reuse does not swap interpolation histories. Static mesh geometry itself is not interpolated.

Global limits are 128 meshes, 256 actors and 16 MiB of decoded mesh storage. Individual meshes have at most 4,096 vertices and 4,096 triangles. The renderer appends native F3DEX2 draws within the original 8 MiB graphics snapshot. Custom draw data uses two 128 KiB banks between the expanded entry queue and sort scratch, leaving the game's 32 KiB frame arena for its draw commands and HUD. It stops adding draws when either budget is full. The actor limit is not a promise that 256 maximum-size meshes fit in one frame. Keep geometry small, reuse meshes and test on lower-powered hardware. Rendering converts public coordinates to Rocket's sixteen-times projection units; transforms outside the native signed 16.16 matrix range are skipped.

## Scenes and collision

A scene is JSON containing a resource-to-mesh map, actors and axis-aligned collision boxes:

```json
{
  "schema": 1,
  "meshes": {"cube": "cube"},
  "actors": [{"id": "platform", "mesh": "cube",
    "position": [0, 0, -1.5], "scale": [8, 8, 1]}],
  "collision": [{"min": [-8, -8, -2.5], "max": [8, 8, -0.5]}]
}
```

`rocket_scene_load` replaces only this mod's managed scene. `rocket_scene_load_at` translates its actors and boxes by an origin. Loading is transactional: an invalid candidate leaves the old managed world intact. Scene actors can also specify velocity, yaw and an `orientation` quaternion. `rocket_actor_find(id)` returns this mod's named actor. A scene can contain 32 meshes, 64 actors and 256 boxes, within the session limits.

`rocket_collision_ray` finds the nearest hit on this mod's boxes. `rocket_collision_move` sweeps an axis-aligned character volume, slides along surfaces and reports grounding. Use the corrected position and velocity in your own controller or the native setters. It includes bounded overlap recovery for teleports. It is not a physics engine: no triangle mesh collision, rigid bodies, slopes or automatic registration into the original camera/player collision tree. Moving actors do not move collision boxes automatically.

To draw a custom adventure instead of the retail scenery, declare `world.render` in `systems`, claim it and call `rocket_native_render(0)`. This hides the original 3D queue while managed actors continue drawing. If the custom renderer submits no actors, it keeps the native scenery visible and records a diagnostic instead of replacing it with an empty scene. Native physics, camera, background and HUD continue running. On scene exit or disable, native rendering returns automatically. A full conversion must supply its own gameplay and account for those remaining systems; hiding the scene is only one part of the job.

## Music, sounds, HUD and cooperation

Custom clips use RIFF/WAV PCM16, mono or stereo, 8–96 kHz. Declare a resource, call `rocket_audio_load`, then pass a `RocketAudioPlay` with gain, pan and loop to `rocket_audio_play`. Check `rocket_audio_playing` and stop voices with `rocket_audio_stop`. Loading decodes a whole clip; this is not streamed music. Limits are 128 clips, 32 voices and 32 MiB of decoded stereo audio. Handles are scoped to the mod and cleaned up on scene exit/disable. Reuse a clip while a scene is active rather than loading it repeatedly.

The host mixes clips into the existing output block with continuous resampling. They share the approved master gain and pause outside gameplay. Native audio timing, queue sizes and frame pacing are unchanged. Mixed output saturates to PCM16; keep your own source and mix gains sensible.

`rocket_sound_play` plays original effects 0–161 through the native mixer, with volume and pan 0–128. `rocket_sound_stop` accepts only your owned handles. For custom music, declare and claim `audio.music`, then use `rocket_native_music_gain(0..100)` to attenuate original music. Effects retain their volume. Cleanup restores the captured gain if no other code has changed it since the SDK's last write.

Submit `RocketHudItem` text or rectangles using a 320×240 viewport canvas and RRGGBBAA colours. Text uses the shared UI font. Up to 64 items per mod and 256 per session are retained until replaced or cleaned up. Submit zero items to clear your HUD. This is a simple overlay drawing API, not a widget framework or original HUD replacement.

Events use `mod_id:event` names. Only the owning mod emits that namespace; other mods can subscribe. Up to 256 payload bytes are copied, and events are queued for the next gameplay update. Treat payloads as an agreed byte protocol, not host pointers. There are 128 queued events per session and 128 subscriptions per mod.

Declare cooperative systems in `systems` and use `rocket_system_claim/release`. A conflicting claim returns `ROCKET_CONFLICT`. Claims are released during cleanup. They arbitrate mods that use the SDK; arbitrary raw hooks can still conflict. Profile dependencies, versions and declared conflicts remain useful for those combinations.

## Cartridge asset edits

The SDK can create a bounded BPS patch from two private, canonical 12 MiB US `.z64` images:

```text
python scripts/rocket_sdk.py asset-patch original.z64 edited.z64 patch.bps
```

The header and game code before ROM offset `0xB0460` must be unchanged. Add `patch.bps` to `[files]`, use `api: 2`, restart activation and `requires: {"asset_layers": 1}`. Only authored changed bytes go into the patch; unchanged ROM ranges are references. Do not distribute extracted game assets or a patched ROM.

Multiple SDK 2 patches made by this tool can share a profile when they use the same original image and edit separate ranges. Overlaps are rejected with the mod names. Rocket-R writes one composite into private launch staging and leaves the installed packages unchanged. Complex BPS patches containing copy commands can run alone, but cannot be composed by this service; regenerate them with the SDK tool. SDK 1's single-patch path stays unchanged and is not silently merged with SDK 2 layers.

This tool does not edit native level/model/music formats for you. Their compression, tables, relocations and size limits still need a format-specific authoring tool. Custom SDK scenes and WAV files do not replace a retail asset just because they are in a package.

## Rocket Workshop

`modding/examples/workshop` is the reference managed mod. Build it with `rocket_sdk.py build`. Its normal mode adds an interpolated gold marker, a boost action, a HUD counter and a separate saved counter. **Workshop arena** moves into two authored rooms above the current player position, hides native 3D draws, plays the example's own authored WAV score, collects six gold blocks and adds a patrol hazard. The cyan cube is a stand-in player model. The arena retains the game's player movement and camera and corrects player motion against its own boxes.

Try Boost using V, right shoulder or Android touch R. Change Boost strength live. Enable arena, collect gears, test the reset button, disable to return, and re-enable to reload the room. Check that progress survives a clean restart and that Original Game is unaffected. The example is deliberately small enough to read and adapt; it is not a replacement engine or an editor.

Workshop 0.2.2 uses gameplay units for its room and player scale, and places the room above the retail scenery to keep native camera collision out of the example. It does not replace the game's camera collision system. For rendering diagnostics, launch Rocket-R with `ROCKET_SDK_RENDER_TRACE=1`. This logs the camera matrices and actor positions at four simulation samples, then stops; it does not log continuously.

## Rocket Randomiser

`modding/examples/randomizer` is a restart mod that shuffles Whoopie World's
Tinker Token rewards. Build and install it through the same SDK tools as
Workshop. Start with a fresh save slot in a separate mod profile. Its seed is
stored before pickups are changed and remains fixed for that slot. See the
[randomiser instructions](../modding/examples/randomizer/README.md) for setup,
save handling and the exact scope of version 0.1.0.

This example uses a verified native token initialiser because SDK 2 does not
have a general inventory or collectible-reward API. The mapping preserves
the native 1/5/10-token flags, counter reconstruction and 200-token ticket.
It demonstrates one world of seeded rewards; it does not shuffle entrances,
machine parts or progression requirements.

## Compatibility and validation

Run `rocket_sdk.py validate` before sharing a package. The launcher repeats package, metadata and resource checks. Managed packages cannot smuggle raw hooks into live activation. Protected port functions, package IDs, original event order and the SDK 1 header stay stable.

Host tests cover ownership, lifetime cleanup, MIPS packet layouts, callbacks, live activation, input edges, saves, scene rollback, geometry encoding, collision, audio continuity and separate asset layers. Published SDK 1 packages are imported and staged unchanged. The native Windows test session has loaded all three alongside Workshop; loading is not proof that every gameplay interaction works. New custom rendering, arena gameplay and mobile performance still require hands-on device testing before a stable release.

Code mods run in the game process. They can call native functions and access guest memory, so these ownership checks are correctness boundaries, not a sandbox for untrusted code. Keep a mod's source available, give it a clear version, list dependencies, and describe which native systems it replaces.
