# Troubleshooting

## The builder says WSL needs first-run setup

Open **Ubuntu** from the Start menu once, choose its Linux username/password, close it, then rerun `ONE-CLICK-BUILD.cmd`.

## The builder stops after installing Visual Studio Build Tools

A newly installed C++/LLVM workload is occasionally not visible to the current shell until Windows restarts. Restart and rerun the builder; it is safe to repeat.

## ROM rejected

Only the unmodified US `NSUE` release is supported initially. The script accepts all three common N64 byte orders but validates the canonical SHA-1 `622D71A44DA0B81EA68092CAC9198C66154A4F4A`. Patched, overdumped, truncated, PAL or prototype ROMs are intentionally rejected.

## Rocket decomp `make` fails

Read the current `build/logs/one-click-*.log`. The WSL stage must have `gcc-mips-linux-gnu`, `binutils-mips-linux-gnu`, Python/venv, wget and build-essential. The decomp's own `make` ends in a byte-for-byte `diff`; a mismatch is treated as unsafe and stops the recomp pipeline.

## N64Recomp reports a missing function/indirect target

Do not edit `RecompiledFuncs`. Add evidenced metadata to `runtime-recomp/rocket.us.recomp-policy.json`, then run `Diagnose-Rocket-Recompile.cmd`. The useful evidence is the function name, VRAM and size/caller from the matching ELF/decomp.

## `osPiRawStartDma_recomp` aborts

Capture the runtime log/stack and identify the translated caller. Rocket contains a guarded startup raw-DMA branch that should not execute for the normal US layout. If a real path reaches it, implement the smallest game/runtime patch supported by the callsite rather than globally turning arbitrary MMIO into host pointers.

## RT64 opens a window but the frame is black

Confirm the log reaches an `M_GFXTASK` and that the task's ucode equals Rocket's `gspF3DEX2_fifoTextStart` (`0x800021C0`). A black window before the first graphics task is a CPU/scheduler bring-up issue; a black window after accepted display lists is a renderer/VI issue.

## Audio task rejected

The initial dispatcher expects `M_AUDTASK` with ucode `0x80001560`. If the log shows another address, do not broaden the dispatcher blindly; inspect the task and Rocket's matching ROM map first.
