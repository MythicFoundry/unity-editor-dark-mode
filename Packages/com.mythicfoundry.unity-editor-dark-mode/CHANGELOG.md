# Changelog

## 1.2.0-preview.12

- Extend the declared minimum Editor version from Unity 2019.1 to Unity 2018.1.
- Add compile-smoke coverage for Unity 2018.1, 2018.2, 2018.3, and 2018.4 while preserving all newer Unity compatibility profiles.
- Keep the Asset Import Worker guard disabled on Unity releases that do not expose either supported worker-process API.

## 1.2.0-preview.11

- Restore the declared Unity 2019.1 compatibility by selecting the available Asset Import Worker API for each Unity generation.
- Keep Unity 2020.2 and newer on the current public API, use the experimental API only on Unity 2019.3 through 2020.1, and avoid unavailable worker APIs on Unity 2019.1 and 2019.2.
- Add compile-smoke coverage for every managed bootstrap compatibility branch through Unity 6.

## 1.2.0-preview.10

- Remove the unsafe common-file-dialog bitmap recoloring and guessed selection overlay introduced in preview.9.
- Preserve native shell themes so navigation buttons, breadcrumbs, search controls, and ClearType text retain their Windows rendering.
- Replace the false-positive pixel regression with checks for retained native themes, semantic item selection, and absence of synthetic selection state.

## 1.2.0-preview.9

- Normalize the Windows common file dialog's navigation and command surfaces when shell controls ignore their assigned dark theme.
- Preserve a visible accent selection row for the DirectUI file list, including inactive and keyboard-moved selection states.
- Upgrade the native `IFileDialog` regression harness to select a real fixture folder and validate rendered header and selection pixels instead of theme handles alone.

## 1.2.0-preview.8

- Keep the pinned native hooks and window subclasses active across managed assembly reloads so Unity's menu bar does not fall back to light Windows painting while managed callbacks run.
- Continue shutting down native callbacks when the Editor quits, and reinitialize idempotently after each managed reload.
- Validate reload continuity in the native lifecycle harness and package validator.

## 1.2.0-preview.7

- Keep owner-drawn, image, frame, and other non-text static controls theme-disabled so Unity progress labels cannot inherit a light Explorer surface.
- Match PowerToys control-theme contracts for checkbox and radio hit testing, custom-painted tab backgrounds, nonstandard buttons, and button control colors.
- Extend the worker-thread dialog regression harness with progress, owner-drawn static, and per-control theme-state coverage.

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
