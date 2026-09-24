# Unity Editor Dark Mode

This package contains the Windows x64 native plug-in. Its importer is configured for the Windows Editor only and to load at Editor startup; it is not included in player builds.

Add this Git dependency to your project's `Packages/manifest.json` (use a published tag):

```json
"com.mythicfoundry.unity-editor-dark-mode": "https://github.com/MythicFoundry/unity-editor-dark-mode.git?path=/Packages/com.mythicfoundry.unity-editor-dark-mode#v1.2.0-preview.1"
```

Unity must be restarted after installing or updating a preloaded native plug-in. Do not install this package alongside another copy of `UnityEditorDarkMode.dll` in `Assets/Plugins`; remove the old copy as part of the migration.

The bundled `Editor/UnityEditorDarkMode.dll.ini` supplies the default colors. Package Manager controls the package cache, so don't rely on editing files there for per-project customization. See the [source repository](https://github.com/MythicFoundry/unity-editor-dark-mode) for the full configuration and implementation notes.
