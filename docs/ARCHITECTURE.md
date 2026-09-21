# Rocket-R architecture

## Runtime path

```text
User-owned Rocket US NSUE ROM
        +
Pinned RocketRet byte-matching NSUE.elf
        |
        v
N64Recomp-generated native CPU functions
        |
        +-----------------------------+
        |                             |
        v                             v
N64ModernRuntime                RSPRecomp n_aspMain
threads / VI / PI / SI /        Nintendo n_audio microcode
EEPROM / timing / saves                |
        |                              v
        |                         host audio queue
        v
standard F3DEX2 OSTasks
        |
        v
RT64 -> D3D12/Vulkan (Metal when a macOS build is added)
```

Rocket-R is a static recompilation, not a source port. The readable RocketRet decompilation is used as a matching ELF/symbol source and as a reference for understanding game behaviour. The executable game path is the translated original MIPS program.

## Why Rocket is a good fit

The matching US decomp build produces `build/us/NSUE.elf` and verifies the rebuilt 12 MiB ROM against the user's original. Its ROM map identifies:

- N64ModernRuntime retains the cartridge ROM load/DMA address at `0x80000400`, but FIXED22 deliberately compiles/calls `game_init` at `0x80000E64` through a host wrapper. The wrapper's init callback recreates the exact retail bootstrap caller stack (`0x803FFFF0`); `gIdleThreadStack` is reserved for thread 1 created later inside `game_init`; the raw retail stub is not executed because it intentionally reaches a `break` after `game_init` returns under the static runtime;
- `rspboot` at ROM `0x2090`;
- Nintendo `n_aspMain` at ROM `0x2160`, size `0xC60`, loaded at IMEM `0x04001080`;
- standard F3DEX2 graphics microcode beginning at ROM `0x2DC0`;
- graphics tasks submitted as `M_GFXTASK` with `gspF3DEX2_fifoTextStart` / `gspF3DEX2_fifoDataStart`.

That means Rocket does not require DKR-R's custom F3DDKR renderer bridge. RT64 can use its normal GBI detection/interpreter path.

## Save/input boundary

The supported cartridge uses 512-byte / 4 Kbit EEPROM and Rumble Pak support. Rocket-R registers `SaveType::Eep4k` and exposes one N64 controller port. The first host implementation accepts SDL gamepads plus a keyboard fallback.

## ROM identity

The one-click builder first validates the canonical SHA-1, then compiles a tiny helper against N64ModernRuntime's own bundled xxHash and writes the exact `XXH3_64` value into an ignored generated header. This avoids hard-coding an XXH3 value that has not been independently verified.

## Generated-code rule

Never patch `runtime-recomp/RecompiledFuncs` or `runtime-recomp/RecompiledRSP` directly. CPU changes belong in `runtime-recomp/rocket.us.recomp-policy.json`; RSP boundaries belong in `runtime-recomp/rsp/n_aspMain.us.toml`; dependency patches belong in `patches/manifest.json`.

## Low-level hardware audit

Rocket's ELF contains low-level libultra symbols and N64 MMIO constants. Most are expected inside libultra functions that N64ModernRuntime replaces. The startup C code also contains a guarded direct `osPiRawStartDma` path; for the linked US memory layout `_codesegs0_1SegmentBssStart` (`0x800AF860`) is below `0x80100000`, so its signed `numBytes` guard prevents that raw-DMA branch from executing. Rocket-R does not patch it speculatively. If a ROM-backed run reaches N64ModernRuntime's raw-PI stub, the failure is explicit and the next patch can be made at an evidenced callsite.

## Accurate-before-modern policy

The initial graphics configuration remains original aspect ratio and original 30 Hz presentation. Rocket's physics are part of gameplay, so high-refresh output must never accelerate the original simulation. FIXED26 added opt-in RT64 interpolation only: Rocket continues to author at 30 Hz, RT64 receives that source cadence explicitly, and extra frames exist solely in the presentation queue. FIXED27 additionally aligns Rocket's CPU horizontal object frustum with the active Expand-to-window aspect, leaving Original 4:3, vertical frustum planes, distance culling and simulation untouched. Original 30 FPS / 4:3 remains the regression baseline while widescreen and 60/120/144 Hz presentation are qualified independently.


## FIXED22 runtime/frontend architecture
- SDL video, controller sampling and launcher event processing are owned by the main thread for the process lifetime.
- The launcher creates one persistent SDL window and passes its native handle to N64ModernRuntime/RT64; runtime `gfx_callbacks` no longer create or pump a second window.
- `recomp::start()` runs on a worker while the main thread services SDL events, input snapshots, rumble and launcher/overlay shortcuts.
- Rocket starts only after RT64 receives its first safe dummy-VI presentation.
- Queued graphics tasks carry an immutable 8 MiB RDRAM snapshot; RT64 temporarily decodes against that snapshot and restores live RDRAM afterwards.
- The SDL/ImGui launcher and RT64/ImGui overlay share PLAY, GRAPHICS, SOUND, CONTROLS and ABOUT pages.
- Windows installs an unhandled-exception minidump writer under `%APPDATA%\Rocket-R\logs`.


## FIXED34 platform fan-out
The ROM-derived pipeline ends before native platform compilation. `NSUE.elf`, N64Recomp output, RSPRecomp output, the ROM identity header and bootstrap header are generated once on the Windows/WSL host. Windows, Linux x86-64, Linux ARM64 and Android ARM64 then compile that same generated source. This keeps gameplay translation identical across packages.

FIXED34 intentionally uses the FIXED27 interpolation architecture. No stable-transform identity sidecar or broad dynamic-vertex interpolation from FIXED28-31 is present. Linux adds no gameplay hooks. Android adds only SDL/`ANativeWindow`, native refresh-rate/fullscreen handling, Android thread naming and build-host-tool portability needed to compile RT64/N64ModernRuntime with the NDK.
