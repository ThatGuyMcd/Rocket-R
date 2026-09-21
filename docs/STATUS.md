
## FIXED34 platform build repair + launcher readability
FIXED34 does not alter the FIXED27 renderer/interpolation baseline. Android now aligns the project CMake minimum with the pinned Android SDK CMake 3.22.1 (the previous 3.24/3.22.1 mismatch guaranteed Gradle native configuration failure). Linux AppImage containers keep Ubuntu 22.04 for wider glibc compatibility but install `tomli`, allowing `self_check.py` to run on Python 3.10. The launcher font is enlarged and requests Comic Sans MS from the host OS without bundling a font file.

# Status and qualification gates

## What is complete in this source package

- local-only repository layout with no project GitHub remote;
- exact pinned Rocket decomp / N64ModernRuntime / N64Recomp / RT64 revisions;
- N64Recomp KSEG0 entrypoint fix already proven in DKR-R;
- US NSUE ROM canonicalisation and SHA-1 gate;
- byte-matching Rocket decomp/ELF build stage;
- automatic XXH3 identity generation using N64ModernRuntime's own xxHash;
- N64Recomp CPU generation stage;
- RSPRecomp configuration for Rocket's `0x2160..0x2DBF` n_audio microcode;
- FIXED26 retains the FIXED22 boot split between Rocket's ROM load address and its callable payload: N64ModernRuntime keeps `0x80000400` for the initial 1 MiB ROM placement, while N64Recomp generates `game_init` at `0x80000E64`; an init callback restores the exact retail bootstrap caller stack (`0x803FFFF0`) so the raw retail stub and its post-return `break` are not executed; `gIdleThreadStack` belongs to thread 1 created later by `game_init`;
- Eep4k save routing and host Rumble Pak exposure;
- SDL2 Windows/Linux window, audio and input integration;
- RT64 standard-F3DEX2 renderer path;
- ROM-free release safety scan/package stage;
- guided Windows one-click prerequisite/build workflow;
- Linux setup/build workflow.

## What cannot be truthfully certified without the user's ROM/hardware run

This package was assembled without possessing or distributing the retail game ROM, and the artifact environment cannot run the final Windows/WSL/GPU pipeline. Therefore these remain **qualification gates**, not claims:

1. N64Recomp accepts every Rocket ELF function/indirect-call boundary without additional manual metadata.
2. Rocket's exact n_audio blob uses the common 0xC60 branch table selected in the RSP config.
3. No game-specific direct MMIO/PI/RSP register path is reached during normal play.
4. The scheduler reaches first VI, title screen and world rendering correctly through the unmodified runtime.
5. Audio timing/queue depth remains correct through music/SFX transitions.
6. EEPROM save/load and rumble are correct in actual gameplay.
7. Every level, vehicle, puzzle, cutscene and ending can be completed.

The builder is designed to turn a failure at any of these gates into a reproducible log instead of a silent crash.

## Definition of first playable milestone

- executable enters the original entrypoint;
- first F3DEX2 task is accepted by RT64;
- title/menu visible;
- controller input accepted;
- n_audio tasks produce audible output;
- new game reaches Whoopie World;
- EEPROM persists across restart.

## Definition of compatibility-complete milestone

A full retail playthrough, including all worlds/challenges/cutscenes and final completion, with no unsupported runtime/RSP path and with save/rumble/audio validated. Only after that should widescreen, interpolation, texture packs or other Modern-mode work become default-facing features.

## FIXED15 stage-7 boundary policy
Rocket's matching ELF exports several assembly-only libultra routines as zero-sized FUNC symbols. N64Recomp intentionally ignores zero-sized JAL targets, so Rocket-R supplies exact function sizes derived from the pinned NSUE segment layout. The first observed blocker was bzero at 0x800040B0 (ROM 0x004CB0-0x004D50, size 0xA0). FIXED15 carries 24 verified assembly-function boundaries, including the complete exceptasm partition, to avoid repeated zero-size JAL failures.

## FIXED16 stage-8 native runtime compile fixes
FIXED15 completed static CPU/RSP recompilation and advanced into the final native Rocket-R + RT64 build. The remaining Windows compile failures were build-system issues: clang-cl needed SSE4.1 enabled for librecomp's generated RSP vector intrinsics, and the repository root include path caused the project `VERSION` file to shadow the C++20 standard `<version>` header on case-insensitive Windows. FIXED16 enables `/clang:-msse4.1`, removes the repository root from RocketR's include search, adds the `generated` directory directly, and updates `game_registration.cpp` to include the ROM identity header from that isolated path.


