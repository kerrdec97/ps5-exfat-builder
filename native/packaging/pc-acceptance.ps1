<#
.SYNOPSIS
  PC acceptance run of the native exFAT Image Builder on a real Windows PC:
  build native/ with MSVC + Qt, run CTest, deploy the Qt runtime, run the
  deployed exe's --self-test and --screenshot-pages, pack the single-file exe
  and run its --self-test, launch the deployed exe for real and check it is
  still running, package a portable zip and record SHA-256s.

.DESCRIPTION
  Everything goes into one evidence folder (default: <repo>\_PC_TEST_EVIDENCE\
  <version>-<commit>). Nothing needs administrator rights, nothing is
  installed, and the user's own settings are never touched: every run of the
  app uses a private data folder inside the evidence folder (FW_DATA_DIR) and
  FW_OFFLINE=1 (no update check, no Discord message). FW_NO_POWER_ACTIONS=1:
  no test and no run of the app can put the PC to sleep or shut it down.

  The script only uses tools that are already installed. When the Microsoft
  C++ Build Tools are missing it writes BLOCKER.md with the exact install
  command and stops with exit code 2. It never installs anything itself:
  installing the Build Tools needs a UAC prompt and accepting Microsoft's
  licence terms, which only the owner may do.

  This is a PC test. It does not exercise a PS5 console, real Sony
  Publishing Tools or a real game dump; SUMMARY.md lists those as not run.

  Windows PowerShell 5.1 compatible.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\pc-acceptance.ps1
