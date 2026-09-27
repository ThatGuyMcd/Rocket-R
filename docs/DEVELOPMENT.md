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

All 40 listed dependency patches are required in their current order. An older
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

Presentation identities belong to individual draws and owners. Shared model
assets and draw order are not sufficient to identify an object between frames.
Rigid rotations use quaternion interpolation; projected shadows need component
interpolation. Keep the corresponding regression cases when changing either path.

## UI

Launcher and overlay pages share their UI code. `DrawPageHeader` keeps title,
caption and divider spacing consistent, including when captions wrap.
`SectionTabs` owns the common responsive grid and the space below it.
Keep page settings below the tab row.

All native pages use the shared 21px Comic Sans font when available. Use the
active font's measurements for layout and keep the fallback consistent.
Controller Studio must keep all 18 N64 inputs and the four existing binding slots.
Do not change saved identifiers just to rename a label.

## Release checks

Use the maintained build and packaging scripts. `stage_release_docs.py` supplies
the same current guides and licence notices to each platform package.
`verify_release.py` checks the final distribution and writes its checksums.
Run the checks in [TESTING.md](TESTING.md) before sharing a build.
