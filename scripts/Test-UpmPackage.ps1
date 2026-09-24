param(
    [string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot 'Packages/com.mythicfoundry.unity-editor-dark-mode'
$manifestPath = Join-Path $packageRoot 'package.json'
$dllPath = Join-Path $packageRoot 'Editor/UnityEditorDarkMode.dll'
$metaPath = "$dllPath.meta"
$configPath = "$dllPath.ini"

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.name -cne 'com.mythicfoundry.unity-editor-dark-mode') {
    throw "Unexpected package name: $($manifest.name)"
}
if ($manifest.version -cnotmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Invalid package version: $($manifest.version)"
}
if ($ExpectedVersion -and $manifest.version -cne $ExpectedVersion) {
    throw "Package version $($manifest.version) does not match expected $ExpectedVersion."
}

foreach ($requiredFile in @($dllPath, $metaPath, $configPath, "$configPath.meta", (Join-Path $packageRoot 'LICENSE.md'))) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required package file missing: $requiredFile"
    }
}

$meta = Get-Content -LiteralPath $metaPath -Raw
if ($meta -cnotmatch '(?m)^  isPreloaded: 1\r?$' -or
    $meta -cnotmatch '(?ms)Editor: Editor\s+second:\s+enabled: 1\s+settings:\s+CPU: AnyCPU\s+DefaultValueInitialized: true\s+OS: Windows' -or
    $meta -cnotmatch '(?ms)Standalone: Win64\s+second:\s+enabled: 0') {
    throw 'The native plug-in importer must preload only in the Windows Editor.'
}

$stream = [System.IO.File]::OpenRead($dllPath)
try {
    $reader = [System.IO.BinaryReader]::new($stream)
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Packaged file is not a PE binary.' }
    $stream.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    if ($peOffset -lt 0x40 -or $peOffset -gt ($stream.Length - 6)) {
        throw 'Packaged DLL has an invalid PE header offset.'
    }
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
        throw 'Packaged DLL must be a Windows x64 PE binary.'
    }
}
finally {
    $stream.Dispose()
}

Write-Output "UPM package $($manifest.name) $($manifest.version) is valid."