#>
[CmdletBinding()]
param(
    # Default: the QT_ROOT_DIR or Qt6_DIR environment variable, else qmake on PATH.
    [string]$QtDir = '',
    [string]$OutRoot = '',
    [string]$BuildDir = '',
    # The bundled PS5 Backport engine, as in make-release.ps1. Empty: CMake
    # downloads the pinned release and checks its SHA-256.
    [string]$BackportEngine = '',
    [int]$AliveSeconds = 30,
    [switch]$SkipBuild
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$native = Join-Path $repo 'native'
if (-not $OutRoot) { $OutRoot = Join-Path $repo '_PC_TEST_EVIDENCE' }
if (-not $BuildDir) { $BuildDir = Join-Path $native 'build-pc-release' }

$version = (Select-String -Path (Join-Path $native 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$commit = (& git -C $repo rev-parse --short=12 HEAD).Trim()
$evidence = Join-Path $OutRoot "$version-$commit"
New-Item -ItemType Directory -Force $evidence | Out-Null
$results = [ordered]@{}

function Write-Step([string]$text) { Write-Host "== $text" }

# Runs a native command with stdout and stderr in one UTF-8 log (and on the
# console) and returns its exit code. Windows PowerShell turns a native
# command's stderr into terminating errors under ErrorActionPreference Stop,
# so the preference is Continue while it runs.
function Invoke-Logged([string]$logName, [scriptblock]$command) {
    $log = Join-Path $evidence $logName
    [IO.File]::WriteAllText($log, '', (New-Object Text.UTF8Encoding($false)))
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $command 2>&1 | ForEach-Object {
            $line = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
            [IO.File]::AppendAllText($log, $line + [Environment]::NewLine, (New-Object Text.UTF8Encoding($false)))
            Write-Host $line
        }
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Save-Text([string]$name, [string]$text) {
    [IO.File]::WriteAllText((Join-Path $evidence $name), $text, (New-Object Text.UTF8Encoding($false)))
}

# A step that cannot go on still leaves a SUMMARY.md that says which one.
function Stop-Run([string]$step, [string]$message) {
    $results[$step] = "FAILED ($message)"
    $rows = ($results.GetEnumerator() | ForEach-Object { "| $($_.Key) | $($_.Value) |" }) -join "`n"
    Save-Text 'SUMMARY.md' ("# PC acceptance run: exFAT Image Builder $version (native)`n`nStopped early.`n`n| Check | Result |`n|---|---|`n$rows`n")
    Write-Host "FAILED: $step ($message)"
    exit 1
}

function Write-Blocker([string]$what) {
    $cmd = 'winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--wait --passive ' +
           '--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ' +
           '--add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.26100"'
    $text = @"
# PC acceptance run blocked

$what

Missing tool chain: the Microsoft C++ Build Tools (MSVC v143, Windows SDK) with
Microsoft's bundled CMake and Ninja. The installer needs **administrator
elevation** (a UAC prompt for the Visual Studio Installer, publisher Microsoft
Corporation) and **acceptance of the Microsoft Software License Terms** for
Visual Studio Build Tools 2022 (https://visualstudio.microsoft.com/license-terms/).
Only the owner can approve either, so nothing was installed.

Install (run it yourself; Windows shows the UAC prompt):

    $cmd

Then run this script again:

    powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\pc-acceptance.ps1
"@
    Save-Text 'BLOCKER.md' $text
    Write-Host $text
    exit 2
}

# ---- 1. Tool chain (known locations only) ------------------------------------
Write-Step 'Tool chain'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Write-Blocker "vswhere.exe is not at $vswhere (no Visual Studio or Build Tools installed)." }
$vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $vsPath) { Write-Blocker 'Visual Studio is installed but without the C++ x64 tools (Microsoft.VisualStudio.Component.VC.Tools.x86.x64).' }
$vsDevCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $vsDevCmd)) { Write-Blocker "VsDevCmd.bat is missing under $vsPath." }

# Import the x64 developer environment into this process.
$envDump = & cmd.exe /s /c "`"$vsDevCmd`" -no_logo -arch=amd64 -host_arch=amd64 && set"
foreach ($line in $envDump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1), 'Process') }
}
foreach ($tool in 'cl', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Blocker "'$tool' is not on PATH after VsDevCmd (add the component Microsoft.VisualStudio.Component.VC.CMake.Project for CMake and Ninja)."
    }
}
if (-not $QtDir) {
    if ($env:QT_ROOT_DIR) { $QtDir = $env:QT_ROOT_DIR }
    elseif ($env:Qt6_DIR -and (Test-Path (Join-Path $env:Qt6_DIR '..\..\..\bin'))) { $QtDir = (Resolve-Path (Join-Path $env:Qt6_DIR '..\..\..')).Path }
    else {
        $qmake = Get-Command qmake -ErrorAction SilentlyContinue
        if ($qmake) { $QtDir = Split-Path (Split-Path $qmake.Source -Parent) -Parent }
    }
}
if (-not $QtDir -or -not (Test-Path (Join-Path $QtDir 'bin\qmake.exe'))) {
    throw 'Qt was not found: pass -QtDir <...\msvc2022_64>, set QT_ROOT_DIR, or put qmake on PATH.'
}
$env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH

$clBanner = (& cmd.exe /c 'cl 2>&1' | Select-Object -First 1)
$tools = [ordered]@{
    visualStudio = $vsPath
    cl           = "$clBanner"
    cmake        = ((& cmake --version) | Select-Object -First 1)
    ninja        = ((& ninja --version) | Select-Object -First 1)
    qt           = ((& (Join-Path $QtDir 'bin\qmake.exe') -query QT_VERSION) | Select-Object -First 1)
    qtDir        = $QtDir
    commit       = (& git -C $repo rev-parse HEAD).Trim()
    branch       = (& git -C $repo rev-parse --abbrev-ref HEAD).Trim()
    dirtyFiles   = @(& git -C $repo status --porcelain=v1)
    windows      = (Get-CimInstance Win32_OperatingSystem | ForEach-Object { "$($_.Caption) $($_.Version)" })
    computer     = $env:COMPUTERNAME
    elevated     = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    started      = (Get-Date).ToString('o')
}
Save-Text 'tools.json' ($tools | ConvertTo-Json -Depth 4)
$tools.GetEnumerator() | ForEach-Object { Write-Host ("  {0}: {1}" -f $_.Key, ($_.Value -join ' ')) }

# ---- 2. Configure and build ---------------------------------------------------
if (-not $SkipBuild) {
    Write-Step 'Configure'
    $engineArg = if ($BackportEngine) { "-DFW_BACKPORT_ENGINE_EXE=$BackportEngine" } else { '-DFW_FETCH_BACKPORT_ENGINE=ON' }
    $code = Invoke-Logged 'configure.log' { & cmake -S $native -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtDir" -DFW_WARNINGS_AS_ERRORS=OFF $engineArg }
    if ($code -ne 0) { Stop-Run 'configure' 'CMake configure failed, see configure.log' }
    $results.configure = 'passed'

    Write-Step 'Build'
    $code = Invoke-Logged 'build.log' { & cmake --build $BuildDir --parallel }
    if ($code -ne 0) { Stop-Run 'build' 'see build.log' }
    $results.build = 'passed'
}

# ---- 3. Automated tests (CTest) ------------------------------------------------
Write-Step 'CTest'
$env:QT_QPA_PLATFORM = 'offscreen'
$env:FW_OFFLINE = '1'
# Sleep, shut down and restart are refused for every test and every run of the
# app below (beside the app's own refusal in test programs).
$env:FW_NO_POWER_ACTIONS = '1'
Remove-Item Env:FW_WINDOWS_MOUNT_TEST -ErrorAction SilentlyContinue # mounting a VHD needs administrator rights
$ctestCode = Invoke-Logged 'ctest.log' { & ctest --test-dir $BuildDir --timeout 900 --output-on-failure --output-junit (Join-Path $evidence 'ctest-junit.xml') }
$summaryMatch = Select-String -Path (Join-Path $evidence 'ctest.log') -Pattern 'tests passed' | Select-Object -Last 1
$summaryLine = if ($summaryMatch) { $summaryMatch.Line } else { 'no result line in ctest.log' }
$results.ctest = $(if ($ctestCode -eq 0) { "passed ($summaryLine)" } else { "FAILED ($summaryLine)" })
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue

# ---- 4. Deploy the Qt runtime ------------------------------------------------------
Write-Step 'Deploy'
$dist = Join-Path $evidence 'exFAT Image Builder'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
$built = Join-Path $BuildDir 'bin\exFAT Image Builder.exe'
Copy-Item $built $dist
$exe = Join-Path $dist 'exFAT Image Builder.exe'
$code = Invoke-Logged 'windeployqt.log' { & (Join-Path $QtDir 'bin\windeployqt.exe') --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime $exe }
if ($code -ne 0) { Stop-Run 'deploy' 'windeployqt failed, see windeployqt.log' }
# The same files as make-release.ps1 section 4, so the run tests what a release holds.
Copy-Item (Join-Path $native 'packaging\README-portable.txt') (Join-Path $dist 'README.txt')
Copy-Item (Join-Path $repo 'LICENSES') -Recurse (Join-Path $dist 'LICENSES')
Copy-Item (Join-Path $native 'packaging\licenses\*') (Join-Path $dist 'LICENSES\')
Copy-Item (Join-Path $native 'packaging\THIRD_PARTY_NOTICES-native.md') (Join-Path $dist 'THIRD_PARTY_NOTICES.md')
Copy-Item (Join-Path $native 'packaging\LICENCE.txt') (Join-Path $dist 'LICENCE.txt')
$engines = Join-Path $BuildDir 'bin\engines'
if (-not (Test-Path (Join-Path $engines 'backport\PS5BackportGUI.exe'))) {
    Stop-Run 'deploy' "the PS5 Backport engine is missing from $engines (configure with -BackportEngine <exe> or let CMake fetch it)"
}
Copy-Item $engines -Recurse (Join-Path $dist 'engines')
$code = Invoke-Logged 'finish-deploy.log' { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $native 'packaging\finish-deploy.ps1') -DeployDir $dist }
if ($code -ne 0) { Stop-Run 'deploy' 'finish-deploy.ps1 failed, see finish-deploy.log' }
$results.deploy = 'passed'

# The app's own data for every run below: never the user's settings, nor the
# PS5 Backport engine's files (shared with the standalone program).
$appData = Join-Path $evidence 'app-data'
$env:FW_DATA_DIR = $appData
$env:FW_OFFLINE = '1'
$env:D97BETA_CONFIG_DIR = Join-Path $appData 'backport-config'
$env:D97BETA_DATA_DIR = Join-Path $appData 'backport-data'
$env:D97BETA_ENGINE_HOME = Join-Path $appData 'backport-engine-home'

# ---- 5. --self-test of the deployed exe -------------------------------------------
Write-Step 'Self-test'
$p = Start-Process -FilePath $exe -ArgumentList '--self-test' -RedirectStandardOutput (Join-Path $evidence 'self-test.json') `
    -RedirectStandardError (Join-Path $evidence 'self-test.err') -PassThru -Wait -NoNewWindow
$results.selfTest = $(if ($p.ExitCode -eq 0) { 'passed' } else { "FAILED (exit $($p.ExitCode))" })

# ---- 5b. Single-file exe: pack it, then --self-test from a fresh folder (first and cached start)
Write-Step 'Single-file exe'
$onefile = Join-Path $evidence 'exFAT Image Builder.exe'
$code = Invoke-Logged 'onefile.log' { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $native 'packaging\make-onefile.ps1') -DeployDir $dist -Output $onefile -BuildDir $BuildDir -Version $version }
if ($code -ne 0) {
    $results.onefile = 'FAILED (make-onefile.ps1, see onefile.log)'
} else {
    $runDir = Join-Path $evidence 'onefile-run'
    New-Item -ItemType Directory -Force $runDir | Out-Null
    Copy-Item $onefile $runDir -Force
    $savedLocal = $env:LOCALAPPDATA
    $env:LOCALAPPDATA = Join-Path $evidence 'onefile-cache' # never the user's own cache
    if (Test-Path $env:LOCALAPPDATA) { Remove-Item -Recurse -Force $env:LOCALAPPDATA }
    New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null
    $times = @()
    $ok = $true
    foreach ($round in 'first', 'cached') {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process -FilePath (Join-Path $runDir 'exFAT Image Builder.exe') -ArgumentList '--self-test' `
            -RedirectStandardOutput (Join-Path $evidence "onefile-self-test-$round.json") `
            -RedirectStandardError (Join-Path $evidence "onefile-self-test-$round.err") -PassThru -Wait -NoNewWindow
        $sw.Stop()
        $times += "$round start $($sw.ElapsedMilliseconds) ms"
        if ($p.ExitCode -ne 0) { $ok = $false }
    }
    $env:LOCALAPPDATA = $savedLocal
    $size = [math]::Round((Get-Item $onefile).Length / 1MB, 1)
    $results.onefile = $(if ($ok) { "passed ($size MiB; --self-test $($times -join ', '))" } else { "FAILED (--self-test: $($times -join ', '))" })
}

