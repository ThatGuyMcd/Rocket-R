[CmdletBinding()]
param(
    [string]$ProjectRoot = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Resolve-RocketRoot {
    param([string]$RequestedRoot)

    $candidates = New-Object System.Collections.Generic.List[string]
    if ($RequestedRoot) { $candidates.Add($RequestedRoot) }
    if ($PSScriptRoot) { $candidates.Add($PSScriptRoot) }
    if ($PSScriptRoot) {
        $parent = Split-Path -Parent $PSScriptRoot
        if ($parent) { $candidates.Add($parent) }
    }
    try { $candidates.Add((Get-Location).Path) } catch {}

    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        try { $resolved = (Resolve-Path -LiteralPath $candidate -ErrorAction Stop).Path }
        catch { continue }

        if ((Test-Path -LiteralPath (Join-Path $resolved 'scripts\OneClickBuild.ps1')) -and
            (Test-Path -LiteralPath (Join-Path $resolved 'scripts\Build-Android.ps1')) -and
            (Test-Path -LiteralPath (Join-Path $resolved 'scripts\build_rocket_decomp.sh')) -and
            (Test-Path -LiteralPath (Join-Path $resolved 'scripts\self_check.py'))) {
            return $resolved
        }
    }

    throw @'
Could not find the Rocket-R repository root.
Extract the entire Rocket-R-PLATFORM-REPAIR-v16 folder into D:\Rocket-R and run
APPLY-Rocket-R-PLATFORM-REPAIR-v16.cmd again.
'@
}

function Test-PowerShellFile {
    param([string]$Path)
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$tokens, [ref]$errors)
    if ($errors -and $errors.Count -gt 0) {
        $message = ($errors | ForEach-Object { $_.Message }) -join '; '
        throw "PowerShell syntax validation failed for $Path : $message"
    }
}

