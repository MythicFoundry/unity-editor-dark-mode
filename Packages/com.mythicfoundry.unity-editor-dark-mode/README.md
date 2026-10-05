# Unity Editor Dark Mode

This package contains the Windows x64 native plug-in and a managed Editor bootstrap for Unity 2018.1 through Unity 6. The bootstrap also compiles on Unity 2017.1-2017.4, selects only APIs available in the current Unity generation, and initializes the plug-in on Unity's main Editor thread; it is not included in player builds. Unity 2017 projects must use the repository's legacy `Assets` distribution instead of this UPM layout.

Add this Git dependency to your project's `Packages/manifest.json` (use a published tag):

```json
"com.mythicfoundry.unity-editor-dark-mode": "https://github.com/MythicFoundry/unity-editor-dark-mode.git?path=/Packages/com.mythicfoundry.unity-editor-dark-mode#v1.2.0-preview.13"
```

Older Package Manager versions, including versions shipped with Unity 2018, may not support Git dependencies that select a package subfolder with `?path=`. If that URL is rejected, copy this package directory into the project's `Packages` directory as an embedded package. The UPM assembly definition contains only fields supported by Unity 2018.1.

For Unity 2017.1-2017.4, run `scripts/Stage-LegacyAssetsPackage.ps1` from the source repository after staging the native DLL, then copy its generated `Assets` directory into the project. That distribution omits assembly definitions and version-specific metadata and uses the legacy `Editor/x86_64` importer convention.

Restart Unity after upgrading because the initialized native module stays pinned for callback safety until the Editor process exits. Do not install this package alongside another copy of `UnityEditorDarkMode.dll` in `Assets/Plugins`; remove the old copy as part of the migration.

The bundled `Editor/UnityEditorDarkMode.dll.ini` supplies the default colors. Package Manager controls the package cache, so don't rely on editing files there for per-project customization. See the [source repository](https://github.com/MythicFoundry/unity-editor-dark-mode) for the full configuration and implementation notes.