# ---- 6. Screenshot every page --------------------------------------------------
Write-Step 'Page screenshots'
$pages = Join-Path $evidence 'pages'
$p = Start-Process -FilePath $exe -ArgumentList '--screenshot-pages', "`"$pages`"" -RedirectStandardOutput (Join-Path $evidence 'screenshots.txt') `
    -RedirectStandardError (Join-Path $evidence 'screenshots.err') -PassThru -Wait -NoNewWindow
$pngs = @(Get-ChildItem $pages -Filter *.png -ErrorAction SilentlyContinue).Count
$results.screenshots = $(if ($p.ExitCode -eq 0 -and $pngs -gt 0) { "passed ($pngs pages)" } else { "FAILED (exit $($p.ExitCode), $pngs pages)" })

# ---- 7. Real launch: still running after N seconds -------------------------------
Write-Step "Launch ($AliveSeconds s)"
$p = Start-Process -FilePath $exe -PassThru
$null = $p.Handle # Windows PowerShell 5.1 only reports the exit code of a process whose handle it holds
Start-Sleep -Seconds $AliveSeconds
$alive = -not $p.HasExited
try {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save((Join-Path $evidence 'desktop-launch.png'))
    $g.Dispose(); $bmp.Dispose()
} catch { Save-Text 'desktop-launch.err' "$_" }
$proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
$launch = [ordered]@{
    alive          = $alive
    seconds        = $AliveSeconds
    workingSetMiB  = $(if ($proc) { [math]::Round($proc.WorkingSet64 / 1MB, 1) } else { $null })
    mainWindowTitle = $(if ($proc) { $proc.MainWindowTitle } else { $null })
    exitCode       = $(if ($alive) { $null } else { $p.ExitCode })
}
Save-Text 'launch.json' ($launch | ConvertTo-Json)
if ($alive) { Stop-Process -Id $p.Id -Force }
$results.launch = $(if ($alive) { "passed (running after $AliveSeconds s, $($launch.workingSetMiB) MiB, window '$($launch.mainWindowTitle)')" } else { "FAILED (exited with $($p.ExitCode))" })
foreach ($k in 'FW_DATA_DIR', 'D97BETA_CONFIG_DIR', 'D97BETA_DATA_DIR', 'D97BETA_ENGINE_HOME') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }

