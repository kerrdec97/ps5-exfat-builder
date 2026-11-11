<#
.SYNOPSIS
  Release build of the native exFAT Image Builder on a Windows PC:
  dist\exFAT Image Builder.exe (ONE file to download and run) and
  dist\SHA256SUMS.txt, optionally the portable zip.

.DESCRIPTION
  1. Uses the MSVC x64 developer environment (imported through vswhere and
     VsDevCmd unless cl.exe is already on PATH) and Qt from -QtDir (default:
     the QT_ROOT_DIR or Qt6_DIR environment variable, else windeployqt on
     PATH).
  2. Configures and builds native/ (Release, Ninja) unless -SkipBuild.
  3. Runs CTest unless -SkipTests.
  4. Deploys the Qt runtime to dist\exFAT Image Builder\ (windeployqt, README,
     LICENCE, LICENSES, third-party notices, engines), as CI does, and checks
     the two bundled engines against their pinned SHA-256.
  5. Packs that folder into dist\exFAT Image Builder.exe (make-onefile.ps1).
     Extra payload files: -Extra '<source>=<path inside>' (one item with
     -File; several as 'a=b','c=d' when called from a PowerShell prompt), or
     copy them under dist\exFAT Image Builder\ and run make-onefile.ps1 again.
  6. Smoke test from a fresh folder with private data and cache folders:
     --self-test twice (first start unpacks, the second uses the cache).
  7. -Zip: the folder edition, a zip of the deploy folder that runs without
     unpacking itself (GitHub #78: offered next to the exe for PCs whose
     antivirus flags the self-unpacking exe). Then SHA256SUMS.txt, which
     names the exe as GitHub serves it (exFAT.Image.Builder.exe).

  Nothing is installed and the user's own settings and caches are not used.
  The Qt bin folder stays on PATH of the calling session; the other
  environment variables the script sets are put back.
  Windows PowerShell 5.1 compatible.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\make-release.ps1 -Zip
#>
[CmdletBinding()]
param(
    [string]$QtDir = '',
    [string]$BuildDir = '',
    [string]$OutDir = '',
    [string[]]$Extra = @(),
    # The bundled PS5 Backport engine (PS5BackportGUI-0.3.4.exe, hash-pinned in
    # native\cmake\BackportEngine.cmake). Empty: CMake downloads the official
    # release and checks its SHA-256.
    [string]$BackportEngine = '',
    # PRIVATE builds only: your own Sony Publishing Tools folder (the one with
    # prospero-pub-cmd.exe, or toolchain\/bin\ holding it). It is copied into
    # the exe as tools\fpkg-toolkit so FPKG works with nothing to set up, and
    # the output goes to dist-private\ unless -OutDir is given. Never publish a
    # build made with this: those files are Sony's, licensed to you.
    [string]$PrivateToolkit = '',
    # PRIVATE builds only: the folder holding your own fpkg-cli.exe (PSVIETHOA's
    # command-line front end of Drakmor's LibProsperoPkg engine), used by the DLC
    # page. Copied into the exe as tools\fpkg-cli. Never publish a build made
    # with this either.
    [string]$PrivateDlcEngine = '',
    # PRIVATE builds only: a folder with your own YouTube app .pkg files for the
    # Y2JB tab (PS5 connect). Copied into the exe as tools\youtube, where the tab
    # lists them by version and firmware. Never publish a build made with this:
    # those packages are Google's and Sony's software.
    [string]$PrivateYoutube = '',
    [int]$Parallel = 0,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$Zip
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$native = Join-Path $repo 'native'
if (-not $BuildDir) { $BuildDir = Join-Path $native 'build-release' }
if (-not $OutDir) { $OutDir = Join-Path $repo $(if ($PrivateToolkit -or $PrivateDlcEngine -or $PrivateYoutube) { 'dist-private' } else { 'dist' }) }
$version = (Select-String -Path (Join-Path $native 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$dist = Join-Path $OutDir 'exFAT Image Builder'
$onefile = Join-Path $OutDir 'exFAT Image Builder.exe'
if (-not $QtDir) {
    if ($env:QT_ROOT_DIR) { $QtDir = $env:QT_ROOT_DIR }
    elseif ($env:Qt6_DIR -and (Test-Path (Join-Path $env:Qt6_DIR '..\..\..\bin'))) { $QtDir = (Resolve-Path (Join-Path $env:Qt6_DIR '..\..\..')).Path }
}

function Write-Step([string]$text) { Write-Host "== $text" }

# Native commands write progress to stderr; Windows PowerShell would turn that
# into errors under 'Stop'.
function Invoke-Native([scriptblock]$command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $command 2>&1 | ForEach-Object { Write-Host "$_" } } finally { $ErrorActionPreference = $previous }
    return $LASTEXITCODE
}

# ---- 1. Tool chain -----------------------------------------------------------------
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'No Visual Studio / Build Tools (vswhere.exe missing). See pc-acceptance.ps1 for the install command.' }
    $vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $vsPath) { throw 'Visual Studio has no C++ x64 tools.' }
    $envDump = @(& cmd.exe /s /c "`"$(Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat')`" -no_logo -arch=amd64 -host_arch=amd64 2>nul && set")
    if ($envDump.Count -eq 0) { throw "VsDevCmd.bat failed under $vsPath (no developer environment)." }
    foreach ($line in $envDump) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1), 'Process') }
    }
}
$windeployqt = if ($QtDir) { Join-Path $QtDir 'bin\windeployqt.exe' } else { '' }
if ($windeployqt -and (Test-Path $windeployqt)) {
    $env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH
} else {
    $found = Get-Command windeployqt -ErrorAction SilentlyContinue
    if (-not $found) { throw 'Qt was not found: pass -QtDir <...\msvc2022_64>, set QT_ROOT_DIR, or put windeployqt on PATH.' }
    $windeployqt = $found.Source
    $QtDir = Split-Path (Split-Path $windeployqt -Parent) -Parent
}

