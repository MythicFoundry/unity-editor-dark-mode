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
$bootstrapPath = Join-Path $packageRoot 'Editor/UnityEditorDarkModeBootstrap.cs'
$assemblyDefinitionPath = Join-Path $packageRoot 'Editor/MythicFoundry.UnityEditorDarkMode.Editor.asmdef'
$definitionPath = Join-Path $repoRoot 'UnityEditorDarkMode.def'
$sourcePath = Join-Path $repoRoot 'UnityEditorDarkMode.cpp'

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

foreach ($requiredFile in @($dllPath, $metaPath, $configPath, "$configPath.meta", $bootstrapPath, "$bootstrapPath.meta", $assemblyDefinitionPath, "$assemblyDefinitionPath.meta", (Join-Path $packageRoot 'LICENSE.md'))) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required package file missing: $requiredFile"
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $packageRoot 'Editor.meta') -PathType Leaf)) {
    throw 'The Editor folder must have a committed .meta file.'
}
foreach ($asset in (Get-ChildItem -LiteralPath $packageRoot -Recurse -File | Where-Object Name -NotLike '*.meta')) {
    if (-not (Test-Path -LiteralPath "$($asset.FullName).meta" -PathType Leaf)) {
        throw "Package asset has no committed .meta file: $($asset.FullName)"
    }
}

$meta = Get-Content -LiteralPath $metaPath -Raw
if ($meta -cnotmatch '(?m)^  isPreloaded: 0\r?$' -or
    $meta -cnotmatch '(?ms)Editor: Editor\s+second:\s+enabled: 1\s+settings:\s+CPU: AnyCPU\s+DefaultValueInitialized: true\s+OS: Windows' -or
    $meta -cnotmatch '(?ms)Standalone: Win64\s+second:\s+enabled: 0') {
    throw 'The native plug-in importer must be non-preloaded and enabled only in the Windows Editor.'
}

$bootstrap = Get-Content -LiteralPath $bootstrapPath -Raw
if ($bootstrap -cnotmatch '\[InitializeOnLoadMethod\]' -or
    $bootstrap -cnotmatch 'AssetDatabase\.IsAssetImportWorkerProcess\(\)' -or
    $bootstrap -cnotmatch 'EntryPoint\s*=\s*"UnityEditorDarkMode_Initialize"') {
    throw 'The managed Editor bootstrap must initialize the native plug-in outside batch mode and Asset Import Workers.'
}

$assemblyDefinition = Get-Content -LiteralPath $assemblyDefinitionPath -Raw | ConvertFrom-Json
if ($assemblyDefinition.name -cne 'MythicFoundry.UnityEditorDarkMode.Editor' -or
    $assemblyDefinition.rootNamespace -cne 'MythicFoundry.UnityEditorDarkMode' -or
    @($assemblyDefinition.includePlatforms).Count -ne 1 -or
    $assemblyDefinition.includePlatforms[0] -cne 'Editor') {
    throw 'The managed bootstrap assembly must be Editor-only and use the MythicFoundry.UnityEditorDarkMode root namespace.'
}

$definition = Get-Content -LiteralPath $definitionPath -Raw
if ($definition -cnotmatch '(?m)^\s*UnityEditorDarkMode_Initialize(?:\s+@\d+)?\s*\r?$') {
    throw 'The native module definition must export UnityEditorDarkMode_Initialize.'
}

$source = Get-Content -LiteralPath $sourcePath -Raw
if ($source -cnotmatch 'MAKEINTRESOURCEA\(136\)' -or
    $source -cnotmatch '(?s)static void RefreshDarkMenuThemes\(\).*?g_setPreferredAppMode\(PreferredAppMode::ForceDark\);.*?g_flushMenuThemes\(\);' -or
    ([regex]::Matches($source, 'RefreshDarkMenuThemes\(\);')).Count -ne 1 -or
    $source -cnotmatch '(?s)UnityEditorDarkMode_Initialize\(\).*?RefreshDarkMenuThemes\(\);') {
    throw 'The late native initializer must restore ForceDark mode and flush cached native menu themes.'
}
if ($source -cnotmatch '(?s)SetWinEventHook\(\s*EVENT_OBJECT_SHOW,\s*EVENT_OBJECT_SHOW,\s*g_module,\s*WindowEventProc,\s*g_processId,\s*0,\s*WINEVENT_INCONTEXT\)') {
    throw 'The process-wide in-context WinEvent hook must identify the native plug-in module.'
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

$module = [System.Runtime.InteropServices.NativeLibrary]::Load($dllPath)
try {
    $initializeExport = [IntPtr]::Zero
    if (-not [System.Runtime.InteropServices.NativeLibrary]::TryGetExport($module, 'UnityEditorDarkMode_Initialize', [ref]$initializeExport) -or
        $initializeExport -eq [IntPtr]::Zero) {
        throw 'The packaged DLL does not export UnityEditorDarkMode_Initialize.'
    }
}
finally {
    [System.Runtime.InteropServices.NativeLibrary]::Free($module)
}

Write-Output "UPM package $($manifest.name) $($manifest.version) is valid."