# ---- 8. Portable zip and checksums --------------------------------------------------
Write-Step 'Package'
$zip = Join-Path $evidence "exFAT-Image-Builder-$version-win64-portable.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# As make-release.ps1: entry names with "/" (Compress-Archive of Windows
# PowerShell 5.1 writes "\", which the zip format does not allow).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$base = (Get-Item -LiteralPath $dist).FullName.TrimEnd('\') # as Get-ChildItem spells it (an -OutRoot may be relative)
$top = Split-Path $base -Leaf
$archive = [IO.Compression.ZipFile]::Open($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($zip), [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in @(Get-ChildItem -LiteralPath $base -Recurse -File -Force | Sort-Object FullName)) {
        $entry = $top + '/' + $f.FullName.Substring($base.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
$sums = @()
# The two files in the evidence folder itself (the deployed exe has the same
# leaf name as the single-file exe; its hash is in SUMMARY.md).
foreach ($f in @($onefile, $zip)) {
    if (-not (Test-Path -LiteralPath $f)) { continue }
    $sums += ((Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLower() + '  ' + (Split-Path $f -Leaf))
}
Save-Text 'SHA256SUMS.txt' (($sums -join "`n") + "`n")
$exeSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash.ToLower()
$results.package = 'portable zip written (this script builds no installer)'

# ---- 9. Summary -------------------------------------------------------------------
$failed = @($results.GetEnumerator() | Where-Object { "$($_.Value)" -like 'FAILED*' }).Count
$rows = ($results.GetEnumerator() | ForEach-Object { "| $($_.Key) | $($_.Value) |" }) -join "`n"
$summary = @"
# PC acceptance run: exFAT Image Builder $version (native)

* Commit: ``$($tools.commit)`` ($($tools.branch)); uncommitted files: $(@($tools.dirtyFiles).Count)
* PC: $($tools.computer), $($tools.windows); elevated: $($tools.elevated)
* Tool chain: $($tools.cl); $($tools.cmake); Ninja $($tools.ninja); Qt $($tools.qt) ($QtDir)
* Executable: ``$exe``
* **SHA-256 of the executable: ``$exeSha``**
* Single-file exe (packed like the release download): ``$onefile`` (SHA-256 in SHA256SUMS.txt)

## PC tests (this machine)

| Check | Result |
|---|---|
$rows

The real-Windows-exFAT-driver test (``tst_exfat_windows`` with
FW_WINDOWS_MOUNT_TEST=1) mounts a VHD and needs administrator rights; this
script always runs it in its non-mounting mode (this run was elevated:
$($tools.elevated)).

## Not tested here (needs hardware or user-supplied tools)

* PS5 console: mounting the images, FTP/payload/klog/Y2JB/ShadowMount+ against
  a real console, launching a backported game on its target firmware.
* Sony Publishing Tools (FPKG builds) and a real game dump.

Overall: $(if ($failed -eq 0) { 'every PC check passed' } else { "$failed PC check(s) FAILED" }).
"@
Save-Text 'SUMMARY.md' $summary
Write-Host $summary
if ($failed -ne 0) { exit 1 }
exit 0
