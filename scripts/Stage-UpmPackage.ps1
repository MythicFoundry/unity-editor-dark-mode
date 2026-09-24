param(
    [string]$CMake = 'cmake'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repoRoot 'build'
$builtDll = Join-Path $buildDirectory 'Release/UnityEditorDarkMode.dll'
$packagedDll = Join-Path $repoRoot 'Packages/com.mythicfoundry.unity-editor-dark-mode/Editor/UnityEditorDarkMode.dll'

& $CMake -S $repoRoot -B $buildDirectory
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& $CMake --build $buildDirectory --config Release --target UnityEditorDarkMode
if ($LASTEXITCODE -ne 0) { throw 'Native plug-in build failed.' }

if (-not (Test-Path -LiteralPath $builtDll -PathType Leaf)) {
    throw "Built DLL not found: $builtDll"
}

Copy-Item -LiteralPath $builtDll -Destination $packagedDll -Force
& (Join-Path $PSScriptRoot 'Test-UpmPackage.ps1')

Write-Output "Staged $packagedDll"
