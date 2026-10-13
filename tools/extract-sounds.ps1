# Copies Minecraft's sounds out of a launcher's hash-based asset store into the
# resource layout the client reads: resources/minecraft/assets/minecraft/
#   sounds.json
#   sounds/<name>.ogg
#
# Usage (from the repo root):
#   powershell -File tools\extract-sounds.ps1
#   powershell -File tools\extract-sounds.ps1 -AssetsDir "C:\Users\me\AppData\Roaming\.minecraft\assets"
#   powershell -File tools\extract-sounds.ps1 -All          # also music and records (large)
#
# Safe to re-run: files already extracted with the right size are skipped.
param(
    # The launcher's "assets" folder (contains "indexes" and "objects").
    # Default: the first of GDLauncher / the official launcher that exists.
    [string]$AssetsDir = '',
    # Index file name without ".json" (e.g. "32"). Default: newest index that has sounds.
    [string]$IndexName = '',
    # Where to extract to.
    [string]$OutDir = 'resources\minecraft\assets\minecraft',
    # Include music and records (a large part of the size).
    [switch]$All
)

$ErrorActionPreference = 'Stop'

if (-not $AssetsDir)
{
    $candidates = @(
        "$env:APPDATA\gdlauncher_carbon\data\assets",
        "$env:APPDATA\.minecraft\assets",
        "$HOME/.minecraft/assets",
        "$HOME/.local/share/gdlauncher_carbon/data/assets"
    )
    $AssetsDir = $candidates | Where-Object { Test-Path "$_\indexes" } | Select-Object -First 1
    if (-not $AssetsDir) { throw "No launcher asset folder found. Pass -AssetsDir <path to the folder that contains 'indexes' and 'objects'>." }
}

# Pick the index: the one asked for, else the newest that lists sounds.json.
$indexFiles = Get-ChildItem "$AssetsDir\indexes" -Filter *.json | Sort-Object LastWriteTime -Descending
if ($IndexName) { $indexFiles = $indexFiles | Where-Object { $_.BaseName -eq $IndexName } }
$index = $null
foreach ($file in $indexFiles)
{
    $candidate = Get-Content $file.FullName -Raw | ConvertFrom-Json
    if ($candidate.objects.PSObject.Properties.Name -contains 'minecraft/sounds.json') { $index = $candidate; $indexUsed = $file.Name; break }
}
if (-not $index) { throw "No asset index in '$AssetsDir\indexes' lists minecraft/sounds.json." }
Write-Output "Assets:  $AssetsDir"
Write-Output "Index:   $indexUsed"
Write-Output "Output:  $OutDir"

$skipPrefixes = @()
if (-not $All) { $skipPrefixes = @('minecraft/sounds/music/', 'minecraft/sounds/records/') }

$copied = 0; $skipped = 0; $missing = 0; $bytes = 0
foreach ($entry in $index.objects.PSObject.Properties)
{
    $name = $entry.Name
    $isSound = ($name -eq 'minecraft/sounds.json') -or ($name -like 'minecraft/sounds/*.ogg')
    if (-not $isSound) { continue }
    if ($skipPrefixes | Where-Object { $name.StartsWith($_) }) { continue }

    $hash = $entry.Value.hash
    $source = Join-Path $AssetsDir "objects\$($hash.Substring(0, 2))\$hash"
    # "minecraft/sounds/dig/stone1.ogg" -> "<OutDir>\sounds\dig\stone1.ogg"
    $target = Join-Path $OutDir ($name.Substring('minecraft/'.Length) -replace '/', '\')

    if (-not (Test-Path $source)) { $missing++; continue }
    if ((Test-Path $target) -and ((Get-Item $target).Length -eq $entry.Value.size)) { $skipped++; continue }

    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item $source $target -Force
    $copied++
    $bytes += $entry.Value.size
    if ($copied % 500 -eq 0) { Write-Output "  ...$copied files" }
}

Write-Output ("Done: {0} copied ({1:N0} MB), {2} already present, {3} missing from the store." -f $copied, ($bytes / 1MB), $skipped, $missing)
if ($missing -gt 0) { Write-Output "Missing files usually mean the launcher hasn't downloaded those assets yet; launch the matching Minecraft version once." }
