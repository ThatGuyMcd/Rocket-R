# Third-party components

Rocket-R is built with the projects below. The builder fetches the revisions in
`dependencies.lock.json` into `extern/`; dependency changes are applied through
the checked patch manifest.

| Project | Used for | Licence at the pinned revision |
| --- | --- | --- |
| RocketRet / Rocket-Robot-On-Wheels | Matching US ELF, symbols and game-code reference | No root licence file is present in the pinned checkout. Original game rights are not granted by Rocket-R. |
| N64Recomp / N64ModernRuntime | N64 runtime, saves, timing and host services | GNU GPL v3 text in `COPYING`. |
| N64Recomp / N64Recomp | Static MIPS translation and RSPRecomp build tooling | MIT. |
| RT64 | Graphics rendering and interpolation | MIT. |
| SDL2 | Windows/input/audio support, including Android integration | zlib licence. |

Windows uses the SDL2 package supplied with the pinned RT64 dependencies.
Linux uses its build container's SDL2 package. Android builds the pinned SDL2
2.26.3 source. Other bundled libraries and tools retain their own licences.

Release packages include the project's GPL text and a `licenses` directory
with notices copied from the checked-out dependencies. Android includes those
files under `assets/rocket-r-docs/`. The source revisions and local patch files
remain part of the source project.

No game ROM or extracted game data is included in the source project or release
packages. Microsoft Comic Sans font files are not redistributed.
