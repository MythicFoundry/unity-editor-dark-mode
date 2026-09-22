# Full-window coverage experiment

This folder preserves the September 22, 2026 experimental build that applied
the dark theme to every top-level window owned by `Unity.exe`.

That broad rule also reached native popup-menu windows (`#32768`). It produced
the alternate Unity menu appearance shown during Seaborn testing. The effect
was visually coherent, but it replaced menu styling that the existing plugin
already handled, so it is intentionally not used by the main build.

Contents:

- `UnityEditorDarkMode.full-window-theme.cpp` is the exact source snapshot.
- `UnityEditorDarkMode.full-window-theme.dll` is the matching x64 Release DLL.

The main implementation keeps the new dialog, modal, cross-thread-window, and
standard-control coverage while explicitly leaving `#32768` popup menus to the
plugin's existing menu path.
