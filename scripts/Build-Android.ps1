[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $ProjectRoot 'dist' }
$Version = (Get-Content (Join-Path $ProjectRoot 'VERSION') -Raw).Trim()
$BuildRoot = Join-Path $ProjectRoot 'build'
$AndroidProject = Join-Path $BuildRoot 'android-project'
$ToolRoot = Join-Path $BuildRoot 'tools'
function Assert-ChildPath([string]$Path, [string]$Parent) {
    $resolvedBuild = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $resolvedTarget = [IO.Path]::GetFullPath($Path)
    if (-not $resolvedTarget.StartsWith($resolvedBuild, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside ${Parent}: $resolvedTarget"
    }
}
function Assert-BuildPath([string]$Path) { Assert-ChildPath $Path $BuildRoot }
Write-Host 'Android build helper: V48 checked patch pipeline' -ForegroundColor DarkGray
$SdkRoot = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Android\Sdk' } else { Join-Path $ToolRoot 'android-sdk' }

function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machine;$user;$env:Path"
}

function Require-Winget {
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
        throw 'Android build needs Java 17. winget/App Installer was not found, so install a JDK 17 manually and rerun.'
    }
}

function Invoke-NativeVisible {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [string[]]$Arguments = @()
    )

    # Windows PowerShell 5.1 can promote native stderr to NativeCommandError
    # records. Android/Java tools legitimately use stderr for normal output,
    # so keep those lines visible and use the process exit code as truth.
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 2>&1 |
            ForEach-Object { Write-Host ([string]$_) }
        return [int]$LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}


function Invoke-GradleStable {
    param(
        [Parameter(Mandatory = $true)][string]$GradlePath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$VersionText,
        [Parameter(Mandatory = $true)][string]$BuildRootPath
    )

    # Run Gradle from a tiny batch file, but launch that batch by *relative name*
    # from AndroidProject. v14-v16 wrapped the absolute runner path in nested
    # quotes for cmd.exe; on Windows PowerShell 5.1 that left a stray quote and
    # Gradle received no task arguments, so it silently ran the default `help`
    # task. Keep the runner filename simple and pass the version through the
    # standard ORG_GRADLE_PROJECT_ environment convention instead of -P quoting.
    $logRoot = Join-Path $BuildRootPath 'logs'
    New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $stdoutPath = Join-Path $logRoot ("gradle-android-" + $stamp + "-stdout.log")
    $stderrPath = Join-Path $logRoot ("gradle-android-" + $stamp + "-stderr.log")
    $runnerName = 'rocket-gradle-runner.cmd'
    $runnerPath = Join-Path $WorkingDirectory $runnerName

    $runner = @(
        '@echo off',
        'setlocal',
        'set "CMAKE_BUILD_PARALLEL_LEVEL=4"',
        ('set "ORG_GRADLE_PROJECT_ROCKET_VERSION=' + $VersionText + '"'),
        'echo [ROCKET-GRADLE] task=:app:assembleRelease version=%ORG_GRADLE_PROJECT_ROCKET_VERSION%',
        ('call "' + $GradlePath + '" --no-daemon --no-watch-fs --stacktrace --max-workers=4 :app:assembleRelease'),
        'set "ROCKET_GRADLE_EXIT=%ERRORLEVEL%"',
        'echo [ROCKET-GRADLE] exit=%ROCKET_GRADLE_EXIT%',
        'exit /b %ROCKET_GRADLE_EXIT%'
    ) -join "`r`n"
    [IO.File]::WriteAllText($runnerPath, $runner + "`r`n", (New-Object System.Text.ASCIIEncoding))

    Write-Host "Gradle isolated stdout: $stdoutPath" -ForegroundColor DarkGray
    Write-Host "Gradle isolated stderr: $stderrPath" -ForegroundColor DarkGray
    Write-Host 'Gradle task: :app:assembleRelease' -ForegroundColor DarkGray

    try {
        # runnerName is deliberately relative to WorkingDirectory. This avoids
        # cmd.exe's nested-quote edge case that produced: '"' is not recognized.
        $process = Start-Process -FilePath $env:ComSpec `
            -ArgumentList @('/d','/c',$runnerName) `
            -WorkingDirectory $WorkingDirectory `
            -WindowStyle Hidden -Wait -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath

        if (Test-Path -LiteralPath $stdoutPath) {
            Get-Content -LiteralPath $stdoutPath | ForEach-Object { Write-Host ([string]$_) }
        }
        if (Test-Path -LiteralPath $stderrPath) {
            Get-Content -LiteralPath $stderrPath | ForEach-Object { Write-Host ([string]$_) }
        }
        return [int]$process.ExitCode
    }
    finally {
        Remove-Item -LiteralPath $runnerPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-JavaVersionText {
    param([Parameter(Mandatory = $true)][string]$JavaExecutable)

    # `java -version` writes its successful version banner to STDERR by design.
    # Capturing it with PowerShell's 2>&1 under ErrorActionPreference=Stop
    # creates a terminating NativeCommandError on Windows PowerShell 5.1.
    # System.Diagnostics captures both streams without manufacturing a
    # PowerShell error record.
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $JavaExecutable
    $startInfo.Arguments = '-version'
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { return '' }
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { return '' }
        return ($stdout + "`n" + $stderr)
    }
    catch {
        return ''
    }
    finally {
        $process.Dispose()
    }
}

