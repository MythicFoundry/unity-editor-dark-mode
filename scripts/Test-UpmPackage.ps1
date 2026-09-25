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
    $bootstrap -cnotmatch 'EntryPoint\s*=\s*"UnityEditorDarkMode_Initialize"' -or
    $bootstrap -cnotmatch 'EntryPoint\s*=\s*"UnityEditorDarkMode_Shutdown"' -or
    $bootstrap -cnotmatch 'EditorApplication\.quitting') {
    throw 'The managed Editor bootstrap must initialize outside batch mode and Asset Import Workers and shut down the native plug-in when the Editor quits.'
}
if ($bootstrap -cmatch 'AssemblyReloadEvents\.beforeAssemblyReload') {
    throw 'The managed Editor bootstrap must keep the pinned native plug-in active across managed assembly reloads.'
}

$assemblyDefinition = Get-Content -LiteralPath $assemblyDefinitionPath -Raw | ConvertFrom-Json
if ($assemblyDefinition.name -cne 'MythicFoundry.UnityEditorDarkMode.Editor' -or
    $assemblyDefinition.rootNamespace -cne 'MythicFoundry.UnityEditorDarkMode' -or
    @($assemblyDefinition.includePlatforms).Count -ne 1 -or
    $assemblyDefinition.includePlatforms[0] -cne 'Editor') {
    throw 'The managed bootstrap assembly must be Editor-only and use the MythicFoundry.UnityEditorDarkMode root namespace.'
}

$definition = Get-Content -LiteralPath $definitionPath -Raw
if ($definition -cnotmatch '(?m)^\s*UnityEditorDarkMode_Initialize(?:\s+@\d+)?\s*\r?$' -or
    $definition -cnotmatch '(?m)^\s*UnityEditorDarkMode_Shutdown(?:\s+@\d+)?\s*\r?$') {
    throw 'The native module definition must export the initialize and shutdown lifecycle functions.'
}

$source = Get-Content -LiteralPath $sourcePath -Raw
if ($source -cnotmatch 'MAKEINTRESOURCEA\(104\)' -or
    $source -cnotmatch 'MAKEINTRESOURCEA\(136\)' -or
    $source -cnotmatch '(?s)static void RefreshDarkModeState\(\).*?g_refreshImmersiveColorPolicyState\(\);.*?g_setPreferredAppMode\(PreferredAppMode::ForceDark\);.*?g_flushMenuThemes\(\);' -or
    $source -cnotmatch '(?s)UnityEditorDarkMode_Initialize\(\).*?RefreshDarkModeState\(\);' -or
    $source -cnotmatch '(?s)case WM_THEMECHANGED:.*?case WM_SETTINGCHANGE:.*?RefreshDarkModeState\(\);.*?ThemeWindowTree\(hWnd\);') {
    throw 'Native initialization and theme-change handling must refresh immersive policy, restore ForceDark mode, and flush cached menu themes.'
}
$dllMain = [regex]::Match($source, '(?s)BOOL APIENTRY DllMain\(.*\z').Value
if (-not $dllMain -or
    $dllMain -cmatch 'GetCurrentProcessId\(|RegisterWindowMessageW\(|EnableDarkMode\(|GetAllWindowsByProcessID\(|SetWindowsHookExW\(|EnsureWindowEventHook\(|UnhookWinEvent\(|UnhookWindowsHookEx\(|RemoveWindowSubclass\(|CloseThemeData\(') {
    throw 'DllMain must remain loader-lock-safe and defer window, hook, and UxTheme work to explicit lifecycle exports.'
}
if ($source -cnotmatch 'GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS \| GET_MODULE_HANDLE_EX_FLAG_PIN' -or
    $source -cnotmatch '(?s)UnityEditorDarkMode_Shutdown\(\).*?UnhookWinEvent\(g_windowEventHook\).*?UnhookWindowsHookEx\(g_hook\).*?RemoveWindowOnOwningThread\(hWnd\)') {
    throw 'The native module must stay pinned while callbacks can exist and must tear them down explicitly on owner threads.'
}
if ($source -cnotmatch '(?s)SetWinEventHook\(\s*EVENT_OBJECT_SHOW,\s*EVENT_OBJECT_SHOW,\s*g_module,\s*WindowEventProc,\s*g_processId,\s*0,\s*WINEVENT_INCONTEXT\)') {
    throw 'The process-wide in-context WinEvent hook must identify the native plug-in module.'
}
if ($source -cnotmatch 'IsWndClass\(hWnd, L"WorkerW"\)' -or
    $source -cnotmatch '\(style & ES_MULTILINE\) \? L"DarkMode_Explorer" : L"DarkMode_CFD"' -or
    $source -cnotmatch 'SetWindowTheme\(hWnd, L"DarkMode_ItemsView", nullptr\)' -or
    $source -cnotmatch 'kCommonFileDialogProperty' -or
    $source -cnotmatch 'IsCommonFileDialogWindow\(hWnd\)') {
    throw 'The native plug-in must theme the common file-dialog shell background, edit controls, and item selection surface.'
}
foreach ($paintFunction in @('PaintCheckOrRadioButton', 'PaintGroupBox', 'PaintTrackbar', 'PaintHotkeyControl', 'PaintStaticText')) {
    if ($source -cnotmatch "static void $paintFunction\(") {
        throw "The native plug-in is missing specialized dark painting through $paintFunction."
    }
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
    $shutdownExport = [IntPtr]::Zero
    if (-not [System.Runtime.InteropServices.NativeLibrary]::TryGetExport($module, 'UnityEditorDarkMode_Initialize', [ref]$initializeExport) -or
        $initializeExport -eq [IntPtr]::Zero -or
        -not [System.Runtime.InteropServices.NativeLibrary]::TryGetExport($module, 'UnityEditorDarkMode_Shutdown', [ref]$shutdownExport) -or
        $shutdownExport -eq [IntPtr]::Zero) {
        throw 'The packaged DLL does not export both native lifecycle functions.'
    }
}
finally {
    [System.Runtime.InteropServices.NativeLibrary]::Free($module)
}

Write-Output "UPM package $($manifest.name) $($manifest.version) is valid."
