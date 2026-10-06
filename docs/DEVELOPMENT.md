# Development

## Where changes belong

| Change | Location |
| --- | --- |
| Host runtime, UI, input and presentation code | `src/` |
| Guest hooks and instruction changes | `runtime-recomp/rocket.us.recomp-policy.json` |
| Audio microcode boundaries | `runtime-recomp/rsp/n_aspMain.us.toml` |
| Dependency changes | Patch files listed in `patches/manifest.json` |
| Build and package tools | `scripts/`, `packaging/` and the root build helpers |
| Regression tests | `tests/` |

Do not hand-edit dependency checkouts, RecompiledFuncs, RecompiledPatches,
RecompiledRSP or generated headers. Apply dependency patches through
`scripts/bootstrap_dependencies.py`. Regenerate game code through the builder.
Keep dependency revisions in `dependencies.lock.json` and patch hashes in
`patches/manifest.json` in sync.

All listed dependency patches are required in their current order. An older
patch can still be needed by a later one. Some active verification scripts also
retain version numbers in their names; the builder still calls them.

Local build tools and caches belong in `build/`, and current release packages
belong in `dist/`. Neither folder is source. Keep one-off experiments, recordings,
old packages and planning notes out of the source tree.

## Working on a change

Start with the [source and regression checks](TESTING.md). Keep changes focused
and use the existing tests when they cover the affected behaviour. Build all
affected platforms. New Windows builds must include a matching Linux AppImage;
both Linux architectures need their own AppImages.

Keep ROMs, signing keys, build output and local diagnostics out of source control.
`build/`, `dist/`, `extern/` and generated game code are ignored. The Android
signing key must survive cleanups so future APKs can update existing installs.

## Audio and interpolation

Desktop audio uses the original guest timing with a maximum PCM gain of 0.70.
That gives the volume slider some headroom. Previous timing and buffering changes
caused regular hitching; check an audible problem before changing those paths.
Android has its own continuous output conversion to the device's sample rate.

Vulkan upload mappings must flush non-coherent memory before GPU use. Readback
mappings must invalidate it before CPU reads. Buffers and textures shared by
distinct queue families need concurrent sharing because Plume's current barriers
do not transfer exclusive ownership. Keep these fixes in the dependency patches.
Graphics-task snapshots use a pool of immutable leases; do not reuse a buffer
until its task has finished parsing or move the SP/DP completion signals.

New Android profiles use Low power: native N64 resolution, 30 FPS, standard
framebuffer precision, double buffering and no extra anisotropy. Saved settings
take precedence. The display frame-rate hint is advisory and does not change
guest timing. Android's input loop waits for SDL events with an 8 ms timeout so
touch state published by Java is still sampled promptly.

Presentation identities belong to individual draws and owners. Shared model
assets and draw order are not sufficient to identify an object between frames.
Rigid rotations use quaternion interpolation; projected shadows need component
interpolation. Keep the corresponding regression cases when changing either path.

## UI

Launcher and overlay pages share their UI code. `DrawPageHeader` keeps title,
caption spacing consistent, including when captions wrap.
`SectionTabs` owns the common responsive grid and the space below it.
Controls keeps its section tabs. Graphics groups display, image, camera and
diagnostic options into cards, with less common options collapsed initially.

`src/ui_theme.hpp` supplies the shared blue palette, cards, buttons and setting
grids. Buttons use solid colours. Keep `WindowRounding` as well as
`PopupRounding`: ImGui modal windows use the former. Only the fullscreen root
temporarily sets its own rounding to zero. The logo is loaded once into the font
atlas, with transparent padding trimmed in memory; the source image is kept intact.
Its original turning animation uses a single textured quad. Slider drawing keeps
ImGui's dragging, keyboard/gamepad navigation and direct numeric entry.

All native pages use the shared 19px Comic Sans base font when available. Larger
headings use separately rasterized sizes of the same face. The blue background
gradient is a static quad. Overlay panels composite to 75% opacity; controls,
text and modal windows keep their normal opacity. Use the
active font's measurements for layout and keep the fallback consistent.
Controller Studio must keep all 18 N64 inputs and the four existing binding slots.
Do not change saved identifiers just to rename a label.

## Mod SDK

SDK 1's `modding/include/rocket/mod.h` is frozen. Add imports and packets in the
SDK 2 headers instead. Module versions describe individual capabilities; reject
unsupported requirements at import rather than accepting a mod that cannot run.
Keep raw hooks on restart activation and managed callbacks on the game thread.
Scene exit and disable must release owned resources and return native controls.

The host services are owned code under `src/mods`. Guest dispatch hooks belong
in `runtime-recomp/rocket.us.recomp-policy.json` and are regenerated by the
normal pipeline. Never patch generated functions or dependencies by hand.
The [SDK 2 guide](SDK2.md) documents wire layouts, resource budgets and tools.

## Release checks

Use the maintained build and packaging scripts. `stage_release_docs.py` supplies
the same current guides and licence notices to each platform package.
`verify_release.py` checks the final distribution and writes its checksums.
Run the checks in [TESTING.md](TESTING.md) before sharing a build.