# A running copy keeps its exe open: say so now, not after the whole build.
foreach ($inUse in $onefile, (Join-Path $dist 'exFAT Image Builder.exe')) {
    if (-not (Test-Path -LiteralPath $inUse)) { continue }
    try { ([IO.File]::Open($inUse, 'Open', 'ReadWrite', 'None')).Close() }
    catch { throw "Close the running app first: $inUse is in use." }
}

# ---- 2./3. Build and test -------------------------------------------------------
if (-not $SkipBuild) {
    Write-Step 'Configure'
    $engineArg = if ($BackportEngine) { "-DFW_BACKPORT_ENGINE_EXE=$BackportEngine" } else { '-DFW_FETCH_BACKPORT_ENGINE=ON' }
    if ((Invoke-Native { cmake -S $native -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtDir" -DFW_WARNINGS_AS_ERRORS=OFF $engineArg }) -ne 0) { throw 'Configure failed' }
    Write-Step 'Build'
    # Always an array: a one-element "if" result would be unrolled to a string,
    # and splatting a string passes it character by character.
    $jobs = @('--parallel')
    if ($Parallel -gt 0) { $jobs += "$Parallel" }
    if ((Invoke-Native { cmake --build $BuildDir @jobs }) -ne 0) { throw 'Build failed' }
}
if (-not $SkipTests) {
    Write-Step 'CTest'
    $savedTestEnv = @{ QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM; FW_OFFLINE = $env:FW_OFFLINE }
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:FW_OFFLINE = '1'
    try {
        $code = Invoke-Native { ctest --test-dir $BuildDir --timeout 900 --output-on-failure }
    } finally {
        foreach ($k in $savedTestEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $savedTestEnv[$k], 'Process') }
    }
    if ($code -ne 0) { throw "CTest failed ($code)" }
}

# ---- 4. Deploy -----------------------------------------------------------------------
Write-Step 'Deploy'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $BuildDir 'bin\exFAT Image Builder.exe') $dist
$deployed = Join-Path $dist 'exFAT Image Builder.exe'
if ((Invoke-Native { & $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw --compiler-runtime $deployed }) -ne 0) { throw 'windeployqt failed' }
Copy-Item (Join-Path $native 'packaging\README-portable.txt') (Join-Path $dist 'README.txt')
Copy-Item (Join-Path $repo 'LICENSES') -Recurse (Join-Path $dist 'LICENSES')
Copy-Item (Join-Path $native 'packaging\licenses\*') (Join-Path $dist 'LICENSES\')
Copy-Item (Join-Path $native 'packaging\THIRD_PARTY_NOTICES-native.md') (Join-Path $dist 'THIRD_PARTY_NOTICES.md')
Copy-Item (Join-Path $native 'packaging\LICENCE.txt') (Join-Path $dist 'LICENCE.txt')
# The Backport tab's engine (built into bin\engines by CMake). A release
# without it would show "engine missing", so it is required.
$engines = Join-Path $BuildDir 'bin\engines'
if (-not (Test-Path (Join-Path $engines 'backport\PS5BackportGUI.exe'))) {
    throw "The PS5 Backport engine is missing from $engines (configure with -BackportEngine <exe> or let CMake fetch it)"
}
# A build tree keeps what an earlier configure put there, so both bundled
# files are compared with the hashes pinned in native\cmake.
function Get-PinnedHash([string]$cmakeFile, [string]$variable) {
    $m = Select-String -Path (Join-Path $native "cmake\$cmakeFile") -Pattern ('set\(' + $variable + ' "([0-9a-f]{64})"\)') | Select-Object -First 1
    if (-not $m) { throw "$variable was not found in native\cmake\$cmakeFile" }
    return $m.Matches[0].Groups[1].Value
}
$pinned = @(
    @{ File = 'backport\PS5BackportGUI.exe'; Hash = (Get-PinnedHash 'BackportEngine.cmake' 'FW_BACKPORT_ENGINE_SHA256') },
    @{ File = 'ampr-emu-ng\libSceAmpr.sprx'; Hash = (Get-PinnedHash 'AmprRuntime.cmake' 'FW_AMPR_RUNTIME_SHA256') })
foreach ($pin in $pinned) {
    $bundled = Join-Path $engines $pin.File
    if (-not (Test-Path -LiteralPath $bundled)) { throw "Missing from the build: $bundled. Configure and build again (without -SkipBuild)." }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $bundled).Hash.ToLower()
    if ($actual -ne $pin.Hash) { throw "$bundled is not the pinned release (SHA-256 $actual, pinned $($pin.Hash)). Configure and build again (without -SkipBuild)." }
}
Copy-Item $engines -Recurse (Join-Path $dist 'engines')
if ($PrivateToolkit) {
    $tk = (Resolve-Path $PrivateToolkit).Path
    $exeIn = @('prospero-pub-cmd.exe', 'toolchain\prospero-pub-cmd.exe', 'bin\prospero-pub-cmd.exe') |
        ForEach-Object { Join-Path $tk $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $exeIn) { throw "prospero-pub-cmd.exe was not found in $tk (or its toolchain\ or bin\ folder)" }
    $tkOut = Join-Path $dist 'tools\fpkg-toolkit'
    Write-Step "Private build: bundling the toolkit from $tk"
    New-Item -ItemType Directory -Force $tkOut | Out-Null
    Copy-Item (Join-Path $tk '*') $tkOut -Recurse -Force
    # The tool loads the VC++ runtime from its own folder or System32; put the
    # app's copies next to it so it also runs on a PC without the redistributable.
    $toolDir = Split-Path (Join-Path $tkOut ($exeIn.Substring($tk.Length).TrimStart('\'))) -Parent
    $crt = @()
    if ($env:VCToolsRedistDir) { $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if ($crt) {
        # Every folder of the toolkit that holds a program (toolchain\ and its
        # ext\ helpers such as p2d.exe and da.exe): Windows looks for the DLLs
        # next to each program, not next to the one that started it.
        $dirs = @($toolDir) + @(Get-ChildItem -Path $tkOut -Recurse -Include '*.exe', '*.dll' -File |
            Select-Object -ExpandProperty DirectoryName -Unique)
        foreach ($d in ($dirs | Select-Object -Unique)) {
            Get-ChildItem -Path $crt.FullName -Filter '*.dll' | ForEach-Object {
                $dest = Join-Path $d $_.Name
                if (-not (Test-Path $dest)) { Copy-Item $_.FullName $dest }
            }
        }
    } else { Write-Host 'Note: the VC++ runtime folder was not found; the tool will use the PC''s installed runtime.' }
}
if ($PrivateDlcEngine) {
    $de = (Resolve-Path $PrivateDlcEngine).Path
    if (Test-Path $de -PathType Leaf) { $de = Split-Path $de -Parent }
    $cliIn = Join-Path $de 'fpkg-cli.exe'
    if (-not (Test-Path $cliIn)) { throw "fpkg-cli.exe was not found in $de" }
    $deOut = Join-Path $dist 'tools\fpkg-cli'
    Write-Step "Private build: bundling the DLC engine from $de"
    New-Item -ItemType Directory -Force $deOut | Out-Null
    Copy-Item $cliIn $deOut
    # Its native Oodle encoder (optional: it has a built-in one).
    $oodle = Join-Path $de 'libScePubTools.dll'
    if (Test-Path $oodle) { Copy-Item $oodle $deOut }
}
if ($PrivateYoutube) {
    $yt = (Resolve-Path $PrivateYoutube).Path
    $pkgs = @(Get-ChildItem -LiteralPath $yt -Filter '*.pkg' -File)
    if ($pkgs.Count -eq 0) { throw "No .pkg files were found in $yt" }
    $ytOut = Join-Path $dist 'tools\youtube'
    Write-Step "Private build: bundling $($pkgs.Count) YouTube package(s) from $yt"
    New-Item -ItemType Directory -Force $ytOut | Out-Null
    foreach ($p in $pkgs) { Copy-Item -LiteralPath $p.FullName $ytOut }
}

# ---- 5. Single-file exe -------------------------------------------------------------
Write-Step 'Single-file exe'
& (Join-Path $PSScriptRoot 'make-onefile.ps1') -DeployDir $dist -Output $onefile -BuildDir $BuildDir -Version $version -Extra $Extra

# ---- 6. Smoke test (fresh folder, private data and cache) -----------------------------
Write-Step 'Smoke test'
$smoke = Join-Path ([IO.Path]::GetTempPath()) ('fw-release-smoke-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
New-Item -ItemType Directory -Force $smoke | Out-Null
$saved = @{ LOCALAPPDATA = $env:LOCALAPPDATA; FW_DATA_DIR = $env:FW_DATA_DIR; FW_OFFLINE = $env:FW_OFFLINE; QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM }
try {
    Copy-Item $onefile $smoke
    $env:LOCALAPPDATA = Join-Path $smoke 'LocalAppData'
    $env:FW_DATA_DIR = Join-Path $smoke 'data'
    $env:FW_OFFLINE = '1'
    Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue # the release ships only the windows platform plugin
    New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null
    foreach ($round in 'first start', 'cached start') {
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = Join-Path $smoke 'exFAT Image Builder.exe'
        $psi.Arguments = '--self-test'
        $psi.WorkingDirectory = $smoke
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = [Diagnostics.Process]::Start($psi)
        $reading = $p.StandardOutput.ReadToEndAsync()
        if (-not $p.WaitForExit(120000)) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue # the launcher's job ends the program too
            throw "Self-test ($round) did not finish within 120 s"
        }
        $sw.Stop()
        $report = $reading.Result | ConvertFrom-Json
        if ($p.ExitCode -ne 0 -or $report.version -ne $version) { throw "Self-test ($round) failed: exit $($p.ExitCode)" }
        Write-Host ("   {0}: {1} ms (--self-test included), {2} dependencies" -f $round, $sw.ElapsedMilliseconds, @($report.dependencies).Count)
    }
} finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    Remove-Item -Recurse -Force $smoke -ErrorAction SilentlyContinue
}

# ---- 7. Zip and checksums -------------------------------------------------------------
$assets = @($onefile)
if ($Zip) {
    Write-Step 'Folder edition (zip)'
    $zipPath = Join-Path $OutDir "exFAT-Image-Builder-$version-win64-folder.zip"
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    Compress-Archive -Path $dist -DestinationPath $zipPath
    $assets += $zipPath
}
# GitHub serves an uploaded "exFAT Image Builder.exe" as "exFAT.Image.Builder.exe":
# the list names the file a user will have.
$sums = foreach ($f in $assets) { (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLower() + '  ' + ((Split-Path $f -Leaf) -replace ' ', '.') }
[IO.File]::WriteAllText((Join-Path $OutDir 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Step 'Done'
foreach ($f in $assets + (Join-Path $OutDir 'SHA256SUMS.txt')) { Write-Host ("   {0}  ({1:N1} MiB)" -f $f, ((Get-Item -LiteralPath $f).Length / 1MB)) }
