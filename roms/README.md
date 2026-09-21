# ROM input

Do not commit or distribute a Rocket: Robot on Wheels ROM.

`ONE-CLICK-BUILD.cmd` asks you to select your own unmodified US cartridge dump
and writes a canonical private working copy under `build/private/`.

Supported target for the initial recompilation:

- Rocket: Robot on Wheels (USA), game code `NSUE`
- Canonical big-endian SHA-1: `622D71A44DA0B81EA68092CAC9198C66154A4F4A`
- Size: 12 MiB (`0xC00000` bytes)

`.z64`, `.v64` and `.n64` byte orders are accepted and canonicalised locally.