## FIXED24 runtime qualification: boot/render proven, audio clock separated from SDL
The Windows native build is proven to complete. Runtime testing then showed why load address and callable entry must remain separate: `GameEntry.entrypoint_address` must stay at retail `0x80000400` for N64ModernRuntime's initial ROM DMA, but executing Rocket's raw startup stub reaches its deliberate `break` at `0x80000438` once `game_init` returns after starting the idle thread. FIXED22 keeps `0x80000400` for ROM placement, generates `game_init` (`0x80000E64`) as the callable payload, and restores the retail bootstrap caller stack (`0x803FFFF0`) in `on_init_callback`, matching the wrapper architecture used by DKR-R. `gIdleThreadStack` is not the bootstrap stack; it is passed to `osCreateThread` by `game_init`.

The host lifecycle now follows the proven DKR-R ownership model: one SDL window is created by the launcher on the main thread, its native handle is passed to RT64, `recomp::start()` runs on a worker, and SDL/video/controller event pumping remains on the owner thread. Game start is deferred until RT64's first safe VI present. Eight generic N64ModernRuntime stability patches from the same pinned runtime revision add null event-queue protection, clean quit wakeups, immutable RDRAM snapshots for graphics tasks, pending-VI coalescing and safer SP/DP completion ordering.

A first full Rocket-R frontend is included: PLAY, GRAPHICS, SOUND, CONTROLS and ABOUT launcher pages; ROM Browse/drag-and-drop; persistent graphics/audio settings; F1/Escape in-game overlay; F11/Alt+Enter fullscreen; live resolution/MSAA/aspect/fullscreen controls; and Windows crash text/minidump output under the Rocket-R config `logs` directory. User runtime testing has confirmed that Rocket boots, renders, and is substantially playable at its expected visual cadence.

FIXED23 proved that Rocket needs real two-slot AI FIFO status and the authored 22,500 Hz device rate, but it still derived guest AI progress from `SDL_GetQueuedAudioSize()`. Rocket's n_audio manager submits 552/368-frame stereo DMA blocks at a 60 Hz retrace cadence, while the SDL host callback was 512 frames. That made the emulated FIFO advance in host-sized jumps larger than Rocket's minimum DMA and produced the observed roughly half-speed, regularly stuttering audio.

