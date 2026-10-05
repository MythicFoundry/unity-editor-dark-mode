param(
    [string]$BootstrapPath = (Join-Path $PSScriptRoot '../Packages/com.mythicfoundry.unity-editor-dark-mode/Editor/UnityEditorDarkModeBootstrap.cs')
)

$ErrorActionPreference = 'Stop'
$bootstrapSource = Get-Content -LiteralPath $BootstrapPath -Raw
$scenarios = @(
    [pscustomobject]@{ Name = 'Unity 2019.1'; Defines = @('UNITY_EDITOR_WIN'); PublicWorkerApi = $false; ExperimentalWorkerApi = $false },
    [pscustomobject]@{ Name = 'Unity 2019.2'; Defines = @('UNITY_EDITOR_WIN'); PublicWorkerApi = $false; ExperimentalWorkerApi = $false },
    [pscustomobject]@{ Name = 'Unity 2019.3'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER'); PublicWorkerApi = $false; ExperimentalWorkerApi = $true },
    [pscustomobject]@{ Name = 'Unity 2019.4'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER'); PublicWorkerApi = $false; ExperimentalWorkerApi = $true },
    [pscustomobject]@{ Name = 'Unity 2020.1'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER'); PublicWorkerApi = $false; ExperimentalWorkerApi = $true },
    [pscustomobject]@{ Name = 'Unity 2020.2'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER', 'UNITY_2020_2_OR_NEWER'); PublicWorkerApi = $true; ExperimentalWorkerApi = $false },
    [pscustomobject]@{ Name = 'Unity 2020.3'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER', 'UNITY_2020_2_OR_NEWER'); PublicWorkerApi = $true; ExperimentalWorkerApi = $false },
    [pscustomobject]@{ Name = 'Unity 2021.3'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER', 'UNITY_2020_2_OR_NEWER'); PublicWorkerApi = $true; ExperimentalWorkerApi = $false },
    [pscustomobject]@{ Name = 'Unity 2022.3'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER', 'UNITY_2020_2_OR_NEWER'); PublicWorkerApi = $true; ExperimentalWorkerApi = $false },
    [pscustomobject]@{ Name = 'Unity 2023.2'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER', 'UNITY_2020_2_OR_NEWER'); PublicWorkerApi = $true; ExperimentalWorkerApi = $false },
    [pscustomobject]@{ Name = 'Unity 6'; Defines = @('UNITY_EDITOR_WIN', 'UNITY_2019_3_OR_NEWER', 'UNITY_2020_2_OR_NEWER'); PublicWorkerApi = $true; ExperimentalWorkerApi = $false }
)

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) "UnityEditorDarkMode-Bootstrap-$([guid]::NewGuid().ToString('N'))"
[System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
try {
    foreach ($scenario in $scenarios) {
        $publicWorkerMethod = if ($scenario.PublicWorkerApi) {
            'public static bool IsAssetImportWorkerProcess() => false;'
        }
        else {
            [string]::Empty
        }
        $experimentalWorkerType = if ($scenario.ExperimentalWorkerApi) {
            @'
namespace UnityEditor.Experimental
{
    public static class AssetDatabaseExperimental
    {
        public static bool IsAssetImportWorkerProcess() => false;
    }
}
'@
        }
        else {
            [string]::Empty
        }
        $stubSource = @"
namespace UnityEngine
{
    public static class Application
    {
        public static bool isBatchMode => false;
    }

    public static class Debug
    {
        public static void LogException(System.Exception exception) { }
        public static void LogError(object message) { }
    }
}

namespace UnityEditor
{
    [System.AttributeUsage(System.AttributeTargets.Method)]
    public sealed class InitializeOnLoadMethodAttribute : System.Attribute { }

    public static class AssetDatabase
    {
        $publicWorkerMethod
    }

    public static class EditorApplication
    {
        public static double timeSinceStartup => 0.0;
        public static event System.Action update { add { } remove { } }
        public static event System.Action quitting { add { } remove { } }
    }
}

$experimentalWorkerType
"@
        $scenarioDirectory = Join-Path $temporaryRoot ($scenario.Name -replace '[^A-Za-z0-9.-]', '-')
        [System.IO.Directory]::CreateDirectory($scenarioDirectory) | Out-Null
        $sourcePath = Join-Path $scenarioDirectory 'BootstrapCompatibility.cs'
        [System.IO.File]::WriteAllText(
            $sourcePath,
            "$stubSource`n$bootstrapSource",
            [System.Text.UTF8Encoding]::new($false))
        $defineOption = "/define:$($scenario.Defines -join ';')"
        $command = @"
`$ErrorActionPreference = 'Stop'
Add-Type -Path '$($sourcePath.Replace("'", "''"))' -CompilerOptions '$defineOption'
"@
        $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
        & (Get-Process -Id $PID).Path -NoLogo -NoProfile -EncodedCommand $encodedCommand
        if ($LASTEXITCODE -ne 0) {
            throw "$($scenario.Name) managed bootstrap compatibility compilation failed."
        }
    }
}
finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output "Managed bootstrap compatibility compilation passed for $($scenarios.Count) Unity version profiles."
