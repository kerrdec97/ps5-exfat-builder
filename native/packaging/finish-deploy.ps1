<#
.SYNOPSIS
  Turns a windeployqt folder into the folder that ships: the folder edition
  (the zip) and the payload of the single-file exe hold the same files.

.DESCRIPTION
   * The VC++ runtime: vc_redist.x64.exe (the installer windeployqt adds and
     nothing runs) is removed and the runtime DLLs (vcruntime140*.dll,
     msvcp140*.dll, ...) from the MSVC redist folder are put next to the
     program, so it starts on a PC without the VC++ Redistributable.
     -NoAppLocalCrt skips this.
   * Qt plugins and DLLs the app never loads are removed. Only the plugin
     folders listed in $keep are touched; in each, the DLLs not listed are
     deleted (a folder left empty goes too). Why each one stays:
       platforms\qwindows            the only platform plugin a release needs
       styles\qmodernwindowsstyle    loaded at start (message boxes shown
                                     before the app's own style is set)
       imageformats\qjpeg, qwebp     cover pictures (engines/gameinfo/Cover.cpp
                                     accepts PNG, JPEG, WEBP and BMP; PNG and
                                     BMP are built into Qt Gui)
       sqldrivers\qsqlite            the PS5 connect pages read SQLite files
       tls\qschannelbackend          HTTPS (update check, DLC lookup); the
                                     cert-only backend is never chosen while
                                     Schannel is there
     generic, iconengines and networkinformation are removed: the app loads
     no generic plugin, draws its SVG icons itself (QSvgRenderer) and never
     uses QNetworkInformation. dxcompiler.dll and dxil.dll (the DirectX shader
     compiler windeployqt adds for Qt Quick and QRhi) are removed: the app is
     Qt Widgets only.

  Running it again on a finished folder changes nothing. Windows PowerShell
  5.1 compatible.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File native\packaging\finish-deploy.ps1 -DeployDir "dist\exFAT Image Builder"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$DeployDir,
    [string]$CrtDir = '',
    [switch]$NoAppLocalCrt
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$DeployDir = (Resolve-Path -LiteralPath $DeployDir).Path

# Plugin folder -> the DLLs that stay (see above). Folders not named here are
# left alone, so a plugin type a later Qt adds is kept until it is looked at.
$keep = [ordered]@{
    'platforms'          = @('qwindows.dll')
    'styles'             = @('qmodernwindowsstyle.dll')
    'imageformats'       = @('qjpeg.dll', 'qwebp.dll')
    'sqldrivers'         = @('qsqlite.dll')
    'tls'                = @('qschannelbackend.dll')
    'generic'            = @()
    'iconengines'        = @()
    'networkinformation' = @()
}
$unusedDlls = @('dxcompiler.dll', 'dxil.dll')

function Find-CrtDir {
    $candidates = @()
    if ($env:VCToolsRedistDir) { $candidates += Get-ChildItem -Directory -Path (Join-Path $env:VCToolsRedistDir 'x64') -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue }
    if (-not $candidates) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            foreach ($vs in @(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
                # One folder at a time: a wildcard in -Path together with -Filter finds nothing in Windows PowerShell.
                foreach ($ver in @(Get-ChildItem -Directory -LiteralPath (Join-Path $vs 'VC\Redist\MSVC') -ErrorAction SilentlyContinue)) {
                    $candidates += Get-ChildItem -Directory -LiteralPath (Join-Path $ver.FullName 'x64') -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue
                }
            }
        }
    }
    $best = $candidates | Sort-Object { $_.Parent.Parent.Name -as [version] }, Name -Descending | Select-Object -First 1
    if ($best) { return $best.FullName }
    return $null
}

if (-not $NoAppLocalCrt) {
    $redist = Join-Path $DeployDir 'vc_redist.x64.exe'
    if (Test-Path -LiteralPath $redist) { Remove-Item -LiteralPath $redist -Force; Write-Host '   - vc_redist.x64.exe (VC++ runtime DLLs are added instead)' }
    if (-not $CrtDir) { $CrtDir = Find-CrtDir }
    if (-not $CrtDir -or -not (Test-Path -LiteralPath $CrtDir)) {
        throw 'The MSVC redist folder (Microsoft.VC14x.CRT) was not found. Run from a VS developer prompt, pass -CrtDir, or -NoAppLocalCrt.'
    }
    $dlls = @(Get-ChildItem -LiteralPath $CrtDir -Filter '*.dll')
    if ($dlls.Count -eq 0) { throw "No DLLs in $CrtDir" }
    foreach ($dll in $dlls) { Copy-Item -LiteralPath $dll.FullName -Destination $DeployDir -Force }
    Write-Host "   + $($dlls.Count) VC++ runtime DLLs from $CrtDir"
}

$removed = @()
foreach ($name in $unusedDlls) {
    $f = Join-Path $DeployDir $name
    if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force; $removed += $name }
}
foreach ($folder in $keep.Keys) {
    $dir = Join-Path $DeployDir $folder
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -File)) {
        if ($keep[$folder] -notcontains $f.Name.ToLowerInvariant()) {
            Remove-Item -LiteralPath $f.FullName -Force
            $removed += "$folder\$($f.Name)"
        }
    }
    if (@(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) { Remove-Item -LiteralPath $dir -Force }
}
if ($removed.Count -gt 0) { Write-Host "   - not used by the app: $($removed -join ', ')" }
