# DarkMode Mod for Unity Editor on Windows
 <a style="text-decoration:none" href="https://github.com/MythicFoundry/unity-editor-dark-mode/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/MythicFoundry/unity-editor-dark-mode/ci.yml?style=flat-square" alt="Build Status" /></a>
<a style="text-decoration:none" href="https://assetstore.unity.com/packages/slug/281842">
<img src="https://img.shields.io/badge/Unity%20AssetStore-Download-orange.svg?style=flat-square" alt="AssetStore Link" />
</a>

A fully working runtime dark mode mod for Unity Editor on Windows with:
- Dark title bar
- Dark menu bar
- Dark context menu
- Dark Unity-owned native dialogs and progress windows
- Dark standard controls, including buttons, labels, edit fields, lists, trees, tabs, tooltips, and progress bars

> This runtime mod works on Windows 11 and Windows 10 1903+. Tested on Unity 2019, 2020, 2021, 2022, 2023 and Unity 6.

![Screenshot](screenshot.jpg?raw=true)

## Unity Package Manager installation

The Windows Editor plug-in is available as `com.mythicfoundry.unity-editor-dark-mode` from a version tag in this repository. After the corresponding tag has been published, add this entry to your Unity project's `Packages/manifest.json` dependencies:

```json
"com.mythicfoundry.unity-editor-dark-mode": "https://github.com/MythicFoundry/unity-editor-dark-mode.git?path=/Packages/com.mythicfoundry.unity-editor-dark-mode#v1.2.0-preview.6"
```

The package includes the native DLL, Windows Editor-only importer settings, and a managed bootstrap that loads and initializes the DLL after Package Manager registration on Unity's main Editor thread. Remove any existing `UnityEditorDarkMode.dll` under `Assets/Plugins` before installing the package; loading both copies is unsupported. Restart Unity after upgrading because the initialized native module stays pinned for callback safety until the Editor process exits.

Git dependencies contain files committed at the tag. A DLL uploaded as a GitHub Release asset alone is not included in the Unity package dependency.

## Easy installation guide
- Download the `UnityEditorDarkMode.unitypackage` from Unity [AssetStore](https://assetstore.unity.com/packages/slug/281842) or GitHub [Releases](https://github.com/0x7c13/UnityEditor-DarkMode/releases) and double click to install it to your Unity project.

  > **WARNING:** If you feel uncomfortable downloading a malicious Unity Package from a stranger like me, then you should not:) Take a look at later sections to see how it works and how to build it yourself if you prefer. Please do your own homework and make your own judgement. I offer this approach as a convenience only.
- Restart Unity Editor and you are done!
- Now enjoy the immersive dark mode in Unity Editor!

## Manual installation guide
- Download the `UnityEditorDarkMode.dll` from [releases](https://github.com/0x7c13/UnityEditor-DarkMode/releases)

  > **WARNING:** If you feel uncomfortable downloading a malicious DLL from a stranger like me, then you should not:) Take a look at later sections to see how it works and how to build it yourself if you prefer. Please do your own homework and make your own judgement. I offer this approach as a convenience for those who don't want to build a C++ project themselves.
- Copy the DLL into your Unity project and apply below settings to the DLL in the Unity Editor inspector:

    ![dll-setting](screenshot-dll-setting.png?raw=true)
    
  - Make sure `Load on startup` is checked which will make the DLL to be loaded on Unity Editor startup.
  - Make sure `OS` is set to `Windows` which will make the DLL to be loaded only on Windows OS.
  - Make sure only `Editor` is checked which will make the DLL to be loaded only in the Unity Editor.

- Restart Unity Editor and you are done!
- Now enjoy the immersive dark mode in Unity Editor!

## What if you don't want to add the DLL to your project?
Put the DLL outside of your project and add a Unity Editor script to your project like below. Injection tools that only load the DLL are not sufficient because loader-lock-safe startup requires an explicit call to `UnityEditorDarkMode_Initialize`.

    ```C#
    #if UNITY_EDITOR_WIN // Windows only, obviously
    namespace Editor.Theme // Change this to your own namespace you like or simply remove it
    {
        using System.Runtime.InteropServices;
        using UnityEditor;

        public static class UnityEditorDarkMode
        {
            // Change below path to the path of the downloaded dll
            [DllImport(@"C:\Users\<...>\Desktop\UnityEditorDarkMode.dll", EntryPoint = "UnityEditorDarkMode_Initialize")]
            [return: MarshalAs(UnmanagedType.Bool)]
            private static extern bool _();

            [InitializeOnLoadMethod]
            private static void __()
            {
                #if !UNITY_2021_1_OR_NEWER
                // [InitializeOnLoadMethod] is getting called before the editor
                // main window is created on earlier versions of Unity, so we
                // need to wait a bit here before attaching the dll.
                System.Threading.Thread.Sleep(100);
                #endif
                _(); // Attach the dll to the Unity Editor
            }
        }
    }
    #endif
    ```

