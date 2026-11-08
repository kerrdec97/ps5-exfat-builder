<#
.SYNOPSIS
  Packs a deployed exFAT Image Builder folder into ONE executable: the
  single-file launcher (fw_onefile_stub.exe, built by CMake) with the whole
  folder appended (fw_onefile_pack.exe, built by CMake).

.DESCRIPTION
  Every file under -DeployDir is packed, sub-folders included: the
  windeployqt output and anything added to it, for example
  engines\backport\PS5BackportGUI.exe. -Extra adds files or folders from
  elsewhere without touching the deploy folder ("<source>=<path inside>").

  The payload is staged in a temporary copy; the deploy folder is not changed.
  In the copy:
   * vc_redist.x64.exe (the VC++ installer windeployqt adds) is left out, and
   * the VC++ runtime DLLs (vcruntime140*.dll, msvcp140*.dll, ...) from the
     MSVC redist folder are added next to the program, so it starts on a PC
     without the VC++ Redistributable. -NoAppLocalCrt skips both.

  Already-compressed files are stored as they are (the packer stores every
  chunk that does not shrink by 3 %); everything else is LZMS-compressed.
  The result is checked with `fw_onefile_pack verify`.

  Windows PowerShell 5.1 compatible. Needs no administrator rights.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\make-onefile.ps1 `
      -DeployDir "dist\exFAT Image Builder" -Output "dist\exFAT Image Builder.exe" -BuildDir build

.EXAMPLE
  ... -Extra 'D:\backport-engine=engines\backport'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$DeployDir,
    [string]$Output = '',
    [string]$BuildDir = '',
    [string]$Stub = '',
    [string]$Packer = '',
    [string]$Version = '',
    [string]$Entry = 'exFAT Image Builder.exe',
    [string[]]$Extra = @(),
    [string]$CrtDir = '',
    [switch]$NoAppLocalCrt,
    [ValidateSet('lzms', 'xpress-huff', 'mszip', 'store')] [string]$Method = 'lzms'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$native = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$DeployDir = (Resolve-Path $DeployDir).Path
if (-not $Output) { $Output = Join-Path (Split-Path $DeployDir -Parent) 'exFAT Image Builder.exe' }
$Output = [IO.Path]::GetFullPath($Output)
if (-not $BuildDir) { $BuildDir = Join-Path $native 'build-pc-release' }
if (-not $Stub) { $Stub = Join-Path $BuildDir 'bin\fw_onefile_stub.exe' }
if (-not $Packer) { $Packer = Join-Path $BuildDir 'bin\fw_onefile_pack.exe' }
foreach ($tool in $Stub, $Packer) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Not built: $tool (build the fw_onefile and fw_onefile_pack targets)" }
}
if (-not $Version) {
    $Version = (Select-String -Path (Join-Path $native 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
}
if (-not (Test-Path -LiteralPath (Join-Path $DeployDir $Entry))) { throw "The program is not in the deploy folder: $Entry" }

# Robocopy exit codes below 8 mean success.
function Copy-Tree([string]$from, [string]$to) {
    & robocopy $from $to /E /COPY:DAT /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying $from to $to failed (robocopy $LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
}

function Find-CrtDir {
    $candidates = @()
    if ($env:VCToolsRedistDir) { $candidates += Get-ChildItem -Directory -Path (Join-Path $env:VCToolsRedistDir 'x64') -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue }
    if (-not $candidates) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            foreach ($vs in @(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
                $candidates += Get-ChildItem -Directory -Path (Join-Path $vs 'VC\Redist\MSVC\*\x64') -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue
            }
        }
    }
    $best = $candidates | Sort-Object { $_.Parent.Parent.Name -as [version] }, Name -Descending | Select-Object -First 1
    if ($best) { return $best.FullName }
    return $null
}

$stage = Join-Path ([IO.Path]::GetTempPath()) ('fw-onefile-stage-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
$started = Get-Date
try {
    Write-Host "== Staging $DeployDir"
    Copy-Tree $DeployDir $stage
    foreach ($item in $Extra) {
        $parts = $item -split '=', 2
        $src = (Resolve-Path $parts[0]).Path
        $dest = if ($parts.Count -gt 1 -and $parts[1]) { $parts[1] } else { Split-Path $src -Leaf }
        $target = Join-Path $stage $dest
        Write-Host "   + $src -> $dest"
        if (Test-Path -LiteralPath $src -PathType Container) {
            Copy-Tree $src $target
        } else {
            New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
            Copy-Item -LiteralPath $src -Destination $target -Force
        }
    }
    if (-not $NoAppLocalCrt) {
        $redist = Join-Path $stage 'vc_redist.x64.exe'
        if (Test-Path -LiteralPath $redist) { Remove-Item -LiteralPath $redist -Force; Write-Host '   - vc_redist.x64.exe (VC++ runtime DLLs are added instead)' }
        if (-not $CrtDir) { $CrtDir = Find-CrtDir }
        if (-not $CrtDir -or -not (Test-Path -LiteralPath $CrtDir)) {
            throw 'The MSVC redist folder (Microsoft.VC14x.CRT) was not found. Run from a VS developer prompt, pass -CrtDir, or -NoAppLocalCrt.'
        }
        $dlls = @(Get-ChildItem -LiteralPath $CrtDir -Filter '*.dll')
        foreach ($dll in $dlls) { Copy-Item -LiteralPath $dll.FullName -Destination $stage -Force }
        Write-Host "   + $($dlls.Count) VC++ runtime DLLs from $CrtDir"
    }

    $files = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
    $raw = ($files | Measure-Object -Property Length -Sum).Sum
    Write-Host ("== Packing {0} files, {1:N1} MiB ({2})" -f $files.Count, ($raw / 1MB), $Method)
    New-Item -ItemType Directory -Force (Split-Path $Output -Parent) | Out-Null
    & $Packer pack --stub $Stub --input $stage --entry $Entry --output $Output --version $Version --method $Method
    if ($LASTEXITCODE -ne 0) { throw "fw_onefile_pack failed ($LASTEXITCODE)" }
    & $Packer verify $Output
    if ($LASTEXITCODE -ne 0) { throw "fw_onefile_pack verify failed ($LASTEXITCODE)" }
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
$size = (Get-Item -LiteralPath $Output).Length
Write-Host ("== {0}: {1:N1} MiB, {2:N1} s" -f $Output, ($size / 1MB), ((Get-Date) - $started).TotalSeconds)
