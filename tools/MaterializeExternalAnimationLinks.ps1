param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'dragon-aspect-flight-animation-links.json'),
    [Parameter(Mandatory = $true)]
    [string]$TargetDataRoot,
    [string]$ExternalOarRoot = 'C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\[NoDelete] [001.00202] More Draconic Aspect Can Fly (With Collisions)\meshes\actors\character\animations\OpenAnimationReplacer\More Dragonic Dragon Aspect Can Fly',
    [switch]$AllowCopyFallback
)

$ErrorActionPreference = 'Stop'

function Resolve-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Join-PortablePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Relative
    )
    $nativeRelative = $Relative -replace '/', [System.IO.Path]::DirectorySeparatorChar
    return Join-Path $Root $nativeRelative
}

function Assert-UnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $fullPath = Resolve-FullPath $Path
    $fullRoot = (Resolve-FullPath $Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to write outside target data root: $fullPath"
    }
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Animation link manifest not found: $ManifestPath"
}

if (-not (Test-Path -LiteralPath $ExternalOarRoot -PathType Container)) {
    throw "External More Draconic OAR root not found: $ExternalOarRoot"
}

New-Item -ItemType Directory -Path $TargetDataRoot -Force | Out-Null

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$linked = 0
$copied = 0

foreach ($entry in $manifest.entries) {
    $sourcePath = Join-PortablePath -Root $ExternalOarRoot -Relative $entry.source
    $targetPath = Join-PortablePath -Root $TargetDataRoot -Relative $entry.target

    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "External animation missing: $sourcePath"
    }

    $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
    if ($sourceHash -ne $entry.sha256) {
        throw "External animation hash mismatch for $sourcePath"
    }

    Assert-UnderRoot -Path $targetPath -Root $TargetDataRoot
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force | Out-Null

    if (Test-Path -LiteralPath $targetPath) {
        Remove-Item -LiteralPath $targetPath -Force
    }

    try {
        & fsutil hardlink create $targetPath $sourcePath | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "fsutil hardlink create failed with exit code $LASTEXITCODE"
        }
        $linked += 1
    } catch {
        if (-not $AllowCopyFallback) {
            throw
        }

        Copy-Item -LiteralPath $sourcePath -Destination $targetPath -Force
        $copied += 1
    }
}

Write-Host "Materialized Dragon Aspect Flight animation links: $linked hardlinks, $copied copied fallback files."
