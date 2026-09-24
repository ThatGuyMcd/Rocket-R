Rocket-R Graphics v9.8 COMPILE FIX

This fixes the actual Windows C++ compile errors from the 20260923-173959 build.

1) recomp_context
N64Recomp defines recomp_context as an anonymous typedef:
    typedef struct { ... } recomp_context;
Rocket-R Graphics v9 incorrectly added:
    struct recomp_context;
Those are different C++ types. v9.8 removes the invalid forward declaration and
includes the authoritative N64Recomp recomp.h header.

2) LauncherHeading
The Graphics/Controls UI calls LauncherHeading(...) but the helper is not visible
on this FIXED34/v8.3 launcher baseline. v9.8 either exposes an existing helper
or supplies a minimal compatibility helper using stable ImGui calls.

The installer:
- backs up both source files;
- runs Rocket-R self-check;
- runs Graphics v9 verification;
- then resumes the EXISTING Windows Ninja build as an incremental compiler test.
If another compiler error appears, the two known fixes are left in place and a
dedicated compile-smoke log is printed so progress is not rolled back.


v9.8 wrapper fix
----------------
The BAT no longer passes -Root. The PowerShell installer derives the repository root from its own script path, avoiding quoted trailing-backslash corruption on Windows.
