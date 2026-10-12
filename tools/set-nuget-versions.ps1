#!/usr/bin/env pwsh
# Pins the NuGet packages used by DiskForge.vcxproj / packages.config to the latest
# stable versions available on nuget.org (CI runs this before restore so the build
# never breaks due to a hand-pinned version drifting out of existence).
param(
    [string]$WinAppSdkPattern = '^1\.',
    [switch]$KeepExisting
)

$ErrorActionPreference = 'Stop'

function Get-LatestStable([string]$packageId, [string]$pattern) {
    $index = Invoke-RestMethod -Uri "https://api.nuget.org/v3-flatcontainer/$($packageId.ToLower())/index.json" -TimeoutSec 30
    $stable = @($index.versions | Where-Object { $_ -notmatch '-' })
    if ($pattern) { $stable = @($stable | Where-Object { $_ -match $pattern }) }
    if ($stable.Count -eq 0) { throw "no stable version found for $packageId" }
    return $stable[-1]
}

$root = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $root 'src\app'

$packages = @(
    @{ id = 'Microsoft.WindowsAppSDK';       pattern = $WinAppSdkPattern },
    @{ id = 'Microsoft.Windows.SDK.BuildTools'; pattern = $null },
    @{ id = 'Microsoft.Windows.CppWinRT';    pattern = '^2\.0\.' }
)

$configPath = Join-Path $appDir 'packages.config'
[xml]$config = Get-Content $configPath

foreach ($p in $packages) {
    $latest = Get-LatestStable -packageId $p.id -pattern $p.pattern
    Write-Host "$($p.id) -> $latest"

    $node = $config.packages.package | Where-Object { $_.id -eq $p.id }
    $oldVersion = $node.version
    $node.version = $latest

    # rewrite versioned package paths in the vcxproj
    $vcxproj = Join-Path $appDir 'DiskForge.vcxproj'
    (Get-Content $vcxproj -Raw) |
        ForEach-Object { $_ -replace [regex]::Escape("packages\$($p.id).$oldVersion\"), "packages\$($p.id).$latest\" } |
        Set-Content $vcxproj -NoNewline -Encoding UTF8
}

$config.Save($configPath)
Write-Host 'packages.config and DiskForge.vcxproj updated.'
