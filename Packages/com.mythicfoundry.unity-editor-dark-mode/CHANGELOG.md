# Changelog

## 1.2.0-preview.6

- Defer all Windows, UxTheme, hook, and subclass work until explicit initialization outside `DllMain`, and add owner-thread shutdown before Unity assembly reload or Editor exit.
- Refresh immersive color policy and cached menu themes when Windows theme settings change.
- Add dark painting for checkboxes, radio buttons, group boxes, trackbars, hotkey controls, and text labels.
- Theme unrecognized descendants only inside verified Windows common file dialogs so new shell-host classes inherit dark Explorer styling without affecting Unity custom-drawn controls.

## 1.2.0-preview.5

- Theme the complete in-process Windows common file-dialog navigation hierarchy, including its `WorkerW` background host.
- Apply common-dialog edit styling and item-view selection styling without replacing the Explorer-themed shell chrome.
- Add an automated `IFileDialog` shell-hierarchy regression harness.

## 1.2.0-preview.4

- Register the process-wide in-context WinEvent callback from its native module so Unity dialogs created on auxiliary UI threads receive dark title-bar and control styling.
- Add an automated worker-thread `#32770` dialog regression harness.

## 1.2.0-preview.3

- Restore the process-wide dark app mode during late package initialization and flush cached native menu themes so Unity dropdown menus use dark styling.

## 1.2.0-preview.2

- Initialize the native plug-in from a managed Editor bootstrap after Package Manager registration so UPM installs can theme the existing Unity window on its owning UI thread.
- Keep the package DLL non-preloaded while preserving standalone DLL startup behavior.

## 1.2.0-preview.1

- Package the Windows Editor native plug-in as a versioned Unity Package Manager Git dependency.
