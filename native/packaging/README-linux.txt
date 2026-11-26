TraceLab for Linux (x86-64)
===========================

Built on Rocky Linux 8 with Qt 5.15, so it runs on Rocky/RHEL/Alma 8 and
newer (and other distributions with glibc 2.28 or later).

Run
---
Unzip anywhere and start the launcher:

    ./TraceLab/TraceLab                 (or: ./TraceLab/TraceLab file.sgy)

The launcher uses the Qt libraries in TraceLab/lib and TraceLab/plugins.
The machine needs the usual desktop libraries (X11/xcb, OpenGL, fontconfig,
freetype). On a minimal Rocky install:

    sudo dnf install libxkbcommon-x11 xcb-util-wm xcb-util-image \
        xcb-util-keysyms xcb-util-renderutil mesa-libGL fontconfig

Settings are kept in ~/.segyread_settings. Computed datasets are written to
the system temporary folder (segyread_scratch).
