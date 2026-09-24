[CmdletBinding()]
param(
    [switch]$RepairDependencies,
    [switch]$NoPackage,
    [switch]$NoLaunch,
    [string[]]$Platforms
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $Root 'build'
$LogRoot = Join-Path $BuildRoot 'logs'
New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$LogPath = Join-Path $LogRoot "one-click-$Stamp.log"
$Version = (Get-Content (Join-Path $Root 'VERSION') -Raw).Trim()
$BuilderRevision = 'FIXED34'
if ($Version -notmatch '^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z._-]+)?$') {
    throw "Invalid VERSION value: '$Version'"
}
$script:WslDistro = $null
Start-Transcript -Path $LogPath -Force | Out-Null

function Banner([string]$Text) {
    Write-Host ''
    Write-Host ('=' * 78) -ForegroundColor DarkCyan
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host ('=' * 78) -ForegroundColor DarkCyan
}

function Ask-YesNo([string]$Question, [bool]$DefaultYes = $true) {
    $suffix = if ($DefaultYes) { '[Y/n]' } else { '[y/N]' }
    $answer = Read-Host "$Question $suffix"
    if ([string]::IsNullOrWhiteSpace($answer)) { return $DefaultYes }
    return $answer.Trim().StartsWith('y', [System.StringComparison]::OrdinalIgnoreCase)
}

function Select-BuildPlatforms([string[]]$RequestedPlatforms) {
    $canonical = [ordered]@{
        '1' = 'Windows-x64'
        '2' = 'Linux-x86_64'
        '3' = 'Linux-aarch64'
        '4' = 'Android-arm64'
    }

    $tokens = @()
    if ($RequestedPlatforms -and $RequestedPlatforms.Count -gt 0) {
        foreach ($item in $RequestedPlatforms) {
            if ($null -ne $item) { $tokens += ([string]$item -split '[,; ]+' | Where-Object { $_ }) }
        }
    } else {
        Write-Host ''
        Write-Host 'Which platform builds would you like to produce?' -ForegroundColor Cyan
        Write-Host '  [1] Windows x64 (.zip)'
        Write-Host '  [2] Linux x86-64 / x64 / AMD64 (.AppImage - Steam Deck / PC)'
        Write-Host '  [3] Linux ARM64 / aarch64 (.AppImage - ARM devices; NOT Steam Deck)'
        Write-Host '  [4] Android ARM64 / arm64-v8a (.APK)'
        Write-Host '  [A] All four platforms'
        Write-Host ''
        $answer = Read-Host 'Select one or more choices (example: 1,2,4)'
        if ([string]::IsNullOrWhiteSpace($answer)) { $answer = '1' }
        $tokens = @($answer -split '[,; ]+' | Where-Object { $_ })
    }

    $result = New-Object System.Collections.Generic.List[string]
    foreach ($raw in $tokens) {
        $token = ([string]$raw).Trim()
        if (-not $token) { continue }
        if ($token -match '^(?i:a|all|everything)$') {
            foreach ($value in $canonical.Values) { if (-not $result.Contains($value)) { $result.Add($value) } }
            continue
        }
        $normalized = $null
        if ($canonical.Contains($token)) {
            $normalized = $canonical[$token]
        } else {
            switch -Regex ($token) {
                '^(?i:windows|windows-x64|win|win64|x64-windows)$' { $normalized = 'Windows-x64'; break }
                '^(?i:linux|linux-x64|linux-x86_64|x86_64|amd64)$' { $normalized = 'Linux-x86_64'; break }
                '^(?i:linux-arm|linux-arm64|linux-aarch64|arm64-linux|aarch64)$' { $normalized = 'Linux-aarch64'; break }
                '^(?i:android|android-arm|android-arm64|arm64-v8a)$' { $normalized = 'Android-arm64'; break }
                default { throw "Unknown platform choice '$token'. Use 1,2,3,4 or A." }
            }
        }
        if ($normalized -and -not $result.Contains($normalized)) { $result.Add($normalized) }
    }
    if ($result.Count -eq 0) { throw 'Select at least one build platform.' }
    return [string[]]($result | ForEach-Object { $_ })
}

function Remove-StaleInterpolationExperimentFiles {
    # FIXED28-31 experimented with semantic transform/vertex interpolation.
    # FIXED34 intentionally returns to the user-qualified FIXED27 renderer.
    # If a new source ZIP/repair overlay was extracted over an older tree,
    # delete only those known obsolete files before the integrity check.
    $obsolete = @(
        'MIXED-SOURCE-RECOVERY.txt',
        'src\interpolation_identity.cpp',
        'src\interpolation_identity.hpp',
        'patches\rt64\0007-rocket-stable-transform-identity-resolver.patch',
        'patches\rt64\0008-reject-implausible-auto-transform-matches.patch',
        'patches\rt64\0009-rocket-dkrr-component-interpolation-policy.patch'
    )
    $removed = New-Object System.Collections.Generic.List[string]
    foreach ($relative in $obsolete) {
        $path = Join-Path $Root $relative
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
            $removed.Add($relative)
        }
    }
    if ($removed.Count -gt 0) {
        Write-Host ('Removed obsolete FIXED28-31 interpolation experiment files: ' + ($removed -join ', ')) -ForegroundColor Yellow
    }
}

function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machine;$user;$env:Path"
}

function Require-Winget {
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
        throw 'Automatic prerequisite installation needs winget/App Installer. Install App Installer from Microsoft Store, then rerun ONE-CLICK-BUILD.cmd.'
    }
}

function Install-Winget([string]$Id, [string]$Name, [string]$Override = '') {
    Require-Winget
    if (-not (Ask-YesNo "$Name is missing. Install it automatically now?" $true)) {
        throw "$Name is required. Install it, then rerun the builder."
    }
    $wingetArgs = @('install','--id',$Id,'--exact','--accept-package-agreements','--accept-source-agreements','--silent')
    if ($Override) { $wingetArgs += @('--override', $Override) }
    & winget.exe @wingetArgs
    if ($LASTEXITCODE -ne 0) { throw "winget could not install $Name (exit $LASTEXITCODE)." }
    Refresh-Path
}

function Find-Python {
    $python = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($python) {
        & $python.Source --version *> $null
        if ($LASTEXITCODE -eq 0) { return $python.Source }
    }
    $py = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($py) {
        & $py.Source -3 --version *> $null
        if ($LASTEXITCODE -eq 0) { return $py.Source }
    }
    return $null
}

function Invoke-Python([string[]]$Arguments, [string]$DetailLog = '') {
    $python = Find-Python
    if (-not $python) { throw 'Python was not found after prerequisite setup.' }
    $oldPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 can promote native stderr into error records.
        # Keep Python/Git diagnostics visible and use the native exit code as
        # the source of truth, matching Invoke-NativeLogged.
        $ErrorActionPreference = 'Continue'
        if ([IO.Path]::GetFileName($python).Equals('py.exe', [StringComparison]::OrdinalIgnoreCase)) {
            if ($DetailLog) {
                & $python -3 @Arguments 2>&1 |
                    Tee-Object -FilePath $DetailLog -Append |
                    ForEach-Object { Write-Host ([string]$_) }
            } else {
                & $python -3 @Arguments 2>&1 | ForEach-Object { Write-Host ([string]$_) }
            }
        } else {
            if ($DetailLog) {
                & $python @Arguments 2>&1 |
                    Tee-Object -FilePath $DetailLog -Append |
                    ForEach-Object { Write-Host ([string]$_) }
            } else {
                & $python @Arguments 2>&1 | ForEach-Object { Write-Host ([string]$_) }
            }
        }
        $pythonExit = [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($pythonExit -ne 0) {
        if ($DetailLog) {
            throw "Python command failed with exit $pythonExit. See $DetailLog"
        }
        throw "Python command failed with exit $pythonExit."
    }
}

function Invoke-NativeLogged([string]$Executable, [string[]]$Arguments, [string]$NativeLog) {
    $oldPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 represents some native stderr lines as error
        # records.  Keep them visible/logged without letting a harmless warning
        # trip the script's global Stop preference; the native exit code remains
        # the authority for success/failure.
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 2>&1 |
            Tee-Object -FilePath $NativeLog -Append |
            ForEach-Object { Write-Host ([string]$_) }
        return [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Import-VsEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $false }
    $install = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $install) { return $false }
    $vsdev = Join-Path $install 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $vsdev)) { return $false }
    $lines = & cmd.exe /d /s /c "`"$vsdev`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    foreach ($line in $lines) {
        $idx = $line.IndexOf('=')
        if ($idx -gt 0) {
            $name = $line.Substring(0,$idx)
            $value = $line.Substring($idx+1)
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
    return $true
}

function Test-CMakeMinimumVersion([string]$Candidate, [version]$MinimumVersion = [version]'3.24.0') {
    if ([string]::IsNullOrWhiteSpace($Candidate) -or -not (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
        return $false
    }

    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $versionOutput = @(& $Candidate --version 2>&1)
        $exitCode = $LASTEXITCODE
    }
    catch {
        return $false
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    if ($exitCode -ne 0) { return $false }
    $versionText = ($versionOutput | ForEach-Object { [string]$_ }) -join "`n"
    if ($versionText -notmatch 'cmake version\s+([0-9]+(?:\.[0-9]+){1,3})') { return $false }
    try { $detected = [version]$Matches[1] } catch { return $false }
    return ($detected -ge $MinimumVersion)
}

