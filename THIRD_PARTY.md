# Third-party components

Rocket-R does not vendor the external projects below in its source ZIP. The builder fetches exact commits into the ignored `extern/` work area.

- **RocketRet/Rocket-Robot-On-Wheels** — matching US decompilation/ELF reference. Pinned at `cd1fc6d3f575841a240373d7b8a2baf532da2f9f`. Treat its upstream repository and files as the authority for its licensing/status; Rocket-R does not redistribute its generated ROM, extracted assets or source tree.
- **N64Recomp/N64ModernRuntime** — pinned at `ae1ffbb909d9f93c88c41830deb539f7feef5ed2`. Upstream licence applies.
- **N64Recomp/N64Recomp** — pinned at `81213c1831fab2521a6a5459c67b63437d67e253`. Upstream licence applies. Rocket-R carries one small source patch already used by DKR-R to fix KSEG0 entrypoint signedness.
- **rt64/rt64** — pinned at `6f1c2d99a4ea571c139f449c326fd176ba8f3496`. Upstream licence applies.
- **SDL2** — the Windows runtime uses the SDL2 package bundled by the pinned RT64 tree; Linux uses the system SDL2 development/runtime package. SDL's upstream licence applies.

Before any public release, perform a fresh licence audit at the exact pinned commits and include all notices/source-offer obligations required by the linked runtime stack.