## How to change the theme?
After first launch, a `UnityEditorDarkMode.dll.ini` file will be created in the same directory as the dll. You can modify the values in this file to change the theme (Restart the editor after changing the values). Default values are given below:
```ini
menubar_textcolor = 200,200,200
menubar_textcolor_disabled = 160,160,160
menubar_bgcolor = 48,48,48
menubaritem_bgcolor = 48,48,48
menubaritem_bgcolor_hot = 62,62,62
menubaritem_bgcolor_selected = 62,62,62
dialog_textcolor = 210,210,210
dialog_textcolor_disabled = 145,145,145
dialog_bgcolor = 48,48,48
control_bgcolor = 58,58,58
control_bgcolor_hot = 72,72,72
control_bgcolor_pressed = 42,42,42
control_bordercolor = 96,96,96
progress_bgcolor = 64,64,64
progress_barcolor = 58,121,187
log_unknown_windows = false
```

Existing INI files remain compatible. Any missing dialog or control color uses the default shown above. Set `log_unknown_windows = true` to emit unhandled Unity child-window class names through `OutputDebugString`; this is intended only for diagnosing new Unity or Windows versions.

## How to remove it?
Remove the DLL from your project and restart Unity Editor (You need to close the editor before deleting the DLL).

## How to build it?
- Make sure latest `CMake`, `Visual Studio` and `MSVC toolchain` are installed on your system. Then run below command in the project directory:

    ```cmd
    cmake -B build && cmake --build build --config Release
    ```
    > NOTE: You may need to add `cmake` to your system path if you haven't already.

- The `UnityEditorDarkMode.dll` will be created under the `build\Release` directory after the build finishes successfully.

## How to prepare a UPM release

1. Update the version in `Packages/com.mythicfoundry.unity-editor-dark-mode/package.json` and its `CHANGELOG.md`.
2. Run `pwsh -File scripts/Stage-UpmPackage.ps1` to build and copy the DLL into the package folder. The script validates the package and its Windows x64 DLL. If CMake is not on `PATH`, pass its executable path with `-CMake`.
3. Commit the source and staged package together. Push the commit and wait for CI to pass.
4. Create and push a tag named `v<package-version>` on that commit. The tag workflow validates that the tag matches `package.json`, checks that the native source builds, and publishes a GitHub Release with the packaged DLL attached. Unity fetches the DLL from the tagged repository contents, not from the release attachment.

Do not tag a version until its native UI behavior has been visually checked in Unity. Upgrading from a preloaded package version requires a Unity restart; obtain permission before restarting someone else's Editor session.

## How it works?
This project is basically a stripped down version of [ReaperThemeHackDll](https://github.com/jjYBdx4IL/ReaperThemeHackDll) made by [jjYBdx4IL](https://github.com/jjYBdx4IL) with some minor modifications. If you like this project, please consider giving a star to the `ReaperThemeHackDll` project as well. Actually, his code with some minor modifications can be used to theme any legacy Windows applications that uses the Win32 title bar, menu bar, context menu, etc.

Ok, so what I have done on top of `ReaperThemeHackDll` is:
- Remove all the unnecessary dependencies of Reaper framework and plugin code since we don't need them for Unity Editor hack.
- Discover top-level windows owned by the current Unity process instead of relying only on `UnityContainerWndClass`. Known standard controls are themed with class-specific handling, while unknown custom-drawn child controls are left to Unity.
- Use a synchronous CBT hook for Unity's main UI thread plus a process-filtered WinEvent hook for windows created on other Unity UI threads. Cross-thread windows are subclassed on their owning thread.
- Apply the Windows immersive-dark-mode attributes and private UxTheme opt-in dynamically, then custom-paint legacy dialog backgrounds, text, buttons, and progress bars where Windows does not provide a complete dark style.
- `UnityContainerWndClass` remains the window class name of Unity Editor's main window. You can inspect a window class with the following Win32 API:
    ```C#
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hWnd, char[] lpClassName, int nMaxCount);
    ```
- `UnityContainerWndClass` has remained consistent across Unity 2019 through Unity 6, but modal and progress-window classes vary. Process ownership and runtime class inspection allow those windows to be covered without hard-coding every top-level class.

  > **NOTE:** If you do this, it basically means this hack can be used for any Windows application that uses the default white Win32 title bar, menu bar, context menu, etc.
- A different color preset is given by default which I think looks better with Unity Editor.
- Some inrelevent code is also removed and some minor modifications are made to make it more performant and clean. You don't have to do it tho so I am not going to explain them here.
- Keep `DllMain` limited to recording the module handle and disabling thread callbacks. The managed bootstrap calls explicit initialize and shutdown exports outside the Windows loader lock, and initialization pins the native module so callbacks can never target unloaded code.

## Known issues
- The DLL can theme native windows and controls hosted by `Unity.exe`. It cannot theme Unity Hub, crash handlers, browsers, version-control clients, or other external processes.
- Windows-owned file, folder, credential, and UAC dialogs ultimately follow the Windows version and system theme. The plugin opts eligible in-process common dialogs into dark mode, but cannot guarantee every shell surface.
- IMGUI and UI Toolkit content is rendered by Unity. Unity's built-in editor skin is normally already dark; custom editor extensions with hard-coded light colors must be fixed in those extensions.
- Very early startup UI shown before Unity loads preloaded native plugins cannot be changed by project installation. Launch-time DLL injection can cover more of that phase.