function Invoke-NativeVisible {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @()
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 2>&1 | ForEach-Object { Write-Host ([string]$_) }
        return [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Replace-DecompParallelBlock {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $original = [IO.File]::ReadAllText($Path)
    $usesCrLf = $original.Contains("`r`n")
    $text = $original.Replace("`r`n", "`n").Replace("`r", "`n")

    $old = @'
JOBS="$(nproc)"
echo "Parallel jobs: $JOBS"
make -j"$JOBS"
'@

    $new = @'
# Rocket's generated Makefile contains aliased paths such as asm/entry.s and
# asm//entry.s which resolve to the same output object. Building those aliases
# concurrently can make two compiler processes overwrite entry.o while the
# linker is starting. Keep this byte-matching legacy stage deterministic.
JOBS=1
echo "Deterministic decomp jobs: $JOBS (serial build prevents aliased object races)"
make -j1
'@

    # v4 may already have serialised this block, but line endings, comments or
    # embedding/escaping inside OneClickBuild.ps1 can differ. Detect the actual
    # behaviour rather than requiring one byte-for-byte formatting variant.
    $hasSerialMake = [regex]::IsMatch($text, '(?m)^\s*make\s+-j1\s*$')
    $hasSerialJob = [regex]::IsMatch($text, '(?m)^\s*JOBS\s*=\s*1\s*$') -or $text.Contains('Deterministic decomp jobs:')
    if ($hasSerialMake -and $hasSerialJob) {
        Write-Host "[OK] $Label is already using a deterministic serial decomp build." -ForegroundColor DarkGreen
        return $false
    }

    if (-not $text.Contains($old)) {
        throw "$Label is neither the original FIXED34 parallel block nor an already-serialised v4 block. No guesswork was applied."
    }

    $text = $text.Replace($old, $new)
    if ($usesCrLf) { $text = $text.Replace("`n", "`r`n") }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $text, $utf8NoBom)
    Write-Host "[FIXED] $Label now builds the fragile Rocket NSUE ELF stage serially." -ForegroundColor Green
    return $true
}


function Normalize-And-Patch-AndroidRuntimeSource {
    param([Parameter(Mandatory = $true)][string]$Root)

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $rendererPath = Join-Path $Root 'src\rt64_renderer.cpp'
    $mainPath = Join-Path $Root 'src\main.cpp'
    if (-not (Test-Path -LiteralPath $rendererPath)) { throw "Missing $rendererPath" }
    if (-not (Test-Path -LiteralPath $mainPath)) { throw "Missing $mainPath" }

    # Android Clang defines both __ANDROID__ and __linux__. Permanently exclude
    # Android from Rocket-R's Linux SDL_Window branch so ANativeWindow is used.
    $renderer = [IO.File]::ReadAllText($rendererPath)
    $safeLinux = '(?m)^#elif defined\(__linux__\)[ \t]*&&[ \t]*!defined\(__ANDROID__\)[ \t]*\r?$'
    $plainLinux = '(?m)^#elif defined\(__linux__\)[ \t]*\r?$'
    $safeCount = ([regex]::Matches($renderer, $safeLinux)).Count
    $plainCount = ([regex]::Matches($renderer, $plainLinux)).Count
    if ($safeCount -eq 0 -and $plainCount -eq 1) {
        $renderer = [regex]::Replace($renderer, $plainLinux, '#elif defined(__linux__) && !defined(__ANDROID__)', 1)
        Write-Host '[FIXED] rt64_renderer.cpp permanently excludes Android from the Linux SDL_Window branch.' -ForegroundColor Green
    }
    elseif ($safeCount -eq 1 -and $plainCount -eq 0) {
        Write-Host '[OK] rt64_renderer.cpp Android/Linux guard is already installed.' -ForegroundColor DarkGreen
    }
    else {
        throw "rt64_renderer.cpp Linux branch state is unexpected: plain=$plainCount safe=$safeCount."
    }

    # Rocket-R already supplies app-private config storage on Android. Never let
    # pinned RT64 attempt its desktop Linux home-directory probe (/data).
    $dataGuard = '(?s)#if[ \t]+defined\(__ANDROID__\)[ \t]*\r?\n[ \t]*.*?app-private.*?\r?\n[ \t]*.*?/data.*?\r?\n[ \t]*app_config\.detectDataPath[ \t]*=[ \t]*false;[ \t]*\r?\n#else[ \t]*\r?\n[ \t]*app_config\.detectDataPath[ \t]*=[ \t]*true;[ \t]*\r?\n#endif'
    if ($renderer -notmatch $dataGuard) {
        $detectPattern = '(?m)^[ \t]*app_config\.detectDataPath[ \t]*=[ \t]*true;[ \t]*\r?$'
        $detectCount = ([regex]::Matches($renderer, $detectPattern)).Count
        if ($detectCount -ne 1) {
            throw "rt64_renderer.cpp expected one detectDataPath=true assignment or the Android guard; found $detectCount."
        }
        $nl = if ($renderer.Contains("`r`n")) { "`r`n" } else { "`n" }
        $replacement = '#if defined(__ANDROID__)' + $nl +
            '    // Android already supplies an app-private --config directory.' + $nl +
            '    // Never let RT64 probe the desktop Linux home (/data on Android).' + $nl +
            '    app_config.detectDataPath = false;' + $nl +
            '#else' + $nl +
            '    app_config.detectDataPath = true;' + $nl +
            '#endif'
        $renderer = [regex]::Replace($renderer, $detectPattern, $replacement, 1)
        Write-Host '[FIXED] Android RT64 desktop data-path probing is permanently disabled.' -ForegroundColor Green
    }
    else {
        Write-Host '[OK] Android RT64 data-path guard is already installed.' -ForegroundColor DarkGreen
    }

    if ($renderer -notmatch 'app_config\.useConfigurationFile[ \t]*=[ \t]*false;' -or
        $renderer -notmatch 'app_config\.detectDataPath[ \t]*=[ \t]*false;' -or
        $renderer -notmatch $safeLinux -or
        $renderer -notmatch 'android_native_window\(\)') {
        throw 'Android RT64 renderer/data-path source verification failed.'
    }
    [IO.File]::WriteAllText($rendererPath, $renderer, $utf8NoBom)

    # Install Android's Java-picker -> native-game handoff permanently. This is
    # semantic and CRLF/LF agnostic. Build-Android never rewrites main.cpp again.
    $main = [IO.File]::ReadAllText($mainPath)
    $directMarker = 'bypassing desktop launcher and starting Rocket directly'
    if (-not $main.Contains($directMarker)) {
        $baselinePattern = '(?ms)^[ \t]*const auto startup[ \t]*=[ \t]*rocket::ui::run_launcher\([ \t\r\n]*rocket::platform::sdl_window\(\),[ \t\r\n]*options\.rom\);[ \t]*\r?$'
        $baselineCount = ([regex]::Matches($main, $baselinePattern)).Count
        if ($baselineCount -ne 1) {
            throw "main.cpp expected one desktop launcher handoff or the installed Android guard; found $baselineCount baseline matches."
        }
        $nl = if ($main.Contains("`r`n")) { "`r`n" } else { "`n" }
        $replacementLines = @(
            '    rocket::ui::StartupResult startup{};',
            '#if defined(__ANDROID__)',
            '    if (options.rom.empty()) {',
            '        std::fprintf(stderr, "[android] no private ROM path was supplied by RocketActivity\n");',
            '        rocket::platform::shutdown();',
            '        return 5;',
            '    }',
            '    std::string android_rom_error;',
            '    if (!rocket::select_rom(options.rom, android_rom_error)) {',
            '        std::fprintf(stderr, "[android] private ROM validation failed: %s\n", android_rom_error.c_str());',
            '        rocket::platform::shutdown();',
            '        return 5;',
            '    }',
            '    startup.launch = true;',
            '    startup.rom_path = options.rom;',
            '    std::fprintf(stderr, "[android] private ROM verified; bypassing desktop launcher and starting Rocket directly\n");',
            '#else',
            '    startup = rocket::ui::run_launcher(',
            '        rocket::platform::sdl_window(), options.rom);',
            '#endif'
        )
        $replacement = $replacementLines -join $nl
        $main = [regex]::Replace($main, $baselinePattern, [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $replacement }, 1)
        Write-Host '[FIXED] main.cpp permanently bypasses the desktop launcher on Android after Java ROM import.' -ForegroundColor Green
    }
    else {
        Write-Host '[OK] Android direct-ROM launcher guard is already installed in main.cpp.' -ForegroundColor DarkGreen
    }

    if ($main -notmatch 'bypassing desktop launcher and starting Rocket directly' -or
        $main -notmatch 'rocket::select_rom\(options\.rom' -or
        $main -notmatch '(?s)#if defined\(__ANDROID__\).*?#else.*?rocket::ui::run_launcher') {
        throw 'Android direct-ROM launcher source verification failed.'
    }
    [IO.File]::WriteAllText($mainPath, $main, $utf8NoBom)

    foreach ($relative in @('build\android-project','build\android-apk-scan')) {
        $path = Join-Path $Root $relative
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host '[OK] Generated Android project/cache state cleared.' -ForegroundColor DarkGreen
}


$Root = Resolve-RocketRoot $ProjectRoot
$BuilderPath = Join-Path $Root 'scripts\OneClickBuild.ps1'
$AndroidPath = Join-Path $Root 'scripts\Build-Android.ps1'
$DecompHelperPath = Join-Path $Root 'scripts\build_rocket_decomp.sh'
$SelfCheckPath = Join-Path $Root 'scripts\self_check.py'
$PayloadAndroidPath = Join-Path $PSScriptRoot 'Payload\Build-Android.ps1'
$LinuxBuildPath = Join-Path $Root 'Build-Linux.sh'
$AppImagePackagerPath = Join-Path $Root 'scripts\package_appimage.sh'
$PayloadLinuxBuildPath = Join-Path $PSScriptRoot 'Payload\Build-Linux.sh'
$PayloadAppImagePackagerPath = Join-Path $PSScriptRoot 'Payload\package_appimage.sh'
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

Write-Host ''
Write-Host 'Rocket-R platform repair v16' -ForegroundColor Cyan
Write-Host "Repository: $Root"
Write-Host ''
Write-Host 'v16 keeps the v15 Android source-guard design and repairs Linux/AppImage launchability:' -ForegroundColor DarkGray
Write-Host '  1. Android/PowerShell native STDERR handling (v3 fix retained).' -ForegroundColor DarkGray
Write-Host '  2. Nondeterministic parallel Rocket ELF/decomp object race.' -ForegroundColor DarkGray
Write-Host '  3. Android RT64 SDL2_INCLUDE_DIRS was empty during CMake configure.' -ForegroundColor DarkGray
Write-Host '  4. RT64-pinned zstd incorrectly selects GNU qsort_r() on Android.' -ForegroundColor DarkGray
Write-Host '  5. Handles _GNU_SOURCE already being defined by the Android toolchain.' -ForegroundColor DarkGray
Write-Host '  6. RT64 shader file_to_c.py now works when CMake selects Python 3.9.' -ForegroundColor DarkGray
Write-Host '  7. Android no longer falls into Rocket-R''s Linux SDL_Window branch.' -ForegroundColor DarkGray
Write-Host '  8. RT64 ApplicationWindow no longer compiles Linux/X11 branches on Android.' -ForegroundColor DarkGray
Write-Host '  9. RT64 generic SDL/Vulkan branches no longer treat ANativeWindow* as SDL_Window*.' -ForegroundColor DarkGray
Write-Host ' 10. Android direct-ROM launcher handoff is installed permanently in src\main.cpp.' -ForegroundColor DarkGray
Write-Host ' 11. Build-Android verifies repository source instead of rewriting main.cpp/rt64_renderer.cpp every build.' -ForegroundColor DarkGray
Write-Host ' 12. Linux AppImages now use zstd SquashFS; modern Type-2 runtimes cannot mount the old XZ payload.' -ForegroundColor DarkGray
Write-Host ' 13. AppImage runtime is pinned to immutable 20251108 assets with SHA-256 verification.' -ForegroundColor DarkGray
Write-Host ' 14. Every AppImage is extraction-validated before the Linux build is accepted.' -ForegroundColor DarkGray
Write-Host ' 15. Linux builds also emit a Portable.tar.gz that preserves executable bits through Windows/NTFS transfers.' -ForegroundColor DarkGray
Write-Host ' 16. x86_64 is labelled as the Steam Deck build; aarch64 is explicitly ARM64-only.' -ForegroundColor DarkGray
Write-Host 'FIXED27 gameplay/interpolation behaviour remains unchanged; only Android platform guards are added to repository-owned source.' -ForegroundColor DarkGray
Write-Host ''

$builder = [IO.File]::ReadAllText($BuilderPath)
if ($builder -notmatch "\`$BuilderRevision\s*=\s*'FIXED34'") {
    throw 'This repair expects scripts\OneClickBuild.ps1 to be FIXED34. No files were changed.'
}
Write-Host '[OK] Main builder is FIXED34.' -ForegroundColor DarkGreen

if (-not (Test-Path -LiteralPath $PayloadAndroidPath)) { throw "Missing Android payload: $PayloadAndroidPath" }
if (-not (Test-Path -LiteralPath $PayloadLinuxBuildPath)) { throw "Missing Linux builder payload: $PayloadLinuxBuildPath" }
if (-not (Test-Path -LiteralPath $PayloadAppImagePackagerPath)) { throw "Missing AppImage payload: $PayloadAppImagePackagerPath" }

# Back up scripts and the two repository-owned runtime sources before modifying anything.
$backupDir = Join-Path $Root ("build\repair-backups\platform-v16-" + $Stamp)
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
Copy-Item -LiteralPath $BuilderPath -Destination (Join-Path $backupDir 'OneClickBuild.ps1') -Force
Copy-Item -LiteralPath $DecompHelperPath -Destination (Join-Path $backupDir 'build_rocket_decomp.sh') -Force
Copy-Item -LiteralPath $AndroidPath -Destination (Join-Path $backupDir 'Build-Android.ps1') -Force
Copy-Item -LiteralPath $LinuxBuildPath -Destination (Join-Path $backupDir 'Build-Linux.sh') -Force
Copy-Item -LiteralPath $AppImagePackagerPath -Destination (Join-Path $backupDir 'package_appimage.sh') -Force
Copy-Item -LiteralPath (Join-Path $Root 'src\rt64_renderer.cpp') -Destination (Join-Path $backupDir 'rt64_renderer.cpp') -Force
Copy-Item -LiteralPath (Join-Path $Root 'src\main.cpp') -Destination (Join-Path $backupDir 'main.cpp') -Force
Write-Host "[OK] Backups created: $backupDir" -ForegroundColor DarkGreen

Normalize-And-Patch-AndroidRuntimeSource -Root $Root

# Keep v3's robust Android helper installed so v4 is fully self-contained.
Copy-Item -LiteralPath $PayloadAndroidPath -Destination $AndroidPath -Force
Test-PowerShellFile $AndroidPath
$android = [IO.File]::ReadAllText($AndroidPath)
foreach ($required in @('Android build helper: FIXED34 robust-native v16',
                         'function Invoke-GradleStable',
                         'gradle-android-',
                         'function Invoke-NativeVisible',
                         'function Get-JavaVersionText',
                         'foreach ($candidateHome in $candidates)',
                         'SDL2_INCLUDE_DIRS',
                         'Android SDL2 include path:',
                         'Android zstd qsort compatibility:',
                         'full Android qsort fallback',
                         'Android RT64 file_to_c compatibility:',
                         'Python 3.10-only newline= arguments',
                         'Android Rocket renderer compatibility:',
                         'Android RT64 window compatibility:',
                         'ANativeWindow',
                         'Linux/X11-only branches',
                         'Android RT64 SDL/Vulkan compatibility:',
                         'generic SDL_Window branches',
                         'Android launcher compatibility:',
                         'bypassing desktop launcher and starting Rocket directly')) {
    if (-not $android.Contains($required)) {
        throw "Android helper verification failed: missing '$required'."
    }
}
Write-Host '[FIXED] Robust Android native-command handling installed/retained.' -ForegroundColor Green

# Install Linux/AppImage repair. These are repository-owned build scripts, not
# gameplay/runtime code. Keep LF endings because WSL executes them directly.
Copy-Item -LiteralPath $PayloadLinuxBuildPath -Destination $LinuxBuildPath -Force
Copy-Item -LiteralPath $PayloadAppImagePackagerPath -Destination $AppImagePackagerPath -Force
$linuxBuilder = [IO.File]::ReadAllText($LinuxBuildPath)
$appImagePackager = [IO.File]::ReadAllText($AppImagePackagerPath)
foreach ($required in @('Steam Deck','Portable.tar.gz','--appimage-extract','linux/amd64','linux/arm64')) {
    if (-not $linuxBuilder.Contains($required)) { throw "Linux builder verification failed: missing '$required'." }
}
foreach ($required in @('-comp zstd','RUNTIME_TAG="20251108"','2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d','00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444','--appimage-extract')) {
    if (-not $appImagePackager.Contains($required)) { throw "AppImage packager verification failed: missing '$required'." }
}
if ($appImagePackager -match '-comp\s+xz') { throw 'Old XZ AppImage compression is still present after v16 repair.' }
Write-Host '[FIXED] Linux AppImage packaging now uses zstd + pinned Type-2 runtime + extraction validation.' -ForegroundColor Green

# Make the platform menu unambiguous and surface the permission-preserving
# portable archives in the One Click Builder's final artifact list.
$builderUi = [IO.File]::ReadAllText($BuilderPath)
$builderUi = $builderUi.Replace(
    "Write-Host '  [2] Linux x86-64 / x64 / AMD64 (.AppImage)'",
    "Write-Host '  [2] Linux x86-64 / x64 / AMD64 (.AppImage - Steam Deck / PC)'")
$builderUi = $builderUi.Replace(
    "Write-Host '  [3] Linux ARM64 / aarch64 (.AppImage)'",
    "Write-Host '  [3] Linux ARM64 / aarch64 (.AppImage - ARM devices; NOT Steam Deck)'")
if (-not $builderUi.Contains('LinuxX64Portable')) {
    $oldX64 = '$BuiltArtifacts.Add($LinuxX64Artifact)'
    $newX64 = @'
$BuiltArtifacts.Add($LinuxX64Artifact)
        $LinuxX64Portable = Join-Path $Dist "Rocket-R-$Version-Linux-x86_64-Portable.tar.gz"
        if (Test-Path $LinuxX64Portable) { $BuiltArtifacts.Add($LinuxX64Portable) }
'@
    if (-not $builderUi.Contains($oldX64)) { throw 'Could not locate Linux x86_64 artifact registration in OneClickBuild.ps1.' }
    $builderUi = $builderUi.Replace($oldX64, $newX64.TrimEnd())
}
if (-not $builderUi.Contains('LinuxArmPortable')) {
    $oldArm = '$BuiltArtifacts.Add($LinuxArmArtifact)'
    $newArm = @'
$BuiltArtifacts.Add($LinuxArmArtifact)
        $LinuxArmPortable = Join-Path $Dist "Rocket-R-$Version-Linux-aarch64-Portable.tar.gz"
        if (Test-Path $LinuxArmPortable) { $BuiltArtifacts.Add($LinuxArmPortable) }
'@
    if (-not $builderUi.Contains($oldArm)) { throw 'Could not locate Linux ARM64 artifact registration in OneClickBuild.ps1.' }
    $builderUi = $builderUi.Replace($oldArm, $newArm.TrimEnd())
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($BuilderPath, $builderUi, $utf8NoBom)
Test-PowerShellFile $BuilderPath
Write-Host '[FIXED] One Click platform labels/artifact list updated for Steam Deck and portable Linux archives.' -ForegroundColor Green


# Patch the embedded helper in OneClickBuild.ps1. This is essential because the
# builder recreates scripts\build_rocket_decomp.sh from this block every run.
[void](Replace-DecompParallelBlock -Path $BuilderPath -Label 'Embedded stage-5 helper in OneClickBuild.ps1')
Test-PowerShellFile $BuilderPath

# Patch the shipped helper too so self_check.py sees the same deterministic code
# before/after OneClickBuild recreates it.
[void](Replace-DecompParallelBlock -Path $DecompHelperPath -Label 'scripts\build_rocket_decomp.sh')

# Confirm both copies contain the serial build and no old 24-core/nproc form.
foreach ($path in @($BuilderPath, $DecompHelperPath)) {
    $check = [IO.File]::ReadAllText($path)
    if ($check -notmatch 'Deterministic decomp jobs:' -or $check -notmatch 'make -j1') {
        throw "Deterministic decomp patch verification failed for $path."
    }
    if ($check -match 'JOBS="\$\(nproc\)"') {
        throw "Old nproc parallel decomp block still exists in $path."
    }
}
Write-Host '[OK] Both stage-5 helper copies are deterministic and serial.' -ForegroundColor DarkGreen

$Python = Get-Command python.exe -ErrorAction SilentlyContinue
if ($Python) {
    Write-Host ''
    Write-Host 'Running Rocket-R source self-check...' -ForegroundColor Cyan
    $checkExit = Invoke-NativeVisible $Python.Source @($SelfCheckPath, '--root', $Root)
    if ($checkExit -ne 0) {
        throw "Rocket-R source self-check failed after repair v16 (exit $checkExit)."
    }
    Write-Host '[OK] Rocket-R source self-check passed.' -ForegroundColor Green
}
else {
    Write-Host '[WARN] Python is not on PATH; self_check.py could not be run by the repair script.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'Platform repair v16 completed successfully.' -ForegroundColor Green
Write-Host ''
Write-Host 'Run ONE-CLICK-BUILD.cmd and select the platform(s) you want.' -ForegroundColor Cyan
Write-Host '  [2] Linux x86-64 - Steam Deck / normal x64 Linux' -ForegroundColor Cyan
Write-Host '  [3] Linux ARM64 - ARM64 Linux only, NOT Steam Deck' -ForegroundColor Cyan
Write-Host '  [4] Android ARM64 / arm64-v8a (.APK)' -ForegroundColor Cyan
Write-Host ''
Write-Host 'In stage 5.6 you should now see:' -ForegroundColor DarkGray
Write-Host '  Deterministic decomp jobs: 1 (serial build prevents aliased object races)' -ForegroundColor DarkGray
Write-Host 'Later, when Android stage 8 starts, you should see:' -ForegroundColor DarkGray
Write-Host '  Android build helper: FIXED34 robust-native v16' -ForegroundColor DarkGray

Write-Host ''
Write-Host 'For Linux builds v16 should report:' -ForegroundColor DarkGray
Write-Host '  Created and extraction-validated ...AppImage' -ForegroundColor DarkGray
Write-Host '  Linux x86_64 portable archive: ...Portable.tar.gz' -ForegroundColor DarkGray
Write-Host 'For Steam Deck, transfer/extract the x86_64 Portable.tar.gz on the Deck itself.' -ForegroundColor Yellow
