#!/usr/bin/env pwsh
# Assembles the portable release zip from the msbuild output directory.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$OutZip
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path (Join-Path $BuildDir 'DiskForge.exe'))) {
    throw "DiskForge.exe not found in $BuildDir"
}

$stage = Join-Path $env:TEMP ("diskforge-stage-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null

# app binaries + self-contained WinAppSDK runtime (skip compiler intermediates)
$keep = @('*.exe', '*.dll', '*.pri')
Get-ChildItem $BuildDir -File | Where-Object {
    $name = $_.Name
    ($keep | Where-Object { $name -like $_ }) -and
    $name -notmatch '\.(pdb|lib|exp|obj|iobj|ipdb|lastbuildstate|log|tlog)$' -and
    $name -notmatch '^\.'
} | ForEach-Object {
    Copy-Item $_.FullName -Destination $stage
}

# Assets folder (if the build produced one)
if (Test-Path (Join-Path $BuildDir 'Assets')) {
    Copy-Item (Join-Path $BuildDir 'Assets') -Destination $stage -Recurse
}

# docs
$root = Split-Path -Parent $PSScriptRoot
Copy-Item (Join-Path $root 'LICENSE') -Destination $stage
$readme = Join-Path $root 'README.md'
if (Test-Path $readme) { Copy-Item $readme -Destination $stage }

if (Test-Path $OutZip) { Remove-Item $OutZip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $OutZip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force

$size = (Get-Item $OutZip).Length / 1MB
Write-Host "Portable zip created: $OutZip ($([math]::Round($size, 1)) MiB)"