FIXED24 makes the N64 AI clock independent from SDL. The guest-visible active/queued DMA slots now drain continuously from `std::chrono::steady_clock` at the exact game-requested sample rate; `osAiGetLength()` and FIFO busy/full status see only that virtual hardware state. SDL is output-only, starts with a protected host-side cushion that is invisible to the guest, and requests a 256-frame callback (below Rocket's 368-frame minimum DMA). Host underruns are diagnostic only and never slow, pause or re-prime the emulated AI clock. The existing normalized 65% default volume is retained.

## FIXED25 presentation qualification: remove Rocket's authored CRT safe-area frame
User runtime testing of FIXED24 confirmed that game speed, video cadence and audio are now smooth. The remaining visible defect was geometric: both Expand to Window and Original 4:3 showed a black frame around the game. The captured 1440x900 client area left approximately the same 18/320 horizontal and 14/240 vertical proportions unused as Rocket's retail renderer itself. The decomp confirms the active depth/render region is `x=18..301`, `y=14..225`, i.e. a deliberate 284x212 CRT overscan-safe area inside the 320x240 framebuffer.

FIXED25 removes that border at the **final RT64 VI presentation only**. A Rocket-only RT64 compile patch enlarges the presentation viewport by the inverse safe-area fractions and lets the existing full output scissor crop the excess. The guest viewport, display lists, depth buffer, framebuffer contents and timing remain untouched. Expand therefore maps the authored active area onto the whole window, while Original 4:3 maps it onto the full-height 4:3 presentation box.

The renderer also carries the same scoped 320x240 VI-height normalization strategy used by DKR-R: if RT64's generic progressive-height heuristic turns Rocket's canonical signal into 244/248 rows by adding guard rows, only `VI_V_START_REG` is temporarily narrowed while `updateScreen()` snapshots the VI, then immediately restored. This is presentation metadata only. No FIXED24 audio FIFO, SDL buffering, scheduler, graphics-task snapshot or main-thread window-ownership logic is changed.

## FIXED26 interpolation qualification: unlock presentation rate without unlocking the game clock
User testing confirmed FIXED25 removes the safe-area frame cleanly while retaining the smooth FIXED24 audio/video behaviour. FIXED26 therefore treats that stack as the baseline and adds only presentation interpolation. Rocket's scheduler renders every two NTSC retraces (`D_80017DE0 = 2`), giving RT64 an authored 30 Hz source. With interpolation enabled, the renderer calls `State::setRefreshRate(30)` for each submitted workload, while the guest scheduler, physics, controller polling and 22.5 kHz AI clock remain untouched.

The Graphics page persists three choices: **Original 30 FPS**, **Match display**, and **Custom 30-500 FPS**. Original uses RT64's original-rate path. Match Display resolves the active SDL display rate before configuring RT64. Custom requests a bounded manual target; the graphics backend may still limit actual presentation to what the active swap chain/display can deliver.

Five RT64 patches are carried directly from the same pinned DKR-R dependency revision to make early-present interpolation reliable with asynchronously rendered/alternating framebuffers. They bind each present to its producing workload, preapprove the exact early-present framebuffer before queue handoff, permit that workload to create the interpolation target, retain the DKR-R SkipBuffering compatibility fix, and expose a monotonic interpolated-present counter. Rocket's existing safe-area presentation patch remains separate and last in the manifest. No FIXED24 AI FIFO/audio code or FIXED25 viewport code is changed.

Qualification order is intentionally conservative: prove **60 FPS** first in title/menu, ordinary gameplay, camera cuts, particles and HUD; then test Match Display and 120/144 Hz. Watch specifically for transform-matching artefacts such as ghosting across hard camera cuts. Original 30 FPS remains the immediate fallback and regression baseline.

## FIXED34 current baseline: FIXED27 rendering restored, four platform packages
FIXED34 retains the FIXED33 RT64 portability EOF correction, aligns Android's project minimum with the pinned SDK CMake 3.22.1, and makes the Ubuntu 22.04 AppImage self-check work with Python 3.10 via `tomli`. These are build-only changes; the FIXED27 renderer/interpolation baseline remains unchanged.

The experimental FIXED28-31 transform-identity/dynamic-vertex interpolation layer is removed. User testing reported worse artefacting and a gameplay crash, while FIXED27 had the best interpolation behaviour. FIXED34 therefore restores FIXED27's renderer/interpolation/culling source and its six RT64 presentation patches as the gameplay baseline. Platform work is compile-time isolated around that baseline.

The One Click Builder now shares one validated ROM -> ELF -> CPU/RSP recompilation across any selected combination of Windows x64, Linux x86-64 AppImage, Linux ARM64 AppImage and Android ARM64 APK. Linux architecture is verified before packaging. Android is ARM64-only, imports the user's ROM through SAF/private app storage, and uses an RT64 Android bridge for `ANativeWindow` plus refresh-rate/window handling. No ROM is shipped in any package.

Runtime qualification remains required on real target hardware. In particular, the Android and ARM64 AppImage paths are new packaging/portability targets; source/preflight validation does not substitute for GPU/gameplay testing.

## FIXED27 widescreen qualification: match CPU object culling to the expanded view
User testing confirmed FIXED26 interpolation itself is clean. The next visible defect was independent of interpolation: in Expand to Window, RT64 showed world space beyond the retail 4:3 view, but Rocket's CPU `frustum_test` still discarded objects against its original camera side planes. Models therefore appeared or disappeared abruptly near the widened left/right edges.

FIXED27 adds one N64Recomp hook at the verified `frustum_test` entry (`0x8003C764`). A host bridge reads Rocket's camera forward vector, four side-plane normals, vertical FOV and authored aspect directly from the camera structure. Only in Expand mode, it identifies the two horizontal planes by their forward component and reconstructs them for the active SDL window aspect plus a 5% guard. Vertical culling, distance culling/fades and object radii remain retail. Original 4:3 returns before any widening.

The bridge publishes window aspect from the SDL owner thread and performs no SDL calls from the guest thread. Plane writes are idempotent: a prior widened set is restored only when the exact values are still owned by the bridge, so a camera update performed by the game is never overwritten with stale state. No FIXED24 audio, FIXED25 presentation crop, FIXED26 interpolation or RT64 patch code changes in this pass.