function Find-JavaHome {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:JAVA_HOME) { $candidates.Add($env:JAVA_HOME) }
    $javaOnPath = Get-Command java.exe -ErrorAction SilentlyContinue
    if ($javaOnPath -and $javaOnPath.Source) {
        $pathJavaHome = Split-Path (Split-Path $javaOnPath.Source -Parent) -Parent
        if ($pathJavaHome) { $candidates.Add($pathJavaHome) }
    }
    foreach ($base in @('C:\Program Files\Microsoft', 'C:\Program Files\Eclipse Adoptium', 'C:\Program Files\Java')) {
        if (Test-Path $base) {
            Get-ChildItem $base -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | ForEach-Object { $candidates.Add($_.FullName) }
        }
    }
    foreach ($candidateHome in $candidates) {
        $java = Join-Path $candidateHome 'bin\java.exe'
        if (-not (Test-Path $java)) { continue }
        $out = Get-JavaVersionText $java
        if ($out -match 'version "(\d+)') {
            $major = [int]$Matches[1]
            if ($major -ge 17 -and $major -lt 22) { return $candidateHome }
        }
    }
    return $null
}

$JavaHome = Find-JavaHome
if (-not $JavaHome) {
    Require-Winget
    Write-Host 'Java 17 is missing. Installing Microsoft OpenJDK 17...' -ForegroundColor Yellow
    $wingetExit = Invoke-NativeVisible 'winget.exe' @(
        'install', '--id', 'Microsoft.OpenJDK.17', '--exact',
        '--accept-package-agreements', '--accept-source-agreements', '--silent'
    )
    if ($wingetExit -ne 0) { throw "Could not install Microsoft OpenJDK 17 (exit $wingetExit)." }
    Refresh-Path
    $JavaHome = Find-JavaHome
}
if (-not $JavaHome) { throw 'Java 17 was not found after installation.' }
$env:JAVA_HOME = $JavaHome
$env:Path = "$(Join-Path $JavaHome 'bin');$env:Path"
Write-Host "Android Java: $JavaHome" -ForegroundColor DarkGreen

New-Item -ItemType Directory -Force -Path $ToolRoot, $SdkRoot | Out-Null
$SdkManager = Join-Path $SdkRoot 'cmdline-tools\latest\bin\sdkmanager.bat'
if (-not (Test-Path $SdkManager)) {
    $Zip = Join-Path $ToolRoot 'android-commandlinetools-11076708.zip'
    if (-not (Test-Path $Zip)) {
        Write-Host 'Downloading pinned Android command-line tools...' -ForegroundColor Cyan
        Invoke-WebRequest -UseBasicParsing -Uri 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip' -OutFile $Zip
    }
    $Extract = Join-Path $ToolRoot 'android-commandlinetools-extract'
    Assert-BuildPath $Extract
    Remove-Item -LiteralPath $Extract -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $Zip -DestinationPath $Extract -Force
    $Latest = Join-Path $SdkRoot 'cmdline-tools\latest'
    Assert-ChildPath $Latest (Join-Path $SdkRoot 'cmdline-tools')
    Remove-Item -LiteralPath $Latest -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Split-Path $Latest -Parent) | Out-Null
    Assert-BuildPath (Join-Path $Extract 'cmdline-tools')
    Move-Item -LiteralPath (Join-Path $Extract 'cmdline-tools') -Destination $Latest
    Remove-Item -LiteralPath $Extract -Recurse -Force
}
if (-not (Test-Path $SdkManager)) { throw "sdkmanager was not installed at $SdkManager" }

