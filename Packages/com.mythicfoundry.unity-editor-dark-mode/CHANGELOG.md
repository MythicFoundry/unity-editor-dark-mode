# Changelog

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
