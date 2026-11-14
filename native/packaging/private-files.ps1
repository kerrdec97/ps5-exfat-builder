# Dot-sourced by make-release.ps1 and make-onefile.ps1. Windows PowerShell 5.1
# compatible.
#
# Find-PrivateFiles <folder>: what only a private build may carry, relative to
# <folder>: the tools\ folders make-release.ps1 -PrivateToolkit,
# -PrivateDlcEngine and -PrivateYoutube create, and, wherever they are, the
# files those folders hold (prospero-pub-cmd.exe, fpkg-cli.exe,
# libScePubTools.dll, .pkg packages), so a renamed copy is found too.
function Find-PrivateFiles([string]$folder) {
    # As Get-ChildItem spells it (Resolve-Path keeps a short 8.3 name).
    $root = (Get-Item -LiteralPath $folder).FullName.TrimEnd('\')
    $found = @()
    foreach ($rel in 'tools\fpkg-toolkit', 'tools\fpkg-cli', 'tools\youtube') {
        if (Test-Path -LiteralPath (Join-Path $root $rel)) { $found += $rel }
    }
    $names = @('prospero-pub-cmd.exe', 'fpkg-cli.exe', 'libscepubtools.dll')
    foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force)) {
        $rel = $f.FullName.Substring($root.Length + 1)
        if ($found | Where-Object { $rel.StartsWith($_ + '\', [StringComparison]::OrdinalIgnoreCase) }) { continue }
        if ($names -contains $f.Name.ToLowerInvariant() -or $f.Extension -ieq '.pkg') { $found += $rel }
    }
    return $found
}