$env:ANDROID_SDK_ROOT = $SdkRoot
$env:ANDROID_HOME = $SdkRoot
$accept = (1..150 | ForEach-Object { 'y' }) -join "`n"
$oldPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $accept | & $SdkManager --sdk_root=$SdkRoot --licenses *> $null
    $licenseExit = [int]$LASTEXITCODE
}
finally {
    $ErrorActionPreference = $oldPreference
}
if ($licenseExit -ne 0) { throw "Android SDK license acceptance failed (exit $licenseExit)." }

$packages = @('platform-tools','platforms;android-34','build-tools;34.0.0','ndk;26.1.10909125','cmake;3.22.1')
Write-Host 'Ensuring pinned Android SDK/NDK/CMake packages...' -ForegroundColor Cyan
$sdkExit = Invoke-NativeVisible $SdkManager (@("--sdk_root=$SdkRoot") + $packages)
if ($sdkExit -ne 0) { throw "Android SDK package installation failed (exit $sdkExit)." }

$GradleRoot = Join-Path $ToolRoot 'gradle-8.7'
$Gradle = Join-Path $GradleRoot 'bin\gradle.bat'
if (-not (Test-Path $Gradle)) {
    $GradleZip = Join-Path $ToolRoot 'gradle-8.7-bin.zip'
    if (-not (Test-Path $GradleZip)) {
        Write-Host 'Downloading pinned Gradle 8.7...' -ForegroundColor Cyan
        Invoke-WebRequest -UseBasicParsing -Uri 'https://services.gradle.org/distributions/gradle-8.7-bin.zip' -OutFile $GradleZip
    }
    $GradleExtract = Join-Path $ToolRoot 'gradle-extract'
    Assert-BuildPath $GradleExtract
    Assert-BuildPath $GradleRoot
    Remove-Item $GradleExtract -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $GradleZip -DestinationPath $GradleExtract -Force
    Remove-Item $GradleRoot -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item (Join-Path $GradleExtract 'gradle-8.7') $GradleRoot
    Remove-Item $GradleExtract -Recurse -Force
}

$SdlJava = Join-Path $ProjectRoot 'extern\sdl2\android-project\app\src\main\java\org\libsdl\app'
if (-not (Test-Path $SdlJava)) { throw 'Pinned SDL2 Android Java sources are missing. Run dependency bootstrap first.' }
Assert-BuildPath $AndroidProject
Remove-Item -LiteralPath $AndroidProject -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $AndroidProject 'app\src\main\java\org\libsdl\app') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $AndroidProject 'app\src\main\java\com\rocketret\rocketr') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $AndroidProject 'app\src\main\res\values') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $AndroidProject 'app\jni') | Out-Null
Copy-Item (Join-Path $ProjectRoot 'packaging\android\build.gradle') (Join-Path $AndroidProject 'build.gradle') -Force
Copy-Item (Join-Path $ProjectRoot 'packaging\android\settings.gradle') (Join-Path $AndroidProject 'settings.gradle') -Force
Copy-Item (Join-Path $ProjectRoot 'packaging\android\app-build.gradle') (Join-Path $AndroidProject 'app\build.gradle') -Force
Copy-Item (Join-Path $ProjectRoot 'packaging\android\app\proguard-rules.pro') (Join-Path $AndroidProject 'app\proguard-rules.pro') -Force
Copy-Item (Join-Path $ProjectRoot 'packaging\android\app\jni\CMakeLists.txt') (Join-Path $AndroidProject 'app\jni\CMakeLists.txt') -Force
# All dependency compatibility changes must be present before building.
# Never transform or restore files in extern/ from a build helper.
$patchCheck = Invoke-NativeVisible 'python' @((Join-Path $ProjectRoot 'scripts\bootstrap_dependencies.py'), '--root', $ProjectRoot)
if ($patchCheck -ne 0) { throw 'The checked dependency patch pipeline failed.' }