function Test-SuspiciousUnixToolPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $true }
    return ($Path -match '(?i)[\\/](?:msys2?|mingw(?:32|64)?|cygwin|devkitPro)[\\/]')
}

function Add-ToolCandidate([System.Collections.Generic.List[string]]$Candidates, [string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return }
    try {
        $expanded = [Environment]::ExpandEnvironmentVariables($Path.Trim())
        if (-not [IO.Path]::IsPathRooted($expanded)) { return }
        $Candidates.Add($expanded)
    }
    catch {}
}

function Get-NativeCMake {
    $candidates = New-Object System.Collections.Generic.List[string]

    # Prefer the Kitware MSI location. Unlike the devkitPro/MSYS CMake that can
    # shadow PATH, this is a native Windows build and works correctly with the
    # Visual Studio compiler environment and Ninja.
    foreach ($rootDir in @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($rootDir) { Add-ToolCandidate $candidates (Join-Path $rootDir 'CMake\bin\cmake.exe') }
    }
    Add-ToolCandidate $candidates 'C:\Program Files\CMake\bin\cmake.exe'
    Add-ToolCandidate $candidates 'C:\Program Files (x86)\CMake\bin\cmake.exe'
    if ($env:LOCALAPPDATA) {
        Add-ToolCandidate $candidates (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    }

    # Visual Studio also ships a native Windows CMake. It is a valid fallback
    # even when that particular VS-bundled version does not advertise the VS17
    # project generator; FIXED12 intentionally uses Ninja instead.
    $vswhere = $null
    if (${env:ProgramFiles(x86)}) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if ($vswhere -and (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        $installs = @(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)
        foreach ($installRaw in $installs) {
            $install = ([string]$installRaw).Trim()
            if (-not $install) { continue }
            Add-ToolCandidate $candidates (Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
        }
    }

    # WinGet's stable link/install locations are useful immediately after an
    # MSI install, before PATH broadcasts reach this PowerShell process.
    if ($env:LOCALAPPDATA) {
        Add-ToolCandidate $candidates (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\cmake.exe')
        $wingetRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
        if (Test-Path -LiteralPath $wingetRoot -PathType Container) {
            Get-ChildItem -LiteralPath $wingetRoot -Directory -Filter 'Kitware.CMake*' -ErrorAction SilentlyContinue | ForEach-Object {
                Get-ChildItem -LiteralPath $_.FullName -Filter 'cmake.exe' -File -Recurse -ErrorAction SilentlyContinue |
                    ForEach-Object { Add-ToolCandidate $candidates $_.FullName }
            }
        }
    }

    # PATH is last and Unix compatibility environments are explicitly rejected.
    Get-Command cmake.exe -All -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Source -and -not (Test-SuspiciousUnixToolPath $_.Source)) {
            Add-ToolCandidate $candidates $_.Source
        }
    }

    $seen = @{}
    foreach ($candidateRaw in $candidates) {
        $candidate = ([string]$candidateRaw).Trim()
        if (-not $candidate -or $seen.ContainsKey($candidate.ToLowerInvariant())) { continue }
        $seen[$candidate.ToLowerInvariant()] = $true
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        if (Test-SuspiciousUnixToolPath $candidate) {
            Write-Host "Rejected Unix/MSYS CMake: $candidate" -ForegroundColor Yellow
            continue
        }
        Write-Host "Probing native CMake candidate: $candidate" -ForegroundColor DarkGray
        if (Test-CMakeMinimumVersion $candidate ([version]'3.24.0')) {
            Write-Host "Accepted native Windows CMake: $candidate" -ForegroundColor DarkGreen
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        Write-Host "Rejected CMake (missing or older than 3.24): $candidate" -ForegroundColor Yellow
    }
    return $null
}

function Get-NativeNinja {
    $candidates = New-Object System.Collections.Generic.List[string]
    $vswhere = $null
    if (${env:ProgramFiles(x86)}) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if ($vswhere -and (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        $installs = @(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)
        foreach ($installRaw in $installs) {
            $install = ([string]$installRaw).Trim()
            if (-not $install) { continue }
            Add-ToolCandidate $candidates (Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')
        }
    }
    if ($env:LOCALAPPDATA) {
        Add-ToolCandidate $candidates (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\ninja.exe')
    }
    Get-Command ninja.exe -All -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Source -and -not (Test-SuspiciousUnixToolPath $_.Source)) {
            Add-ToolCandidate $candidates $_.Source
        }
    }

    $seen = @{}
    foreach ($candidateRaw in $candidates) {
        $candidate = ([string]$candidateRaw).Trim()
        if (-not $candidate -or $seen.ContainsKey($candidate.ToLowerInvariant())) { continue }
        $seen[$candidate.ToLowerInvariant()] = $true
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        if (Test-SuspiciousUnixToolPath $candidate) { continue }
        $oldPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $candidate --version *> $null
            $exitCode = $LASTEXITCODE
        } catch { $exitCode = 1 } finally { $ErrorActionPreference = $oldPreference }
        if ($exitCode -eq 0) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    return $null
}

function Get-ClangCl {
    $cmd = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source -and -not (Test-SuspiciousUnixToolPath $cmd.Source)) {
        return $cmd.Source
    }

    $vswhere = $null
    if (${env:ProgramFiles(x86)}) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if ($vswhere -and (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        $installs = @(& $vswhere -products * -property installationPath 2>$null)
        foreach ($installRaw in $installs) {
            $install = ([string]$installRaw).Trim()
            if (-not $install) { continue }
            foreach ($relative in @('VC\Tools\Llvm\x64\bin\clang-cl.exe','VC\Tools\Llvm\bin\clang-cl.exe')) {
                $candidate = Join-Path $install $relative
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
            }
        }
    }
    return $null
}

function Bash-Quote([string]$Value) {
    if ($Value.Contains("'")) {
        throw "The repository path contains a single quote, which cannot be passed safely to the WSL build shell. Move Rocket-R to a path without a single quote (for example C:\Rocket-R) and rerun."
    }
    return "'" + $Value + "'"
}

function Get-WslDistros {
    $raw = @(& wsl.exe -l -q 2>$null)
    if ($LASTEXITCODE -ne 0) { return @() }
    return @($raw | ForEach-Object { ([string]$_).Replace(([char]0).ToString(), [string]::Empty).Trim() } | Where-Object { $_ })
}

function Select-UbuntuDistro {
    $distros = @(Get-WslDistros)
    $ubuntu = @($distros | Where-Object { $_ -match '^(?i:Ubuntu)(?:-|$)' })
    if ($ubuntu.Count -eq 0) { return $null }
    $exact = @($ubuntu | Where-Object { $_ -ieq 'Ubuntu' })
    if ($exact.Count -gt 0) { return $exact[0] }
    return $ubuntu[0]
}

function Resolve-RocketBootstrap([string]$ElfPath) {
    if (-not (Test-Path -LiteralPath $ElfPath -PathType Leaf)) {
        throw "Cannot inspect Rocket ELF because it does not exist: $ElfPath"
    }
    if (-not $script:WslDistro) {
        throw 'Internal error: Ubuntu WSL distribution was not selected before Rocket ELF inspection.'
    }

    $wslElf = Resolve-WslPath $ElfPath
    $symbolsLog = Join-Path $LogRoot "rocket-elf-symbols-$Stamp.log"
    $entryDisasmLog = Join-Path $LogRoot "rocket-entry-disasm-$Stamp.log"
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $raw = @(& wsl.exe -d $script:WslDistro --exec mips-linux-gnu-readelf -Ws $wslElf 2>&1)
        $exitCode = $LASTEXITCODE
        $disasm = @(& wsl.exe -d $script:WslDistro --exec mips-linux-gnu-objdump -d --start-address=0x80000400 --stop-address=0x80000440 $wslElf 2>&1)
        $disasmExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($symbolsLog, ((($raw | ForEach-Object { [string]$_ }) -join "`n") + "`n"), $utf8NoBom)
    [System.IO.File]::WriteAllText($entryDisasmLog, ((($disasm | ForEach-Object { [string]$_ }) -join "`n") + "`n"), $utf8NoBom)
    if ($exitCode -ne 0) {
        throw "Could not inspect Rocket ELF symbols with mips-linux-gnu-readelf. See $symbolsLog"
    }
    if ($disasmExit -ne 0) {
        throw "Could not disassemble Rocket's retail startup stub with mips-linux-gnu-objdump. See $entryDisasmLog"
    }

    [uint32]$retailAddress = 0
    [uint32]$gameInitAddress = 0
    [uint32]$bssStartAddress = 0
    [uint32]$bssEndAddress = 0
    $foundRetail = $false
    $foundGameInit = $false
    $foundBssStart = $false
    $foundBssEnd = $false
    foreach ($rawLine in $raw) {
        $line = [string]$rawLine
        if ($line -match '^\s*\d+:\s+([0-9A-Fa-f]+)\s+\d+\s+FUNC\s+\S+\s+\S+\s+\S+\s+EntryPoint\s*$') {
            $retailAddress = [Convert]::ToUInt32($Matches[1], 16)
            $foundRetail = $true
        }
        elseif ($line -match '^\s*\d+:\s+([0-9A-Fa-f]+)\s+\d+\s+FUNC\s+\S+\s+\S+\s+\S+\s+game_init\s*$') {
            $gameInitAddress = [Convert]::ToUInt32($Matches[1], 16)
            $foundGameInit = $true
        }
        elseif ($line -match '^\s*\d+:\s+([0-9A-Fa-f]+)\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+_codesegs0_1SegmentBssStart\s*$') {
            $bssStartAddress = [Convert]::ToUInt32($Matches[1], 16)
            $foundBssStart = $true
        }
        elseif ($line -match '^\s*\d+:\s+([0-9A-Fa-f]+)\s+\d+\s+\S+\s+\S+\s+\S+\s+\S+\s+_codesegs0_1SegmentBssEnd\s*$') {
            $bssEndAddress = [Convert]::ToUInt32($Matches[1], 16)
            $foundBssEnd = $true
        }
    }
    if (-not $foundRetail -or -not $foundGameInit -or -not $foundBssStart -or -not $foundBssEnd) {
        throw "Rocket ELF is missing EntryPoint, game_init, or the codesegs0_1 BSS boundaries. See $symbolsLog"
    }

    $retailHex = '0x{0:X8}' -f $retailAddress
    $gameInitHex = '0x{0:X8}' -f $gameInitAddress
    $bssStartHex = '0x{0:X8}' -f $bssStartAddress
    $bssEndHex = '0x{0:X8}' -f $bssEndAddress
    if ($retailHex -ne '0x80000400') {
        throw "Pinned Rocket ELF resolved EntryPoint to $retailHex instead of expected 0x80000400. See $symbolsLog"
    }
    if ($gameInitHex -ne '0x80000E64') {
        throw "Pinned Rocket ELF resolved game_init to $gameInitHex instead of expected 0x80000E64. See $symbolsLog"
    }
    if ($bssStartHex -ne '0x800AF860' -or $bssEndHex -ne '0x800C00A0' -or $bssEndAddress -le $bssStartAddress) {
        throw "Pinned Rocket ELF resolved codesegs0_1 BSS to $bssStartHex-$bssEndHex instead of expected 0x800AF860-0x800C00A0. See $symbolsLog"
    }

    # Recover the bootstrap caller stack directly from Rocket's retail entry stub.
    # This is NOT gIdleThreadStack: game_init later creates thread 1 with
    # gIdleThreadStack[0x1000]. The cartridge bootstrap itself uses 0x803FFFF0.
    [uint32]$stackFromEntry = 0
    $stackHighFound = $false
    [uint64]$stackHigh = 0
    foreach ($rawLine in $disasm) {
        $line = [string]$rawLine
        if (-not $stackHighFound -and $line -match '(?i)\blui\s+\$?sp\s*,\s*0x([0-9a-f]+)') {
            $stackHigh = [Convert]::ToUInt64($Matches[1], 16) * 65536
            $stackHighFound = $true
            continue
        }
        if ($stackHighFound -and $line -match '(?i)\baddiu?\s+\$?sp\s*,\s*\$?sp\s*,\s*(-?0x[0-9a-f]+|-?\d+)') {
            $token = $Matches[1]
            [int64]$imm = 0
            if ($token -match '^-0x([0-9a-f]+)$') {
                $imm = -[Convert]::ToInt64($Matches[1], 16)
            }
            elseif ($token -match '^0x([0-9a-f]+)$') {
                $unsignedImm = [Convert]::ToInt64($Matches[1], 16)
                if ($unsignedImm -gt 0x7FFF) { $unsignedImm -= 0x10000 }
                $imm = $unsignedImm
            }
            else {
                $imm = [Convert]::ToInt64($token, 10)
            }
            $stackFromEntry = [uint32](([int64]$stackHigh + $imm) -band 0xFFFFFFFFL)
            break
        }
        if ($stackHighFound -and $line -match '(?i)\bori\s+\$?sp\s*,\s*\$?sp\s*,\s*0x([0-9a-f]+)') {
            $stackFromEntry = [uint32](($stackHigh + [Convert]::ToUInt64($Matches[1], 16)) -band 0xFFFFFFFFL)
            break
        }
    }
    if ($stackFromEntry -eq 0) {
        throw "Could not recover Rocket's bootstrap stack setup from the retail EntryPoint disassembly. See $entryDisasmLog"
    }
    $decodedStack = '0x{0:X8}' -f $stackFromEntry
    if ($decodedStack -ne '0x803FFFF0') {
        throw "Rocket EntryPoint sets sp=$decodedStack instead of the pinned retail value 0x803FFFF0. See $entryDisasmLog"
    }

    $disasmText = (($disasm | ForEach-Object { [string]$_ }) -join "`n")
    if ($disasmText -notmatch '(?im)^\s*80000430:.*\bjal\b.*80000e64') {
        throw "Rocket EntryPoint no longer calls game_init at 0x80000E64 from 0x80000430. See $entryDisasmLog"
    }
    if ($disasmText -notmatch '(?im)^\s*80000438:.*\bbreak\b') {
        throw "Rocket EntryPoint no longer contains the expected post-game_init BREAK at 0x80000438. See $entryDisasmLog"
    }

    Write-Host "Rocket retail ROM load address: EntryPoint at $retailHex" -ForegroundColor Green
    Write-Host "Rocket callable static-recomp entrypoint: game_init at $gameInitHex" -ForegroundColor Green
    Write-Host "Rocket bootstrap caller stack: $decodedStack (verified directly from EntryPoint disassembly)" -ForegroundColor Green
    Write-Host "Rocket bootstrap BSS clear: $bssStartHex-$bssEndHex" -ForegroundColor Green
    Write-Host "Rocket ELF symbol report: $symbolsLog" -ForegroundColor DarkGreen
    Write-Host "Rocket EntryPoint disassembly: $entryDisasmLog" -ForegroundColor DarkGreen
    return [PSCustomObject]@{
        Retail = $retailHex
        GameInit = $gameInitHex
        InitialStack = $decodedStack
        BssStart = $bssStartHex
        BssEnd = $bssEndHex
    }
}

function Invoke-WslBash([string]$Command) {
    if (-not $script:WslDistro) { throw 'Internal error: Ubuntu WSL distribution was not selected.' }
    & wsl.exe -d $script:WslDistro -- bash -lc $Command
    if ($LASTEXITCODE -ne 0) { throw "WSL command failed with exit $LASTEXITCODE in $script:WslDistro." }
}

function Resolve-WslPath([string]$WindowsPath) {
    if (-not $script:WslDistro) { throw 'Internal error: Ubuntu WSL distribution was not selected.' }

    # Do not pass a raw Windows path such as D:\Rocket-R as a native WSL
    # command argument. Windows PowerShell 5.1 can lose the backslash at that
    # boundary. WSLENV's /p flag asks WSL itself to translate the environment
    # variable from a Windows path to the selected distro's Linux mount path.
    $fullWindowsPath = [IO.Path]::GetFullPath($WindowsPath)
    $bridgeName = 'ROCKET_R_BUILDER_WINPATH'
    $oldBridgeValue = [Environment]::GetEnvironmentVariable($bridgeName, 'Process')
    $oldWslEnv = [Environment]::GetEnvironmentVariable('WSLENV', 'Process')
    $oldPreference = $ErrorActionPreference

    try {
        [Environment]::SetEnvironmentVariable($bridgeName, $fullWindowsPath, 'Process')

        $wslEnvEntries = @()
        if (-not [string]::IsNullOrWhiteSpace($oldWslEnv)) {
            $wslEnvEntries = @($oldWslEnv.Split(':') | Where-Object { $_ -and $_ -notmatch '^ROCKET_R_BUILDER_WINPATH(?:/.*)?$' })
        }
        $wslEnvEntries += ($bridgeName + '/p')
        [Environment]::SetEnvironmentVariable('WSLENV', ($wslEnvEntries -join ':'), 'Process')

        $ErrorActionPreference = 'Continue'
        $rawResult = @(& wsl.exe -d $script:WslDistro -- printenv $bridgeName 2>&1)
        $wslExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
        [Environment]::SetEnvironmentVariable($bridgeName, $oldBridgeValue, 'Process')
        [Environment]::SetEnvironmentVariable('WSLENV', $oldWslEnv, 'Process')
    }

    $result = @($rawResult | ForEach-Object {
        ([string]$_).Replace(([char]0).ToString(), [string]::Empty).Trim()
    } | Where-Object { $_ } | Select-Object -Last 1)

    if ($wslExitCode -ne 0 -or $result.Count -eq 0 -or [string]::IsNullOrWhiteSpace($result[0]) -or -not $result[0].StartsWith('/')) {
        $detail = if ($rawResult) { (($rawResult | ForEach-Object { [string]$_ }) -join ' ').Trim() } else { 'no output' }
        throw "Could not translate '$fullWindowsPath' into a WSL path in $script:WslDistro. WSL path bridge said: $detail"
    }

    return $result[0].Trim()
}

function Ensure-RocketDecompHelper {
    # Stage 5 is deliberately embedded in the Windows builder as well as shipped
    # as a normal source file. Recreate it on every run so an incomplete patch
    # extraction, antivirus cleanup, or stale checkout cannot strand the build.
    $helperPath = Join-Path $Root 'scripts\build_rocket_decomp.sh'
    $helperText = @'
#!/usr/bin/env bash
set -Eeuo pipefail

# Compatibility markers for FIXED6 source_self_check.py when FIXED7 is dropped
# into an existing working folder as a single-file builder replacement:
# 5.2 - Create isolated Splat Python environment
# 5.3 - Split the verified NSUE ROM with pinned Splat
# 5.4 - Build/verify Rocket's legacy compiler tools
# 5.5 - Build byte-matching Rocket NSUE ROM and ELF

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <rocket-r-root> <detail-log>" >&2
  exit 64
fi

ROOT="$1"
DETAIL_LOG="$2"
SOURCE_DECOMP="$ROOT/extern/rocket-decomp"
ROM="$ROOT/build/private/rocket.us.z64"
NATIVE_BASE="${ROCKET_R_WSL_WORKSPACE:-$HOME/.cache/rocket-r}"
DECOMP="$NATIVE_BASE/rocket-decomp"
VENV="$DECOMP/.rocket-venv"

mkdir -p "$(dirname "$DETAIL_LOG")"
: > "$DETAIL_LOG"
exec > >(tee -a "$DETAIL_LOG") 2>&1

echo "[Rocket decomp helper] started: $0"
echo "[Rocket decomp helper] Rocket-R root: $ROOT"
echo "[Rocket decomp helper] source checkout: $SOURCE_DECOMP"
echo "[Rocket decomp helper] native workspace: $DECOMP"
echo "[Rocket decomp helper] detail log: $DETAIL_LOG"

CURRENT_STEP="initialization"
trap 'rc=$?; echo; echo "[FAILED] Step: $CURRENT_STEP"; echo "[FAILED] Exit code: $rc"; echo "[FAILED] Command: $BASH_COMMAND"; echo "[FAILED] Detail log: $DETAIL_LOG"; exit $rc' ERR

step() {
  CURRENT_STEP="$1"
  echo
  echo "=============================================================================="
  echo "  $1"
  echo "=============================================================================="
}

step "5.1 - Validate Rocket decomp inputs"
echo "Rocket-R root:      $ROOT"
echo "Source decomp:      $SOURCE_DECOMP"
echo "Native WSL decomp:  $DECOMP"
echo "Detail log:         $DETAIL_LOG"
[[ -d "$SOURCE_DECOMP/.git" ]] || { echo "Pinned Rocket decomp checkout is missing: $SOURCE_DECOMP" >&2; exit 10; }
[[ -f "$ROM" ]] || { echo "Validated private ROM is missing: $ROM" >&2; exit 11; }
[[ -f "$SOURCE_DECOMP/Makefile" ]] || { echo "Rocket decomp Makefile is missing." >&2; exit 12; }
[[ -f "$SOURCE_DECOMP/tools/NSUE.00.yaml" ]] || { echo "Rocket NSUE Splat config is missing." >&2; exit 13; }
SOURCE_COMMIT="$(git -C "$SOURCE_DECOMP" rev-parse HEAD)"
echo "Pinned decomp commit: $SOURCE_COMMIT"
echo "ROM bytes:            $(stat -c%s "$ROM")"

step "5.2 - Stage Rocket decomp on native WSL filesystem"
# Rocket uses 32-bit GCC 2.7.2/SN64 executables. Their old 32-bit stat ABI can
# overflow on DrvFS/9P inode metadata under /mnt/<drive>, producing EOVERFLOW
# ('Value too large for defined data type'). Build on WSL's native ext4 instead.
rm -rf "$DECOMP"
mkdir -p "$DECOMP"
(
  cd "$SOURCE_DECOMP"
  tar \
    --exclude='./.rocket-venv' \
    --exclude='./build' \
    --exclude='./asm' \
    --exclude='./data' \
    --exclude='./baserom.us.z64' \
    -cf - .
) | (
  cd "$DECOMP"
  tar -xf -
)
cp -f "$ROM" "$DECOMP/baserom.us.z64"
cd "$DECOMP"
NATIVE_FS="$(stat -f -c %T .)"
echo "Native workspace filesystem: $NATIVE_FS"
case "$DECOMP" in
  /mnt/*)
    echo "Refusing to run legacy Rocket compilers from a Windows-mounted WSL path: $DECOMP" >&2
    exit 25
    ;;
esac
[[ "$(git rev-parse HEAD)" == "$SOURCE_COMMIT" ]] || { echo "Native staged decomp commit mismatch." >&2; exit 26; }
echo "Native staged decomp commit: $(git rev-parse HEAD)"

step "5.3 - Create isolated Splat Python environment"
rm -rf "$VENV"
python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip setuptools wheel
if [[ -f tools/splat/requirements.txt ]]; then
  "$VENV/bin/python" -m pip install -r tools/splat/requirements.txt
  # Rocket pins Splat 0.12.10 from December 2022. Its requirements only set a
  # minimum spimdisasm version, so a fresh install in 2026 otherwise pulls a
  # much newer disassembler whose generated assembly contains marker macros
  # (nonmatching/enddlabel) that this Rocket checkout does not define.
  # Freeze spimdisasm to the historical minimum this Splat commit declared.
  "$VENV/bin/python" -m pip install --upgrade --force-reinstall "spimdisasm==1.9.0"
else
  echo "Splat requirements file is missing." >&2
  exit 14
fi
"$VENV/bin/python" -m pip check
"$VENV/bin/python" - <<'PYIMPORT'
import rabbitizer
import spimdisasm
import tqdm
import yaml
from colorama import Fore, Style
from intervaltree import Interval, IntervalTree
print("Splat core Python imports: PASS")
print(f"spimdisasm runtime version: {spimdisasm.__version__}")
if spimdisasm.__version__ != "1.9.0":
    raise RuntimeError(f"Expected historical spimdisasm 1.9.0, got {spimdisasm.__version__}")
try:
    from yaml import CLoader
    print("PyYAML LibYAML CLoader: PASS")
except Exception as exc:
    print(f"PyYAML LibYAML CLoader: unavailable ({exc}); pure Python loader remains usable")
PYIMPORT

echo "Resolved Python package versions:"
"$VENV/bin/python" -m pip freeze | sort
export PATH="$VENV/bin:$PATH"

step "5.4 - Split the verified NSUE ROM with pinned Splat"
make setup
[[ -d asm ]] || { echo "make setup completed but asm/ was not generated." >&2; exit 15; }
[[ -f NSUE.ld ]] || { echo "make setup completed but NSUE.ld was not generated." >&2; exit 16; }
# Newer spimdisasm versions (1.36+) emit marker macros which this 2022 Rocket
# include_asm.h does not define. Refuse to start compilation if they reappear.
if grep -R -n -E '^[[:space:]]*(nonmatching|enddlabel)([[:space:]]|$)' asm > "$NATIVE_BASE/unexpected-asm-markers.txt" 2>/dev/null; then
  echo "Generated assembly contains unsupported post-2022 spimdisasm markers:" >&2
  head -n 20 "$NATIVE_BASE/unexpected-asm-markers.txt" >&2 || true
  echo "Historical Splat dependency lock did not take effect." >&2
  exit 30
fi
echo "Generated assembly compatibility scan: PASS (no nonmatching/enddlabel markers)"

step "5.5 - Build/verify Rocket's legacy compiler tools"
make -C tools all
[[ -x tools/gcc-2.7.2/gcc ]] || { echo "KMC GCC 2.7.2 was not produced." >&2; exit 17; }
[[ -x tools/gcc-2.7.2/as ]] || { echo "KMC binutils 2.6 assembler was not produced." >&2; exit 18; }
[[ -x tools/modern-sn64/gcc ]] || { echo "SN64 GCC was not produced." >&2; exit 19; }
[[ -x tools/modern-sn64/modern-asn64.py ]] || { echo "modern-asn64.py was not produced/executable." >&2; exit 20; }
echo "Legacy tool architecture/runtime diagnostics:"
file tools/gcc-2.7.2/gcc tools/gcc-2.7.2/as tools/modern-sn64/gcc || true
for tool in tools/gcc-2.7.2/gcc tools/gcc-2.7.2/as tools/modern-sn64/gcc; do
  echo "+ $tool --version"
  "$tool" --version </dev/null | head -n 3 || { echo "Legacy compiler could not execute: $tool" >&2; exit 24; }
done

step "5.6 - Build byte-matching Rocket NSUE ROM and ELF"
# Rocket's generated Makefile contains aliased paths such as asm/entry.s and
# asm//entry.s which resolve to the same output object. Building those aliases
# concurrently can make two compiler processes overwrite entry.o while the
# linker is starting. Keep this byte-matching legacy stage deterministic.
JOBS=1
echo "Deterministic decomp jobs: $JOBS (serial build prevents aliased object races)"
make -j1

step "5.7 - Verify outputs and copy them back to Rocket-R"
[[ -f build/us/NSUE.elf ]] || { echo "build/us/NSUE.elf was not produced." >&2; exit 21; }
[[ -f build/us/NSUE.z64 ]] || { echo "build/us/NSUE.z64 was not produced." >&2; exit 22; }
cmp -s baserom.us.z64 build/us/NSUE.z64 || { echo "Generated NSUE.z64 does not byte-match the validated ROM." >&2; exit 23; }
WINDOWS_OUTPUT="$SOURCE_DECOMP/build/us"
mkdir -p "$WINDOWS_OUTPUT"
cp -f build/us/NSUE.elf "$WINDOWS_OUTPUT/NSUE.elf"
cp -f build/us/NSUE.z64 "$WINDOWS_OUTPUT/NSUE.z64"
[[ -f "$WINDOWS_OUTPUT/NSUE.elf" ]] || { echo "Failed to copy NSUE.elf back to Rocket-R." >&2; exit 27; }
cmp -s build/us/NSUE.elf "$WINDOWS_OUTPUT/NSUE.elf" || { echo "Copied NSUE.elf differs from the verified native build output." >&2; exit 28; }
cmp -s build/us/NSUE.z64 "$WINDOWS_OUTPUT/NSUE.z64" || { echo "Copied NSUE.z64 differs from the verified native build output." >&2; exit 29; }
echo "NSUE ELF: $(stat -c%s build/us/NSUE.elf) bytes"
echo "NSUE ROM byte-match: PASS"
echo "Copied verified ELF to: $WINDOWS_OUTPUT/NSUE.elf"
echo "Rocket matching-ELF stage: PASS"

'@
    # The closing marker above is intentionally normalized below. PowerShell 5.1
    # writes UTF-8 with a BOM via Set-Content; use .NET so Bash gets UTF-8/no-BOM
    # and LF line endings regardless of the Windows checkout policy.
    $helperText = $helperText.TrimStart() -replace "`r`n", "`n" -replace "`r", "`n"
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $scriptsDir = Split-Path $helperPath -Parent
    New-Item -ItemType Directory -Force -Path $scriptsDir | Out-Null
    [System.IO.File]::WriteAllText($helperPath, $helperText.TrimEnd() + "`n", $utf8NoBom)
    if (-not (Test-Path $helperPath)) {
        throw 'Could not create the embedded Rocket decomp helper.'
    }
    return $helperPath
}

function Find-BuiltFile([string]$Directory, [string]$Name) {
    $item = Get-ChildItem -Path $Directory -Recurse -File -Filter $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $item) { throw "Build completed but $Name was not found under $Directory." }
    return $item.FullName
}

try {
    Banner "Rocket-R ${Version}: local static recompilation builder"
    Write-Host "Repository: $Root"
    Write-Host "Build log:  $LogPath"
    Write-Host "Builder revision: $BuilderRevision"
    Write-Host 'This builder never uploads this repository or your ROM.' -ForegroundColor Green
    if ($Root.Length -gt 120) {
        Write-Host 'WARNING: This repository is in a long path. If a dependency hits Windows path-length limits, move the folder somewhere short such as C:\Rocket-R and rerun.' -ForegroundColor Yellow
    }

    $SelectedPlatforms = Select-BuildPlatforms $Platforms
    $BuildWindows = $SelectedPlatforms -contains 'Windows-x64'
    $BuildLinuxX64 = $SelectedPlatforms -contains 'Linux-x86_64'
    $BuildLinuxArm64 = $SelectedPlatforms -contains 'Linux-aarch64'
    $BuildAndroidArm64 = $SelectedPlatforms -contains 'Android-arm64'
    $NeedLinuxPackages = $BuildLinuxX64 -or $BuildLinuxArm64
    Write-Host ("Selected platforms: " + ($SelectedPlatforms -join ', ')) -ForegroundColor Green
    Write-Host 'FIXED34 deliberately restores the complete FIXED27 interpolation/runtime baseline; platform support is layered around it.' -ForegroundColor Green

    Banner '1/9 - Windows prerequisites + source integrity'
    Remove-StaleInterpolationExperimentFiles
    $DecompHelperPath = Ensure-RocketDecompHelper
    Write-Host "Stage-5 helper verified/recreated: $DecompHelperPath" -ForegroundColor DarkGreen
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'Rocket-R requires 64-bit Windows.' }

    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
        Install-Winget 'Git.Git' 'Git for Windows'
    }
    if (-not (Find-Python)) {
        Install-Winget 'Python.Python.3.12' 'Python 3.12'
    }
    Invoke-Python @((Join-Path $Root 'scripts\self_check.py'),'--root',$Root)

    if (-not (Import-VsEnvironment)) {
        Install-Winget 'Microsoft.VisualStudio.2022.BuildTools' 'Visual Studio 2022 Build Tools' '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.CMake.Project'
        if (-not (Import-VsEnvironment)) {
            Write-Host 'ACTION REQUIRED: Visual Studio Build Tools was installed, but its C++ environment is not active yet.' -ForegroundColor Yellow
            Write-Host 'Restart Windows, then double-click ONE-CLICK-BUILD.cmd again.' -ForegroundColor Yellow
            Stop-Transcript | Out-Null
            exit 20
        }
    }
    $NativeCMake = Get-NativeCMake
    if (-not $NativeCMake) {
        Write-Host 'No suitable native Windows CMake 3.24+ was found. Installing Kitware CMake...' -ForegroundColor Yellow
        Require-Winget
        if (-not (Ask-YesNo 'Native Kitware CMake is required. Install/repair it automatically now?' $true)) {
            throw 'Native Kitware CMake 3.24+ is required.'
        }
        $oldPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & winget.exe install --id Kitware.CMake --exact --accept-package-agreements --accept-source-agreements --silent
            $cmakeInstallExit = $LASTEXITCODE
        } finally { $ErrorActionPreference = $oldPreference }
        Refresh-Path
        # winget returns a non-zero status when the requested package is already
        # installed and has no upgrade. That is not a build failure if a valid
        # native cmake.exe exists after discovery.
        for ($cmakeProbeAttempt = 1; $cmakeProbeAttempt -le 5 -and -not $NativeCMake; $cmakeProbeAttempt++) {
            $NativeCMake = Get-NativeCMake
            if (-not $NativeCMake) { Start-Sleep -Milliseconds 500 }
        }
        if (-not $NativeCMake) {
            throw "Native Kitware CMake installation/discovery failed (winget exit $cmakeInstallExit)."
        }
    }
    Write-Host "Native Windows CMake: $NativeCMake" -ForegroundColor DarkGreen

    $NativeNinja = Get-NativeNinja
    if (-not $NativeNinja) {
        Install-Winget 'Ninja-build.Ninja' 'native Ninja'
        Refresh-Path
        $NativeNinja = Get-NativeNinja
    }
    if (-not $NativeNinja) { throw 'A native Windows Ninja executable could not be found after prerequisite setup.' }
    Write-Host "Native Ninja: $NativeNinja" -ForegroundColor DarkGreen

    $MsvcCl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $MsvcCl) { throw 'MSVC cl.exe was not found after importing the Visual Studio 2022 x64 developer environment.' }
    $MsvcCl = $MsvcCl.Source
    Write-Host "MSVC compiler: $MsvcCl" -ForegroundColor DarkGreen

    $ClangCl = Get-ClangCl
    if (-not $ClangCl) {
        throw 'clang-cl.exe was not found. In Visual Studio Installer, add the C++ Clang/LLVM support component, then rerun ONE-CLICK-BUILD.cmd.'
    }
    Write-Host "ClangCL compiler: $ClangCl" -ForegroundColor DarkGreen

    Banner '2/9 - WSL / Ubuntu decompilation toolchain'
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        throw 'WSL is not available. Enable Windows Subsystem for Linux from Windows Features (and Virtual Machine Platform if requested), restart, then rerun ONE-CLICK-BUILD.cmd.'
    }

    $script:WslDistro = Select-UbuntuDistro
    if (-not $script:WslDistro) {
        if (Ask-YesNo 'No Ubuntu WSL distribution is configured. Install Ubuntu now?' $true) {
            & wsl.exe --install -d Ubuntu
            if ($LASTEXITCODE -ne 0) {
                throw "Windows could not start the Ubuntu WSL installation (exit $LASTEXITCODE)."
            }
            Write-Host ''
            Write-Host 'ACTION REQUIRED: Ubuntu/WSL installation was started.' -ForegroundColor Yellow
            Write-Host 'If Windows requests a restart, restart. Then launch Ubuntu once, create its Linux username/password, and run ONE-CLICK-BUILD.cmd again.' -ForegroundColor Yellow
            Stop-Transcript | Out-Null
            exit 20
        }
        throw "An Ubuntu WSL distribution is required to build RocketRet's matching NSUE ELF."
    }
    Write-Host "Using WSL distribution: $script:WslDistro"
    & wsl.exe -d $script:WslDistro -- bash -lc 'printf WSL_READY'
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ACTION REQUIRED: $script:WslDistro exists but its first-run Linux account setup is incomplete." -ForegroundColor Yellow
        Write-Host "Launch $script:WslDistro once, create its username/password, close it, then rerun ONE-CLICK-BUILD.cmd." -ForegroundColor Yellow
        Stop-Transcript | Out-Null
        exit 20
    }
    # Prove Windows -> WSL path translation before dependency work or ROM selection.
    # This catches WSL interop/path configuration problems at the earliest safe point.
    $WslRoot = Resolve-WslPath $Root
    Write-Host "WSL repository path: $WslRoot" -ForegroundColor DarkGreen

    Write-Host 'The next command may ask for your Linux sudo password. That is expected.' -ForegroundColor Yellow
    $WslPackages = 'build-essential git python3 python3-venv python3-pip python3-dev libyaml-dev pkg-config make wget curl tar file libc6-i386 gcc-mips-linux-gnu binutils-mips-linux-gnu'
    if ($NeedLinuxPackages) {
        $WslPackages += ' rsync docker.io qemu-user-static binfmt-support'
    }
    # BUILDER v21.2: direct WSL package bootstrap helper
    # Keep multi-line Bash out of PowerShell/native-process command strings.
    # The helper owns the package list, sudo and all apt quoting.
    $WslBootstrapWindows = Join-Path $Root 'scripts\bootstrap_wsl_packages.sh'
    if (-not (Test-Path $WslBootstrapWindows)) {
        throw "Missing WSL package bootstrap helper: $WslBootstrapWindows"
    }
    $WslBootstrapScript = "$WslRoot/scripts/bootstrap_wsl_packages.sh"
    if ($NeedLinuxPackages) {
        & wsl.exe -d $script:WslDistro -- bash $WslBootstrapScript --need-linux-packages
    } else {
        & wsl.exe -d $script:WslDistro -- bash $WslBootstrapScript
    }
    if ($LASTEXITCODE -ne 0) {
        throw "WSL package bootstrap helper failed with exit $LASTEXITCODE in $script:WslDistro."
    }

    Banner '3/9 - Pinned source dependencies'
    $DependencyLog = Join-Path $LogRoot "dependency-bootstrap-$Stamp.log"
    Write-Host "Dependency bootstrap detail log: $DependencyLog" -ForegroundColor DarkGreen
    Write-Host 'FIXED34 prefers locally cached pinned Git commits, keeps the accepted FIXED27 renderer/interpolation patch set, and only fetches a dependency when its required object is missing.' -ForegroundColor DarkGreen
    $bootstrapArgs = @((Join-Path $Root 'scripts\bootstrap_dependencies.py'),'--root',$Root)
    if ($RepairDependencies) { $bootstrapArgs += '--repair' }
    Invoke-Python $bootstrapArgs $DependencyLog

    Banner '4/9 - Your Rocket US ROM'
    $PrivateDir = Join-Path $BuildRoot 'private'
    New-Item -ItemType Directory -Force -Path $PrivateDir | Out-Null
    $CanonicalRom = Join-Path $PrivateDir 'rocket.us.z64'
    $RomMetadata = Join-Path $PrivateDir 'rocket.us.json'
    $SourceRom = $null
    if ($env:ROCKET_ROM -and (Test-Path $env:ROCKET_ROM)) {
        $SourceRom = (Resolve-Path $env:ROCKET_ROM).Path
    } elseif (Test-Path $CanonicalRom) {
        $SourceRom = $CanonicalRom
        Write-Host 'Reusing the already validated private ROM copy.'
    } else {
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.OpenFileDialog
            $dialog.Title = 'Select your unmodified Rocket: Robot on Wheels (USA) ROM'
            $dialog.Filter = 'Nintendo 64 ROM (*.z64;*.v64;*.n64)|*.z64;*.v64;*.n64|All files (*.*)|*.*'
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                $SourceRom = $dialog.FileName
            }
        } catch {
            Write-Host 'Graphical file picker was unavailable; falling back to a path prompt.' -ForegroundColor Yellow
        }
        if (-not $SourceRom) { $SourceRom = Read-Host 'Enter the full path to your Rocket USA ROM' }
    }
    if (-not $SourceRom -or -not (Test-Path $SourceRom)) { throw 'No readable ROM was selected.' }
    Invoke-Python @((Join-Path $Root 'scripts\validate_rom.py'),$SourceRom,$CanonicalRom,'--metadata',$RomMetadata)

    Banner '5/9 - Matching Rocket NSUE ELF'
    $DecompLogName = "rocket-decomp-$Stamp.log"
    $DecompLog = Join-Path $LogRoot $DecompLogName
    $WslDecompLog = "$WslRoot/build/logs/$DecompLogName"
    # Recreate once more immediately before use. This makes stage 5 independent
    # of whatever happened to the source helper earlier in the run.
    $DecompHelperPath = Ensure-RocketDecompHelper
    $WslDecompScript = Resolve-WslPath $DecompHelperPath
    Write-Host "Rocket decomp detail log: $DecompLog" -ForegroundColor DarkGreen

    # Stage 5 does not need shell operators, so avoid bash -lc entirely here.
    # Windows PowerShell 5.1 can mangle a composed native command line even when
    # its embedded single-quoted paths are valid Bash. Pass every argument to
    # wsl.exe separately and let WSL exec /bin/bash directly instead.
    Write-Host "WSL helper path: $WslDecompScript" -ForegroundColor DarkGray
    $wslStage5Args = @('-d', $script:WslDistro, '--exec', '/bin/bash', $WslDecompScript, $WslRoot, $WslDecompLog)
    try {
        & wsl.exe @wslStage5Args
        if ($LASTEXITCODE -ne 0) {
            throw "Rocket decomp helper exited with $LASTEXITCODE in $script:WslDistro."
        }
    }
    catch {
        Write-Host ''
        Write-Host 'Rocket decompilation failed inside WSL.' -ForegroundColor Red
        if (Test-Path $DecompLog) {
            Write-Host 'Last 80 lines from the dedicated decomp log:' -ForegroundColor Yellow
            Get-Content $DecompLog -Tail 80 | ForEach-Object { Write-Host $_ }
        } else {
            Write-Host 'The dedicated decomp log was not created; the failure occurred before the Linux helper started.' -ForegroundColor Yellow
        }
        throw "Rocket NSUE ELF build failed. See $DecompLog for the exact Linux command and error."
    }

    $ElfPath = Join-Path $Root 'extern\rocket-decomp\build\us\NSUE.elf'
    if (-not (Test-Path $ElfPath)) { throw 'Rocket decomp finished without producing build/us/NSUE.elf.' }

    Banner '6/9 - N64Recomp and RSPRecomp tools'
    $N64Source = Join-Path $Root 'extern\n64-modern-runtime\N64Recomp'
    $N64Build = Join-Path $BuildRoot 'tools\n64recomp'
    $N64Log = Join-Path $LogRoot "n64recomp-tools-$Stamp.log"

    # N64RecompCLI is the executable target; N64Recomp itself is only the
    # core library target. FIXED12 uses native Windows CMake + Ninja inside the
    # imported VS2022 x64 environment, explicitly binding MSVC cl.exe. This
    # avoids all Visual Studio-generator discovery and MSYS/devkitPro shadowing.
    Remove-Item $N64Build -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "N64Recomp tool log: $N64Log"
    Write-Host "Using native CMake: $NativeCMake"
    Write-Host 'Configuring N64Recomp with native Ninja + MSVC (x64)...'
    $n64ConfigureExit = Invoke-NativeLogged $NativeCMake @('-S',$N64Source,'-B',$N64Build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$NativeNinja",'-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_C_COMPILER=$MsvcCl","-DCMAKE_CXX_COMPILER=$MsvcCl") $N64Log
    if ($n64ConfigureExit -ne 0) { throw "N64Recomp CMake configuration failed (exit $n64ConfigureExit). See $N64Log" }

    Write-Host 'Building N64RecompCLI + RSPRecomp (Release)...'
    $n64BuildExit = Invoke-NativeLogged $NativeCMake @('--build',$N64Build,'--target','N64RecompCLI','RSPRecomp','--parallel') $N64Log
    if ($n64BuildExit -ne 0) { throw "N64Recomp/RSPRecomp build failed (exit $n64BuildExit). See $N64Log" }
    $N64Exe = Find-BuiltFile $N64Build 'N64Recomp.exe'
    $RspExe = Find-BuiltFile $N64Build 'RSPRecomp.exe'
    Write-Host "N64Recomp executable: $N64Exe" -ForegroundColor Green
    Write-Host "RSPRecomp executable: $RspExe" -ForegroundColor Green

    # librecomp checks ROMs with XXH3_64. Calculate the exact value from the
    # user's verified canonical ROM instead of hard-coding an unverified hash.
    $HashBuild = Join-Path $BuildRoot 'tools\rom-hash'
    $HashLog = Join-Path $LogRoot "rom-hash-tool-$Stamp.log"
    Remove-Item $HashBuild -Recurse -Force -ErrorAction SilentlyContinue
    $hashConfigureExit = Invoke-NativeLogged $NativeCMake @('-S',(Join-Path $Root 'tools\rom_hash'),'-B',$HashBuild,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$NativeNinja",'-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_C_COMPILER=$MsvcCl","-DCMAKE_CXX_COMPILER=$MsvcCl","-DRUNTIME_DIR=$(Join-Path $Root 'extern\n64-modern-runtime')") $HashLog
    if ($hashConfigureExit -ne 0) { throw "ROM hash tool configuration failed (exit $hashConfigureExit). See $HashLog" }
    $hashBuildExit = Invoke-NativeLogged $NativeCMake @('--build',$HashBuild,'--parallel') $HashLog
    if ($hashBuildExit -ne 0) { throw "ROM hash tool build failed (exit $hashBuildExit). See $HashLog" }
    $HashExe = Find-BuiltFile $HashBuild 'RocketRomHash.exe'
    $RomHash = (& $HashExe $CanonicalRom).Trim()
    if ($LASTEXITCODE -ne 0 -or $RomHash -notmatch '^0x[0-9A-F]{16}ULL$') { throw "Unexpected XXH3 ROM hash output: $RomHash" }
    $GeneratedDir = Join-Path $Root 'generated'
    New-Item -ItemType Directory -Force -Path $GeneratedDir | Out-Null
    @"
#pragma once
#include <cstdint>
namespace rocket::generated {
inline constexpr std::uint64_t kRomXxh3 = $RomHash;
}
"@ | Set-Content -Encoding UTF8 (Join-Path $GeneratedDir 'rom_identity.generated.hpp')
    Write-Host "ROM XXH3: $RomHash"

    Banner '7/9 - Static CPU and RSP recompilation'
    $CpuOut = Join-Path $Root 'runtime-recomp\RecompiledFuncs'
    $RspOut = Join-Path $Root 'runtime-recomp\RecompiledRSP'
    Get-ChildItem $CpuOut -Force | Where-Object { $_.Name -ne '.gitkeep' } | Remove-Item -Recurse -Force
    Get-ChildItem $RspOut -Force | Where-Object { $_.Name -ne '.gitkeep' } | Remove-Item -Recurse -Force

    # Keep the cartridge's retail 0x80000400 address for N64ModernRuntime's
    # initial ROM placement, but compile/call game_init as the host entrypoint.
    # The raw retail stub deliberately BREAKs at 0x80000438 if game_init ever
    # returns; under the static runtime it does return after starting the idle
    # thread. This split mirrors DKR-R's proven bootstrap-wrapper architecture.
    $RocketBootstrap = Resolve-RocketBootstrap $ElfPath
    $RocketEntrypoint = $RocketBootstrap.GameInit
    $BootstrapHeader = Join-Path $GeneratedDir 'bootstrap.generated.hpp'
    @"
#pragma once
#include <cstdint>
namespace rocket::generated {
inline constexpr std::uint32_t kRetailLoadAddress = $($RocketBootstrap.Retail)U;
inline constexpr std::uint32_t kCallableEntrypoint = $($RocketBootstrap.GameInit)U;
inline constexpr std::uint32_t kInitialStackPointer = $($RocketBootstrap.InitialStack)U;
inline constexpr std::uint32_t kBootstrapBssStart = $($RocketBootstrap.BssStart)U;
inline constexpr std::uint32_t kBootstrapBssEnd = $($RocketBootstrap.BssEnd)U;
}
"@ | Set-Content -Encoding UTF8 $BootstrapHeader

    $RecompToml = Join-Path $BuildRoot 'generated\rocket.us.toml'
    $RecompLog = Join-Path $LogRoot "static-recomp-$Stamp.log"
    Write-Host "Static recompilation log: $RecompLog" -ForegroundColor DarkGreen
    Write-Host 'Rocket static-recomp policy: game_init callable entry + 24 verified zero-size assembly routines; retail 0x80000400 retained for ROM placement' -ForegroundColor DarkGreen
    Invoke-Python @((Join-Path $Root 'scripts\generate_recomp_config.py'),
        '--policy',(Join-Path $Root 'runtime-recomp\rocket.us.recomp-policy.json'),
        '--elf',$ElfPath,'--rom',$CanonicalRom,'--output-functions',$CpuOut,
        '--entrypoint',$RocketEntrypoint,'--output',$RecompToml,'--functions-per-output-file','50')

    # First ask N64Recomp to parse the ELF and dump its resolved context. This
    # proves the config and selected entrypoint are valid before code generation.
    $GeneratedWork = Join-Path $BuildRoot 'generated'
    Remove-Item (Join-Path $GeneratedWork 'dump.toml') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $GeneratedWork 'data_dump.toml') -Force -ErrorAction SilentlyContinue
    Push-Location $GeneratedWork
    try {
        Write-Host 'Validating Rocket ELF context with N64Recomp --dump-context...'
        $contextExit = Invoke-NativeLogged $N64Exe @($RecompToml,'--dump-context') $RecompLog
        if ($contextExit -ne 0) {
            throw "N64Recomp ELF/context validation failed (exit $contextExit). See $RecompLog"
        }
        if (Test-Path 'dump.toml') { Move-Item 'dump.toml' 'rocket.functions.dump.toml' -Force }
        if (Test-Path 'data_dump.toml') { Move-Item 'data_dump.toml' 'rocket.data.dump.toml' -Force }
    }
    finally { Pop-Location }

    Write-Host 'Generating Rocket CPU recompilation...'
    $recompExit = Invoke-NativeLogged $N64Exe @($RecompToml) $RecompLog
    if ($recompExit -ne 0) {
        throw "N64Recomp CPU generation failed (exit $recompExit). See $RecompLog for the exact function/instruction."
    }

    # Graphics v20: diagnostic-only instrumentation. This adds one observation
    # call at generated add_render_entry entry; it does not wrap or replace the renderer.
    Invoke-Python @((Join-Path $Root 'scripts\patch_popin_diagnostics_generated.py'),'--root',$Root)

    # Graphics v21: recover entries beyond Rocket's retail 256-slot list inside
    # the same func_8008B694 invocation. The normal renderer/tail remain authoritative.
    Invoke-Python @((Join-Path $Root 'scripts\patch_render_queue_expansion_v21_generated.py'),'--root',$Root)

    # Graphics v27: expand only the GfxTask command/matrix arena in Expansion Pak RAM.
    # Draw Distance, visibility and interpolation math are untouched.
    Invoke-Python @((Join-Path $Root 'scripts\patch_graphics_arena_v27_generated.py'),'--root',$Root)






    # Graphics v19: N64Recomp regenerates these files every build, so apply the
    # global-order expanded render-queue wrapper immediately after CPU generation.

    Push-Location (Join-Path $Root 'runtime-recomp\rsp')
    try {
        Write-Host 'Generating Rocket n_aspMain RSP recompilation...'
        $rspExit = Invoke-NativeLogged $RspExe @((Join-Path $Root 'runtime-recomp\rsp\n_aspMain.us.toml')) $RecompLog
        if ($rspExit -ne 0) {
            throw "RSPRecomp failed for Rocket n_aspMain (exit $rspExit). See $RecompLog for the exact indirect target/instruction."
        }
    } finally { Pop-Location }
    if (-not (Test-Path (Join-Path $CpuOut 'recomp_overlays.inl'))) { throw 'N64Recomp did not emit recomp_overlays.inl.' }
    if (-not (Get-ChildItem $RspOut -Filter '*.cpp' -File -ErrorAction SilentlyContinue)) { throw 'RSPRecomp did not emit a C++ microcode file.' }

    Banner '8/9 - Selected native platform builds'
    $Dist = Join-Path $Root 'dist'
    New-Item -ItemType Directory -Force -Path $Dist | Out-Null
    $BuiltArtifacts = New-Object System.Collections.Generic.List[string]
    $RocketExe = $null
    $RocketDir = $null

    Write-Host 'Rocket runtime policy: FIXED27 gameplay/interpolation baseline restored exactly (retail 30 Hz simulation + RT64 presentation interpolation + safe-area crop + widescreen CPU frustum), with platform-only build/packaging changes layered around it.' -ForegroundColor DarkGreen
    Write-Host 'Attachment/skybox interpolation v6: ENABLED (shared-parent + dynamic GFX identities).' -ForegroundColor Green
    Write-Host 'N64 colour dithering v7: LAUNCHER TOGGLE (retail Bayer / disabled).' -ForegroundColor Green
    Write-Host 'Interpolation + presentation fix v3.1: ENABLED (centroid-stable small geometry + double-buffered RT64 presentation targets).' -ForegroundColor Green

    if ($BuildWindows) {
        Write-Host ''
        Write-Host '[Windows x64]' -ForegroundColor Cyan
        $WindowsBuild = Join-Path $BuildRoot 'windows'
        $RuntimeLog = Join-Path $LogRoot "rocket-runtime-windows-$Stamp.log"
        Remove-Item $WindowsBuild -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Rocket runtime build log: $RuntimeLog"
        $runtimeConfigureExit = Invoke-NativeLogged $NativeCMake @('-S',$Root,'-B',$WindowsBuild,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$NativeNinja",'-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_C_COMPILER=$ClangCl","-DCMAKE_CXX_COMPILER=$ClangCl") $RuntimeLog
        if ($runtimeConfigureExit -ne 0) { throw "Rocket-R Windows CMake configuration failed (exit $runtimeConfigureExit). See $RuntimeLog" }
        $runtimeBuildExit = Invoke-NativeLogged $NativeCMake @('--build',$WindowsBuild,'--target','RocketR','--parallel') $RuntimeLog
        if ($runtimeBuildExit -ne 0) { throw "Rocket-R Windows build failed (exit $runtimeBuildExit). See $RuntimeLog" }
        $RocketExe = Find-BuiltFile $WindowsBuild 'Rocket-R.exe'
        $RocketDir = Split-Path $RocketExe -Parent
        Write-Host "Built: $RocketExe" -ForegroundColor Green
    }

    if ($BuildLinuxX64) {
        Write-Host ''
        Write-Host '[Linux x86_64 AppImage]' -ForegroundColor Cyan
        $LinuxX64Log = Join-Path $LogRoot "rocket-runtime-linux-x86_64-$Stamp.log"
        $wslLinuxArgs = @('-d',$script:WslDistro,'--exec','/bin/bash',"$WslRoot/Build-Linux.sh",'--arch','x86_64','--output-dir',"$WslRoot/dist")
        $linuxExit = Invoke-NativeLogged 'wsl.exe' $wslLinuxArgs $LinuxX64Log
        if ($linuxExit -ne 0) { throw "Rocket-R Linux x86_64/AppImage build failed (exit $linuxExit). See $LinuxX64Log" }
        $LinuxX64Artifact = Join-Path $Dist "Rocket-R-$Version-Linux-x86_64.AppImage"
        if (-not (Test-Path $LinuxX64Artifact)) { throw "Linux x86_64 build completed but $LinuxX64Artifact was not produced." }
        $BuiltArtifacts.Add($LinuxX64Artifact)
        $LinuxX64Portable = Join-Path $Dist "Rocket-R-$Version-Linux-x86_64-Portable.tar.gz"
        if (Test-Path $LinuxX64Portable) { $BuiltArtifacts.Add($LinuxX64Portable) }
        $LinuxX64SteamDeck = Join-Path $Dist "Rocket-R-$Version-Linux-x86_64-SteamDeck.tar.gz"
        if (Test-Path $LinuxX64SteamDeck) { $BuiltArtifacts.Add($LinuxX64SteamDeck) }
    }

    if ($BuildLinuxArm64) {
        Write-Host ''
        Write-Host '[Linux ARM64 / aarch64 AppImage]' -ForegroundColor Cyan
        $LinuxArmLog = Join-Path $LogRoot "rocket-runtime-linux-aarch64-$Stamp.log"
        $wslArmArgs = @('-d',$script:WslDistro,'--exec','/bin/bash',"$WslRoot/Build-Linux.sh",'--arch','aarch64','--output-dir',"$WslRoot/dist")
        $linuxArmExit = Invoke-NativeLogged 'wsl.exe' $wslArmArgs $LinuxArmLog
        if ($linuxArmExit -ne 0) { throw "Rocket-R Linux ARM64/AppImage build failed (exit $linuxArmExit). See $LinuxArmLog" }
        $LinuxArmArtifact = Join-Path $Dist "Rocket-R-$Version-Linux-aarch64.AppImage"
        if (-not (Test-Path $LinuxArmArtifact)) { throw "Linux ARM64 build completed but $LinuxArmArtifact was not produced." }
        $BuiltArtifacts.Add($LinuxArmArtifact)
        $LinuxArmPortable = Join-Path $Dist "Rocket-R-$Version-Linux-aarch64-Portable.tar.gz"
        if (Test-Path $LinuxArmPortable) { $BuiltArtifacts.Add($LinuxArmPortable) }
    }

    if ($BuildAndroidArm64) {
        Write-Host ''
        Write-Host '[Android ARM64 / arm64-v8a APK]' -ForegroundColor Cyan
        $AndroidLog = Join-Path $LogRoot "rocket-runtime-android-arm64-$Stamp.log"
        $PowerShellExe = (Get-Command powershell.exe -ErrorAction Stop).Source
        $androidExit = Invoke-NativeLogged $PowerShellExe @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $Root 'scripts\Build-Android.ps1'),'-ProjectRoot',$Root,'-OutputDirectory',$Dist) $AndroidLog
        if ($androidExit -ne 0) { throw "Rocket-R Android ARM64/APK build failed (exit $androidExit). See $AndroidLog" }
        $AndroidArtifact = Join-Path $Dist "Rocket-R-$Version-Android-arm64-v8a.apk"
        if (-not (Test-Path $AndroidArtifact)) { throw "Android ARM64 build completed but $AndroidArtifact was not produced." }
        $BuiltArtifacts.Add($AndroidArtifact)
    }

    Banner '9/9 - ROM-free release outputs'
    if ($BuildWindows) {
        if (-not $NoPackage) {
            $PackageName = "Rocket-R-$Version-Windows-x64"
            $Stage = Join-Path $BuildRoot "package\$PackageName"
            Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force -Path $Stage | Out-Null
            Copy-Item (Join-Path $RocketDir '*') $Stage -Recurse -Force
            Copy-Item (Join-Path $Root 'README.md') $Stage -Force
            Copy-Item (Join-Path $Root 'THIRD_PARTY.md') $Stage -Force
            Copy-Item (Join-Path $Root 'LICENSE.md') $Stage -Force
            Copy-Item (Join-Path $Root 'docs\STATUS.md') (Join-Path $Stage 'STATUS.md') -Force
            Invoke-Python @((Join-Path $Root 'scripts\scan_release.py'),$Stage)
            $ZipPath = Join-Path $Dist "$PackageName.zip"
            Remove-Item $ZipPath -Force -ErrorAction SilentlyContinue
            Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $ZipPath -CompressionLevel Optimal
            $BuiltArtifacts.Add($ZipPath)
            Write-Host "Package: $ZipPath" -ForegroundColor Green
        } else {
            $BuiltArtifacts.Add($RocketExe)
            Write-Host "NoPackage selected; Windows executable retained at: $RocketExe" -ForegroundColor Yellow
        }
    }

    Write-Host 'ROM-free release scan/package policy is applied per selected platform. Linux AppImages and the Android APK are the native distributable formats, so those selections are always packaged.' -ForegroundColor DarkGreen
    Write-Host ''
    Write-Host 'Selected build outputs:' -ForegroundColor Cyan
    foreach ($artifact in $BuiltArtifacts) { Write-Host "  $artifact" -ForegroundColor Green }
    Write-Host ''
    Write-Host 'BUILD COMPLETE.' -ForegroundColor Green
    Write-Host 'The runtime uses your private ROM only; packaged builds are scanned to reject ROMs.'
    if ($BuildWindows -and -not $NoLaunch -and (Ask-YesNo 'Launch the freshly built Windows runtime with your verified ROM now?' $false)) {
        & $RocketExe --rom $CanonicalRom
    }
    Stop-Transcript | Out-Null
    exit 0
}
catch {
    Write-Host ''
    Write-Host 'BUILD STOPPED SAFELY' -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host 'Nothing was uploaded and your original ROM was not modified.' -ForegroundColor Yellow
    Write-Host "Full log: $LogPath" -ForegroundColor Yellow
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}
