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
Extract the entire Rocket-R-ANDROID-REPAIR-v14 folder into D:\Rocket-R and run
APPLY-Rocket-R-ANDROID-REPAIR-v14.cmd again.
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

    # A v13 process that died inside Gradle could not execute its temporary
    # cleanup. Normalize the two repository-owned files back to the stable
    # FIXED34/v11 shape before installing the permanent Android data-path guard.
    $rendererPath = Join-Path $Root 'src\rt64_renderer.cpp'
    $mainPath = Join-Path $Root 'src\main.cpp'
    if (-not (Test-Path -LiteralPath $rendererPath)) { throw "Missing $rendererPath" }
    if (-not (Test-Path -LiteralPath $mainPath)) { throw "Missing $mainPath" }

    $renderer = [IO.File]::ReadAllText($rendererPath)

    $safeLinux = '(?m)^#elif defined\(__linux__\)\s*&&\s*!defined\(__ANDROID__\)\s*$'
    $plainLinux = '(?m)^#elif defined\(__linux__\)\s*$'
    $safeCount = ([regex]::Matches($renderer, $safeLinux)).Count
    $plainCount = ([regex]::Matches($renderer, $plainLinux)).Count
    if ($safeCount -eq 1 -and $plainCount -eq 0) {
        $renderer = [regex]::Replace($renderer, $safeLinux, '#elif defined(__linux__)', 1)
        Write-Host '[RESET] Removed a leftover temporary v10-v13 Linux-branch edit from rt64_renderer.cpp.' -ForegroundColor Yellow
    }
    elseif (-not ($safeCount -eq 0 -and $plainCount -eq 1)) {
        throw "rt64_renderer.cpp Linux branch state is unexpected: plain=$plainCount safe=$safeCount."
    }

    $dataGuard = '(?s)#if\s+defined\(__ANDROID__\)\s*\r?\n[ \t]*app_config\.detectDataPath\s*=\s*false;\s*\r?\n#else\s*\r?\n[ \t]*app_config\.detectDataPath\s*=\s*true;\s*\r?\n#endif'
    if ($renderer -notmatch $dataGuard) {
        $detectPattern = '(?m)^[ \t]*app_config\.detectDataPath\s*=\s*true;[ \t]*$'
        $detectCount = ([regex]::Matches($renderer, $detectPattern)).Count
        if ($detectCount -ne 1) {
            throw "rt64_renderer.cpp expected one detectDataPath=true assignment or the Android guard; found $detectCount."
        }
        $replacement = '#if defined(__ANDROID__)' + [Environment]::NewLine +
            '    // Android already supplies an app-private --config directory.' + [Environment]::NewLine +
            '    // Never let RT64 probe the desktop Linux home (/data on Android).' + [Environment]::NewLine +
            '    app_config.detectDataPath = false;' + [Environment]::NewLine +
            '#else' + [Environment]::NewLine +
            '    app_config.detectDataPath = true;' + [Environment]::NewLine +
            '#endif'
        $renderer = [regex]::Replace($renderer, $detectPattern, $replacement, 1)
        Write-Host '[FIXED] Android RT64 desktop data-path probing is disabled in normal source code.' -ForegroundColor Green
    }
    else {
        Write-Host '[OK] Android RT64 data-path guard is already installed.' -ForegroundColor DarkGreen
    }

    if ($renderer -notmatch 'app_config\.useConfigurationFile\s*=\s*false;' -or
        $renderer -notmatch 'app_config\.detectDataPath\s*=\s*false;') {
        throw 'Android RT64 data-path source verification failed.'
    }
    [IO.File]::WriteAllText($rendererPath, $renderer, $utf8NoBom)

    $main = [IO.File]::ReadAllText($mainPath)
    $directMarker = 'bypassing desktop launcher and starting Rocket directly'
    if ($main.Contains($directMarker)) {
        $directPattern = '(?ms)^[ \t]*rocket::ui::StartupResult startup\{\};\s*\r?\n#if defined\(__ANDROID__\).*?^[ \t]*#endif[ \t]*$'
        $directCount = ([regex]::Matches($main, $directPattern)).Count
        if ($directCount -ne 1) {
            throw "main.cpp contains the temporary Android direct-launch marker but its block could not be normalized uniquely (matches=$directCount)."
        }
        $baseline = '    const auto startup = rocket::ui::run_launcher(' + [Environment]::NewLine +
            '        rocket::platform::sdl_window(), options.rom);'
        $main = [regex]::Replace($main, $directPattern, $baseline, 1)
        Write-Host '[RESET] Removed a leftover temporary v11-v13 direct-launch edit from main.cpp.' -ForegroundColor Yellow
        [IO.File]::WriteAllText($mainPath, $main, $utf8NoBom)
    }

    if ($main -notmatch '(?s)const auto startup\s*=\s*rocket::ui::run_launcher\(\s*rocket::platform::sdl_window\(\),\s*options\.rom\);') {
        throw 'main.cpp launcher handoff was not restored to the stable FIXED34/v11 source shape.'
    }

    # A crashed Gradle run can leave its generated project/cache behind. The
    # Android helper recreates it from scratch, so delete only generated state.
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
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