# Repository-owned Android renderer handling is installed permanently by repair v15.
# Build-Android only verifies it; it never rewrites src\rt64_renderer.cpp.
$RocketRendererPath = Join-Path $ProjectRoot 'src\rt64_renderer.cpp'
if (-not (Test-Path -LiteralPath $RocketRendererPath)) {
    throw "Rocket-R RT64 renderer source is missing: $RocketRendererPath"
}
$RocketRendererText = [IO.File]::ReadAllText($RocketRendererPath)
if ($RocketRendererText -notmatch '(?m)^#elif defined\(__linux__\)[ \t]*&&[ \t]*!defined\(__ANDROID__\)[ \t]*\r?$' -or
    $RocketRendererText -notmatch '(?m)^#elif defined\(__ANDROID__\)[ \t]*\r?$' -or
    $RocketRendererText -notmatch 'android_native_window\(\)' -or
    $RocketRendererText -notmatch 'app_config\.detectDataPath[ \t]*=[ \t]*false;') {
    throw 'Rocket-R Android renderer/data-path guards are missing. Restore the checked source and rerun the build.'
}
Write-Host 'Android Rocket renderer compatibility: permanent ANativeWindow/data-path guards verified.' -ForegroundColor DarkGray

# The Android direct-ROM handoff is installed permanently by repair v15.
# Do not mutate src\main.cpp during builds: verify the guard and continue.
$MainCppPath = Join-Path $ProjectRoot 'src\main.cpp'
if (-not (Test-Path -LiteralPath $MainCppPath)) {
    throw "Rocket-R main source is missing: $MainCppPath"
}
$MainCppText = [IO.File]::ReadAllText($MainCppPath)
if ($MainCppText -notmatch 'bypassing desktop launcher and starting Rocket directly' -or
    $MainCppText -notmatch 'rocket::select_rom\(options\.rom' -or
    $MainCppText -notmatch '(?s)#if defined\(__ANDROID__\).*?#else.*?rocket::ui::run_launcher') {
    throw 'Rocket-R Android direct-ROM launcher guard is missing. Restore the checked source and rerun the build.'
}
Write-Host 'Android launcher compatibility: permanent direct-ROM handoff verified; no build-time main.cpp rewrite required.' -ForegroundColor DarkGray

Copy-Item (Join-Path $ProjectRoot 'packaging\android\app\src\main\AndroidManifest.xml') (Join-Path $AndroidProject 'app\src\main\AndroidManifest.xml') -Force
# Copy the complete resource tree, including the icon referenced by the manifest.
Copy-Item (Join-Path $ProjectRoot 'packaging\android\app\src\main\res\*') (Join-Path $AndroidProject 'app\src\main\res') -Recurse -Force
Copy-Item (Join-Path $ProjectRoot 'packaging\android\app\src\main\assets') (Join-Path $AndroidProject 'app\src\main') -Recurse -Force
$docsExit = Invoke-NativeVisible 'python' @((Join-Path $ProjectRoot 'scripts\stage_release_docs.py'), '--root', $ProjectRoot, '--output', (Join-Path $AndroidProject 'app\src\main\assets\rocket-r-docs'))
if ($docsExit -ne 0) { throw 'Could not stage Android release documentation.' }
Copy-Item (Join-Path $ProjectRoot 'packaging\android\app\src\main\java\com\rocketret\rocketr\*.java') (Join-Path $AndroidProject 'app\src\main\java\com\rocketret\rocketr') -Force
Copy-Item (Join-Path $SdlJava '*.java') (Join-Path $AndroidProject 'app\src\main\java\org\libsdl\app') -Force
"sdk.dir=$($SdkRoot.Replace('\','/').Replace(':','\:'))" | Set-Content -Encoding ASCII (Join-Path $AndroidProject 'local.properties')

$gradleProperties = @(
    'org.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8',
    'org.gradle.daemon=false',
    'org.gradle.vfs.watch=false',
    'org.gradle.workers.max=4'
) -join "`r`n"
[IO.File]::WriteAllText(
    (Join-Path $AndroidProject 'gradle.properties'),
    $gradleProperties,
    (New-Object System.Text.ASCIIEncoding))



