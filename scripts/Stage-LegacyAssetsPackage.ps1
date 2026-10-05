param(
    [string]$OutputRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/LegacyAssetsPackage')
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot 'Packages/com.mythicfoundry.unity-editor-dark-mode'
$packageEditorRoot = Join-Path $packageRoot 'Editor'
$resolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$resolvedRepoRoot = [System.IO.Path]::GetFullPath($repoRoot)
$volumeRoot = [System.IO.Path]::GetPathRoot($resolvedOutputRoot)

if ([string]::IsNullOrWhiteSpace($resolvedOutputRoot) -or
    $resolvedOutputRoot.TrimEnd('\', '/') -eq $volumeRoot.TrimEnd('\', '/') -or
    $resolvedOutputRoot.TrimEnd('\', '/') -eq $resolvedRepoRoot.TrimEnd('\', '/')) {
    throw "Refusing unsafe legacy package output path: $resolvedOutputRoot"
}
if (Test-Path -LiteralPath $resolvedOutputRoot) {
    throw "Legacy package output already exists: $resolvedOutputRoot"
}

$legacyEditorRoot = Join-Path $resolvedOutputRoot 'Assets/Editor/UnityEditorDarkMode'
$legacyNativeRoot = Join-Path $legacyEditorRoot 'x86_64'
[System.IO.Directory]::CreateDirectory($legacyNativeRoot) | Out-Null

$fileMappings = @(
    [pscustomobject]@{
        Source = Join-Path $packageEditorRoot 'UnityEditorDarkModeBootstrap.cs'
        Destination = Join-Path $legacyEditorRoot 'UnityEditorDarkModeBootstrap.cs'
    },
    [pscustomobject]@{
        Source = Join-Path $packageEditorRoot 'UnityEditorDarkMode.dll'
        Destination = Join-Path $legacyNativeRoot 'UnityEditorDarkMode.dll'
    },
    [pscustomobject]@{
        Source = Join-Path $packageEditorRoot 'UnityEditorDarkMode.dll.ini'
        Destination = Join-Path $legacyNativeRoot 'UnityEditorDarkMode.dll.ini'
    },
    [pscustomobject]@{
        Source = Join-Path $packageRoot 'LICENSE.md'
        Destination = Join-Path $resolvedOutputRoot 'LICENSE.md'
    }
)

foreach ($mapping in $fileMappings) {
    if (-not (Test-Path -LiteralPath $mapping.Source -PathType Leaf)) {
        throw "Legacy package source file is missing: $($mapping.Source)"
    }

    Copy-Item -LiteralPath $mapping.Source -Destination $mapping.Destination
    $sourceHash = (Get-FileHash -LiteralPath $mapping.Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $mapping.Destination -Algorithm SHA256).Hash
    if ($sourceHash -cne $destinationHash) {
        throw "Legacy package staging changed file bytes: $($mapping.Destination)"
    }
}

$manifest = Get-Content -LiteralPath (Join-Path $packageRoot 'package.json') -Raw | ConvertFrom-Json
$instructions = @"
Unity Editor Dark Mode $($manifest.version) - legacy Assets package

Copy the Assets directory into the root of a Unity project, then restart the Editor.
The Editor/x86_64 path gives Unity 2017 and Unity 2018 the editor-only, x64 native
plug-in defaults without relying on Package Manager or assembly-definition support.

This package intentionally omits .asmdef and .meta files. The target Unity version
will generate compatible metadata. Do not install it alongside the UPM package or
another UnityEditorDarkMode.dll.

Supported operating systems: Windows 10 1903+ x64 and Windows 11 x64.
"@
[System.IO.File]::WriteAllText(
    (Join-Path $resolvedOutputRoot 'README.txt'),
    $instructions,
    [System.Text.UTF8Encoding]::new($false))

if (Get-ChildItem -LiteralPath $resolvedOutputRoot -Recurse -File | Where-Object { $_.Extension -in @('.asmdef', '.meta') }) {
    throw 'The legacy Assets package must not contain assembly definitions or version-specific Unity metadata.'
}

Write-Output "Staged legacy Assets package $($manifest.version) at $resolvedOutputRoot"