Write-Host ''
Write-Host 'Rocket-R Android repair v14' -ForegroundColor Cyan
Write-Host "Repository: $Root"
Write-Host ''
Write-Host 'v14 rebuilds Android from the last known-good v11 pipeline and separates runtime fixes from build-time source patching:' -ForegroundColor DarkGray
Write-Host '  1. Android/PowerShell native STDERR handling (v3 fix retained).' -ForegroundColor DarkGray
Write-Host '  2. Nondeterministic parallel Rocket ELF/decomp object race.' -ForegroundColor DarkGray
Write-Host '  3. Android RT64 SDL2_INCLUDE_DIRS was empty during CMake configure.' -ForegroundColor DarkGray
Write-Host '  4. RT64-pinned zstd incorrectly selects GNU qsort_r() on Android.' -ForegroundColor DarkGray
Write-Host '  5. Handles _GNU_SOURCE already being defined by the Android toolchain.' -ForegroundColor DarkGray
Write-Host '  6. RT64 shader file_to_c.py now works when CMake selects Python 3.9.' -ForegroundColor DarkGray
Write-Host '  7. Android no longer falls into Rocket-R''s Linux SDL_Window branch.' -ForegroundColor DarkGray
Write-Host '  8. RT64 ApplicationWindow no longer compiles Linux/X11 branches on Android.' -ForegroundColor DarkGray
Write-Host '  9. RT64 generic SDL/Vulkan branches no longer treat ANativeWindow* as SDL_Window*.' -ForegroundColor DarkGray
Write-Host ' 10. Android skips the redundant desktop ImGui launcher after Java ROM import.' -ForegroundColor DarkGray
Write-Host 'FIXED27 gameplay/interpolation behaviour and pinned sources remain unchanged after the Android build.' -ForegroundColor DarkGray
Write-Host ''

$builder = [IO.File]::ReadAllText($BuilderPath)
if ($builder -notmatch "\`$BuilderRevision\s*=\s*'FIXED34'") {
    throw 'This repair expects scripts\OneClickBuild.ps1 to be FIXED34. No files were changed.'
}
Write-Host '[OK] Main builder is FIXED34.' -ForegroundColor DarkGreen

if (-not (Test-Path -LiteralPath $PayloadAndroidPath)) {
    throw "Missing Android payload: $PayloadAndroidPath"
}

# Back up scripts and the two repository-owned runtime sources before modifying anything.
$backupDir = Join-Path $Root ("build\repair-backups\android-v14-" + $Stamp)
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
Copy-Item -LiteralPath $BuilderPath -Destination (Join-Path $backupDir 'OneClickBuild.ps1') -Force
Copy-Item -LiteralPath $DecompHelperPath -Destination (Join-Path $backupDir 'build_rocket_decomp.sh') -Force
Copy-Item -LiteralPath $AndroidPath -Destination (Join-Path $backupDir 'Build-Android.ps1') -Force
Copy-Item -LiteralPath (Join-Path $Root 'src\rt64_renderer.cpp') -Destination (Join-Path $backupDir 'rt64_renderer.cpp') -Force
Copy-Item -LiteralPath (Join-Path $Root 'src\main.cpp') -Destination (Join-Path $backupDir 'main.cpp') -Force
Write-Host "[OK] Backups created: $backupDir" -ForegroundColor DarkGreen

Normalize-And-Patch-AndroidRuntimeSource -Root $Root

# Keep v3's robust Android helper installed so v4 is fully self-contained.
Copy-Item -LiteralPath $PayloadAndroidPath -Destination $AndroidPath -Force
Test-PowerShellFile $AndroidPath
$android = [IO.File]::ReadAllText($AndroidPath)
foreach ($required in @('Android build helper: FIXED34 robust-native v14',
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
        throw "Rocket-R source self-check failed after repair v14 (exit $checkExit)."
    }
    Write-Host '[OK] Rocket-R source self-check passed.' -ForegroundColor Green
}
else {
    Write-Host '[WARN] Python is not on PATH; self_check.py could not be run by the repair script.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'Android repair v14 completed successfully.' -ForegroundColor Green
Write-Host ''
Write-Host 'Run ONE-CLICK-BUILD.cmd and select:' -ForegroundColor Cyan
Write-Host '  [4] Android ARM64 / arm64-v8a (.APK)' -ForegroundColor Cyan
Write-Host ''
Write-Host 'In stage 5.6 you should now see:' -ForegroundColor DarkGray
Write-Host '  Deterministic decomp jobs: 1 (serial build prevents aliased object races)' -ForegroundColor DarkGray
Write-Host 'Later, when Android stage 8 starts, you should see:' -ForegroundColor DarkGray
Write-Host '  Android build helper: FIXED34 robust-native v14' -ForegroundColor DarkGray