Write-Host 'Building Rocket-R Android ARM64 release...' -ForegroundColor Cyan
Push-Location $AndroidProject
try {
    $gradleExit = Invoke-GradleStable -GradlePath $Gradle -WorkingDirectory $AndroidProject -VersionText $Version -BuildRootPath $BuildRoot
    if ($gradleExit -ne 0) { throw "Gradle Android build failed (exit $gradleExit). See the gradle-android stdout/stderr logs under build\logs." }
}
finally {
    Pop-Location

}

$Unsigned = Join-Path $AndroidProject 'app\build\outputs\apk\release\app-release-unsigned.apk'
if (-not (Test-Path $Unsigned)) { throw "Gradle did not produce $Unsigned" }
$BuildTools = Join-Path $SdkRoot 'build-tools\34.0.0'
$ZipAlign = Join-Path $BuildTools 'zipalign.exe'
$ApkSigner = Join-Path $BuildTools 'apksigner.bat'
$KeyTool = Join-Path $JavaHome 'bin\keytool.exe'
$SigningDir = Join-Path $BuildRoot 'private\android-signing'
$Keystore = Join-Path $SigningDir 'rocket-r-local-release.jks'
New-Item -ItemType Directory -Force -Path $SigningDir, $OutputDirectory | Out-Null
if (-not (Test-Path $Keystore)) {
    $keyExit = Invoke-NativeVisible $KeyTool @(
        '-genkeypair','-noprompt','-keystore',$Keystore,'-alias','rocket-r',
        '-keyalg','RSA','-keysize','2048','-validity','10000',
        '-storepass','rocket-r-local','-keypass','rocket-r-local',
        '-dname','CN=Rocket-R Local Build,O=Rocket-R,C=GB'
    )
    if ($keyExit -ne 0) { throw "Could not create the local Android signing key (exit $keyExit)." }
}
$Aligned = Join-Path $BuildRoot 'android-project\app-release-aligned.apk'
$Final = Join-Path $OutputDirectory "Rocket-R-$Version-Android-arm64-v8a.apk"
Remove-Item $Aligned, $Final -Force -ErrorAction SilentlyContinue
$zipExit = Invoke-NativeVisible $ZipAlign @('-f','4',$Unsigned,$Aligned)
if ($zipExit -ne 0) { throw "zipalign failed for Android APK (exit $zipExit)." }
$signExit = Invoke-NativeVisible $ApkSigner @(
    'sign','--ks',$Keystore,'--ks-key-alias','rocket-r',
    '--ks-pass','pass:rocket-r-local','--key-pass','pass:rocket-r-local',
    '--out',$Final,$Aligned
)
if ($signExit -ne 0) { throw "apksigner failed for Android APK (exit $signExit)." }
$verifyExit = Invoke-NativeVisible $ApkSigner @('verify','--verbose',$Final)
if ($verifyExit -ne 0) { throw "Android APK signature verification failed (exit $verifyExit)." }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($Final)
try {
    $entries = @($archive.Entries | ForEach-Object { $_.FullName })
    if (-not ($entries -contains 'lib/arm64-v8a/libmain.so')) { throw 'APK is missing ARM64 libmain.so.' }
    if (-not ($entries -contains 'lib/arm64-v8a/libSDL2.so')) { throw 'APK is missing ARM64 libSDL2.so.' }
    if ($entries | Where-Object { $_ -match '^lib/(x86|x86_64|armeabi-v7a)/' }) { throw 'APK unexpectedly contains a non-ARM64 native ABI.' }
}
finally { $archive.Dispose() }

$ScanDir = Join-Path $BuildRoot 'android-apk-scan'
Assert-BuildPath $ScanDir
Remove-Item -LiteralPath $ScanDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $ScanDir | Out-Null
[IO.Compression.ZipFile]::ExtractToDirectory($Final, $ScanDir)
$Python = Get-Command python.exe -ErrorAction SilentlyContinue
if ($Python) {
    $scanExit = Invoke-NativeVisible $Python.Source @((Join-Path $ProjectRoot 'scripts\scan_release.py'),$ScanDir)
    if ($scanExit -ne 0) { throw "ROM-free APK release scan failed (exit $scanExit)." }
}
Remove-Item -LiteralPath $ScanDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Android ARM64 APK: $Final" -ForegroundColor Green
