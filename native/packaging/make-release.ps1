<#
.SYNOPSIS
  Release build of the native exFAT Image Builder on a Windows PC:
  dist\exFAT Image Builder.exe (ONE file to download and run) and
  dist\SHA256SUMS.txt, optionally the portable zip.

.DESCRIPTION
  1. Uses the MSVC x64 developer environment (imported through vswhere and
     VsDevCmd unless cl.exe is already on PATH) and Qt from -QtDir.
  2. Configures and builds native/ (Release, Ninja) unless -SkipBuild.
  3. Runs CTest unless -SkipTests.
  4. Deploys the Qt runtime to dist\exFAT Image Builder\ (windeployqt, README,
     LICENSES, third-party notices), as CI does.
  5. Packs that folder into dist\exFAT Image Builder.exe (make-onefile.ps1).
     Extra payload files: -Extra '<source>=<path inside>' (repeatable), or
     copy them under dist\exFAT Image Builder\ and run make-onefile.ps1 again.
  6. Smoke test from a fresh folder with private data and cache folders:
     --self-test twice (first start unpacks, the second uses the cache).
  7. -Zip: the portable zip of the deploy folder. Then SHA256SUMS.txt.

  Nothing is installed and the user's own settings and caches are not used.
  Windows PowerShell 5.1 compatible.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\make-release.ps1 -Zip
#>
[CmdletBinding()]
param(
    [string]$QtDir = 'D:\LAPTOP_C_COPY_20261029\Qt\6.10.3\msvc2022_64',
    [string]$BuildDir = '',
    [string]$OutDir = '',
    [string[]]$Extra = @(),
    # The bundled PS5 Backport engine (PS5BackportGUI-0.3.4.exe, hash-pinned in
    # native\cmake\BackportEngine.cmake). Empty: CMake downloads the official
    # release and checks its SHA-256.
    [string]$BackportEngine = '',
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
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }
$version = (Select-String -Path (Join-Path $native 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value

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
    $envDump = & cmd.exe /s /c "`"$(Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat')`" -no_logo -arch=amd64 -host_arch=amd64 2>nul && set"
    foreach ($line in $envDump) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1), 'Process') }
    }
}
$windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
if (Test-Path $windeployqt) {
    $env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH
} else {
    $found = Get-Command windeployqt -ErrorAction SilentlyContinue
    if (-not $found) { throw "Qt is not at $QtDir and windeployqt is not on PATH" }
    $windeployqt = $found.Source
    $QtDir = Split-Path (Split-Path $windeployqt -Parent) -Parent
}

# ---- 2./3. Build and test -------------------------------------------------------
if (-not $SkipBuild) {
    Write-Step 'Configure'
    $engineArg = if ($BackportEngine) { "-DFW_BACKPORT_ENGINE_EXE=$BackportEngine" } else { '-DFW_FETCH_BACKPORT_ENGINE=ON' }
    if ((Invoke-Native { cmake -S $native -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtDir" -DFW_WARNINGS_AS_ERRORS=OFF $engineArg }) -ne 0) { throw 'Configure failed' }
    Write-Step 'Build'
    $jobs = if ($Parallel -gt 0) { @('--parallel', "$Parallel") } else { @('--parallel') }
    if ((Invoke-Native { cmake --build $BuildDir @jobs }) -ne 0) { throw 'Build failed' }
}
if (-not $SkipTests) {
    Write-Step 'CTest'
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:FW_OFFLINE = '1'
    $code = Invoke-Native { ctest --test-dir $BuildDir --timeout 900 --output-on-failure }
    Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    if ($code -ne 0) { throw "CTest failed ($code)" }
}

# ---- 4. Deploy -----------------------------------------------------------------------
Write-Step 'Deploy'
$dist = Join-Path $OutDir 'exFAT Image Builder'
$onefile = Join-Path $OutDir 'exFAT Image Builder.exe'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $BuildDir 'bin\exFAT Image Builder.exe') $dist
$deployed = Join-Path $dist 'exFAT Image Builder.exe'
if ((Invoke-Native { & $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw --compiler-runtime $deployed }) -ne 0) { throw 'windeployqt failed' }
Copy-Item (Join-Path $native 'packaging\README-portable.txt') (Join-Path $dist 'README.txt')
Copy-Item (Join-Path $repo 'LICENSES') -Recurse (Join-Path $dist 'LICENSES')
Copy-Item (Join-Path $native 'packaging\licenses\*') (Join-Path $dist 'LICENSES\')
Copy-Item (Join-Path $native 'packaging\THIRD_PARTY_NOTICES-native.md') (Join-Path $dist 'THIRD_PARTY_NOTICES.md')
# The Backport tab's engine (built into bin\engines by CMake). A release
# without it would show "engine missing", so it is required.
$engines = Join-Path $BuildDir 'bin\engines'
if (-not (Test-Path (Join-Path $engines 'backport\PS5BackportGUI.exe'))) {
    throw "The PS5 Backport engine is missing from $engines (configure with -BackportEngine <exe> or let CMake fetch it)"
}
Copy-Item $engines -Recurse (Join-Path $dist 'engines')

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
    Write-Step 'Portable zip'
    $zipPath = Join-Path $OutDir "exFAT-Image-Builder-Farewell-$version-win64-portable.zip"
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    Compress-Archive -Path $dist -DestinationPath $zipPath
    $assets += $zipPath
}
$sums = foreach ($f in $assets) { (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLower() + '  ' + (Split-Path $f -Leaf) }
[IO.File]::WriteAllText((Join-Path $OutDir 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Step 'Done'
foreach ($f in $assets + (Join-Path $OutDir 'SHA256SUMS.txt')) { Write-Host ("   {0}  ({1:N1} MiB)" -f $f, ((Get-Item -LiteralPath $f).Length / 1MB)) }
