# Changelog

## 1.2.0-preview.3

- Restore the process-wide dark app mode during late package initialization and flush cached native menu themes so Unity dropdown menus use dark styling.

## 1.2.0-preview.2

- Initialize the native plug-in from a managed Editor bootstrap after Package Manager registration so UPM installs can theme the existing Unity window on its owning UI thread.
- Keep the package DLL non-preloaded while preserving standalone DLL startup behavior.

## 1.2.0-preview.1

- Package the Windows Editor native plug-in as a versioned Unity Package Manager Git dependency.
