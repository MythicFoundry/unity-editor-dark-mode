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
$bootstrapCompatibilityTestPath = Join-Path $repoRoot 'tests/Test-BootstrapCompatibility.ps1'
$legacyStagerPath = Join-Path $repoRoot 'scripts/Stage-LegacyAssetsPackage.ps1'

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
if ($manifest.unity -cne '2018.1') {
    throw "Package minimum Unity version $($manifest.unity) does not match the UPM layout minimum 2018.1."
}

foreach ($requiredFile in @($dllPath, $metaPath, $configPath, "$configPath.meta", $bootstrapPath, "$bootstrapPath.meta", $assemblyDefinitionPath, "$assemblyDefinitionPath.meta", (Join-Path $packageRoot 'LICENSE.md'), $legacyStagerPath)) {
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
    $bootstrap -cnotmatch '#if UNITY_2018_2_OR_NEWER\s+return Application\.isBatchMode;\s+#else(?s:.*?)Environment\.GetCommandLineArgs\(\)(?s:.*?)StringComparison\.OrdinalIgnoreCase(?s:.*?)#endif' -or
    $bootstrap -cnotmatch '#if UNITY_2018_1_OR_NEWER\s+EditorApplication\.quitting -= Shutdown;\s+EditorApplication\.quitting \+= Shutdown;\s+#endif' -or
    $bootstrap -cnotmatch '#if UNITY_2020_2_OR_NEWER\s+return AssetDatabase\.IsAssetImportWorkerProcess\(\);' -or
    $bootstrap -cnotmatch '#elif UNITY_2019_3_OR_NEWER\s+return UnityEditor\.Experimental\.AssetDatabaseExperimental\.IsAssetImportWorkerProcess\(\);' -or
    $bootstrap -cnotmatch '#else\s+return false;\s+#endif' -or
    $bootstrap -cnotmatch 'EntryPoint\s*=\s*"UnityEditorDarkMode_Initialize"' -or
    $bootstrap -cnotmatch 'EntryPoint\s*=\s*"UnityEditorDarkMode_Shutdown"' -or
    $bootstrap -cnotmatch 'EditorApplication\.quitting') {
    throw 'The managed Editor bootstrap must avoid unavailable legacy APIs, initialize outside batch mode and Asset Import Workers, and shut down the native plug-in when the Editor exposes a quit callback.'
}
if ($bootstrap -cmatch 'AssemblyReloadEvents\.beforeAssemblyReload') {
    throw 'The managed Editor bootstrap must keep the pinned native plug-in active across managed assembly reloads.'
}
& $bootstrapCompatibilityTestPath -BootstrapPath $bootstrapPath

$assemblyDefinition = Get-Content -LiteralPath $assemblyDefinitionPath -Raw | ConvertFrom-Json
$assemblyDefinitionPropertyNames = @($assemblyDefinition.PSObject.Properties.Name)
$unsupportedAssemblyDefinitionProperties = @(
    $assemblyDefinitionPropertyNames |
        Where-Object { $_ -cnotin @('name', 'references', 'includePlatforms', 'excludePlatforms') }
)
if ($assemblyDefinition.name -cne 'MythicFoundry.UnityEditorDarkMode.Editor' -or
    $unsupportedAssemblyDefinitionProperties.Count -ne 0 -or
    $assemblyDefinitionPropertyNames -ccontains 'excludePlatforms' -or
    @($assemblyDefinition.includePlatforms).Count -ne 1 -or
    $assemblyDefinition.includePlatforms[0] -cne 'Editor') {
    throw 'The managed bootstrap assembly must be Editor-only and use only the assembly-definition fields supported by Unity 2017.3.'
}

$temporaryBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$legacyTemporaryRoot = Join-Path $temporaryBase "UnityEditorDarkMode-Legacy-$([guid]::NewGuid().ToString('N'))"
if (-not ([System.IO.Path]::GetFullPath($legacyTemporaryRoot)).StartsWith($temporaryBase, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Legacy package test path escaped the temporary directory: $legacyTemporaryRoot"
}
try {
    & $legacyStagerPath -OutputRoot $legacyTemporaryRoot

    $legacyEditorRoot = Join-Path $legacyTemporaryRoot 'Assets/Editor/UnityEditorDarkMode'
    $legacyNativeRoot = Join-Path $legacyEditorRoot 'x86_64'
    $legacyMappings = @(
        [pscustomobject]@{ Source = $bootstrapPath; Destination = Join-Path $legacyEditorRoot 'UnityEditorDarkModeBootstrap.cs' },
        [pscustomobject]@{ Source = $dllPath; Destination = Join-Path $legacyNativeRoot 'UnityEditorDarkMode.dll' },
        [pscustomobject]@{ Source = $configPath; Destination = Join-Path $legacyNativeRoot 'UnityEditorDarkMode.dll.ini' },
        [pscustomobject]@{ Source = Join-Path $packageRoot 'LICENSE.md'; Destination = Join-Path $legacyTemporaryRoot 'LICENSE.md' }
    )
    foreach ($mapping in $legacyMappings) {
        if (-not (Test-Path -LiteralPath $mapping.Destination -PathType Leaf)) {
            throw "Legacy Assets package file missing: $($mapping.Destination)"
        }
        if ((Get-FileHash -LiteralPath $mapping.Source -Algorithm SHA256).Hash -cne
            (Get-FileHash -LiteralPath $mapping.Destination -Algorithm SHA256).Hash) {
            throw "Legacy Assets package file does not match its canonical source: $($mapping.Destination)"
        }
    }
    $legacyMetadataFiles = @(
        Get-ChildItem -LiteralPath $legacyTemporaryRoot -Recurse -File |
            Where-Object { $_.Extension -in @('.asmdef', '.meta') }
    )
    if (-not (Test-Path -LiteralPath (Join-Path $legacyTemporaryRoot 'README.txt') -PathType Leaf) -or
        $legacyMetadataFiles.Count -ne 0) {
        throw 'The legacy Assets package must include its installation instructions and omit assembly definitions and version-specific Unity metadata.'
    }
}
finally {
    if (Test-Path -LiteralPath $legacyTemporaryRoot) {
        Remove-Item -LiteralPath $legacyTemporaryRoot -Recurse -Force
    }
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
    $source -cnotmatch 'IsFileDialogShellManagedControl\(hWnd\)' -or
    $source -cnotmatch 'kCommonFileDialogProperty' -or
    $source -cnotmatch 'IsCommonFileDialogWindow\(hWnd\)') {
    throw 'The native plug-in must theme the common file-dialog shell background, native controls, edit controls, and item view.'
}
$shellManagedThemeBranch = [regex]::Match(
    $source,
    '(?s)else if \(IsCommonFileDialogWindow\(hWnd\) && IsFileDialogShellManagedControl\(hWnd\)\) \{(?<Body>.*?)\r?\n\s*\}')
if (-not $shellManagedThemeBranch.Success -or
    $shellManagedThemeBranch.Groups['Body'].Value -cmatch 'SetWindowTheme') {
    throw 'The native plug-in must leave shell-managed common-file-dialog controls on the themes assigned by Windows.'
}
if ($source -cmatch 'ColorizeFileDialogNavigationControl' -or
    $source -cmatch 'PaintFileDialogSelectionIndicator' -or
    $source -cmatch 'kFileDialogSelectedRowProperty' -or
    $source -cmatch '#pragma comment\(lib, "msimg32\.lib"\)') {
    throw 'The native plug-in must not replace shell themes, rewrite rendered navigation pixels, or synthesize selection geometry.'
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
