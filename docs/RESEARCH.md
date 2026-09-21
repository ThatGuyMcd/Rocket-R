# Research baseline

The initial repository was based on direct inspection of these upstream projects and exact revisions:

- `ThatGuyMcd/DKR-R` — architecture/build/policy conventions used as the format baseline.
- `RocketRet/Rocket-Robot-On-Wheels` at `cd1fc6d3f575841a240373d7b8a2baf532da2f9f` — matching NSUE ELF/ROM map and readable game/libmus reference.
- `N64Recomp/N64ModernRuntime` at `ae1ffbb909d9f93c88c41830deb539f7feef5ed2` — same baseline pinned by DKR-R.
- `N64Recomp/N64Recomp` at `81213c1831fab2521a6a5459c67b63437d67e253` — same baseline pinned by DKR-R.
- `rt64/rt64` at `6f1c2d99a4ea571c139f449c326fd176ba8f3496` — same baseline pinned by DKR-R.

Important findings from the Rocket decomp:

- `NSUE.elf` is emitted by the matching build.
- Retail startup segment: ROM `0x001000-0x001100`, VRAM `0x80000400-0x80000500`, symbol `EntryPoint`. N64ModernRuntime must keep `GameEntry.entrypoint_address = 0x80000400` because it also uses that address for the initial 1 MiB ROM DMA. Runtime testing showed the raw stub reaches its real `break` at `0x80000438` after `game_init` returns. FIXED22 therefore separates load address from callable payload: `0x80000400` remains the load address, N64Recomp generates `game_init` at `0x80000E64`, and an init callback restores the actual bootstrap caller stack decoded from the retail stub (`0x803FFFF0`). `gIdleThreadStack` is instead used later by `game_init` when it creates thread 1.
- `n_aspMainTextStart`: `0x80001560`.
- `gspF3DEX2_fifoTextStart`: `0x800021C0`.
- `gspF3DEX2_fifoDataStart`: `0x80018370`.
- ROM RSP boundaries: rspboot `0x2090`, n_aspMain `0x2160`, F3DEX2 `0x2DC0`.
- The graphics scheduler submits standard `M_GFXTASK` jobs using the F3DEX2 FIFO ucode.
- The audio library is configured for Nintendo `n_audio` / `n_aspMain`.
- The cartridge uses 4 Kbit EEPROM and Rumble Pak support.

The 0xC60/0x1080 n_audio indirect target set in `runtime-recomp/rsp/n_aspMain.us.toml` is the common table independently used by existing N64Recomp projects for Harvest Moon 64, Mario Party 3 and Buck Bumble. Rocket's first ROM-backed RSP generation/runtime pass remains the final qualification of that binary variant.
