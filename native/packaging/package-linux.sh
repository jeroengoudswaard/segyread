#!/usr/bin/env bash
# Usage (from native/, with Docker): docker build -t segybuild . && #   docker run --rm -v "$PWD:/src:ro" -v "$PWD/..:/out" segybuild bash /src/packaging/package-linux.sh
# Runs inside the segybuild (rockylinux:8, Qt5) container: copies the
# sources, builds, runs the tests and packs a portable folder with the Qt5
# libraries and plugins it needs. /src = native/ (read-only), /out = output.
set -euo pipefail
yum install -y gcc-toolset-12 zip >/dev/null 2>&1 || true
rm -rf /build && mkdir -p /build
cd /src
tar --exclude='./build-*' -cf - . | (cd /build && tar -xf -)
cd /build
sed -i 's/\r$//' build.sh
./build.sh 2>&1 | tail -20
BIN=build-linux-compatible
"$BIN/segytest" | tail -2 || true

PKG=/tmp/TraceLab
rm -rf "$PKG" && mkdir -p "$PKG/lib" "$PKG/plugins"
cp "$BIN/segyviewer" "$PKG/TraceLab.bin"
QTPLUG=$(qmake-qt5 -query QT_INSTALL_PLUGINS 2>/dev/null || echo /usr/lib64/qt5/plugins)
for d in platforms xcbglintegrations imageformats iconengines platforminputcontexts; do
    [ -d "$QTPLUG/$d" ] && cp -r "$QTPLUG/$d" "$PKG/plugins/"
done
# Libraries: Qt and its less common dependencies; the base system (glibc,
# X11/xcb, GL, fontconfig, freetype) is left to the target machine.
collect() {
    ldd "$1" | awk '/=> \//{print $3}' | while read -r lib; do
        case "$(basename "$lib")" in
            libQt5*|libicu*|libdouble-conversion*|libpcre2-16*|libmd4c*|libb2*|libxkbcommon*|libgraphite2*|libharfbuzz*|libpng16*|libzstd*)
                [ -f "$PKG/lib/$(basename "$lib")" ] || cp -L "$lib" "$PKG/lib/" ;;
        esac
    done
}
collect "$PKG/TraceLab.bin"
for so in $(find "$PKG/plugins" -name '*.so'); do collect "$so"; done
for so in "$PKG"/lib/*.so*; do collect "$so"; done
cat > "$PKG/TraceLab" <<'EOS'
#!/bin/sh
# TraceLab launcher: uses the Qt libraries in this folder.
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
exec "$HERE/TraceLab.bin" "$@"
EOS
chmod +x "$PKG/TraceLab" "$PKG/TraceLab.bin"
cp /src/packaging/README-linux.txt "$PKG/README.txt"
cd /tmp && rm -f /out/Shockwave-TraceLab-linux-x64.zip && zip -qr /out/Shockwave-TraceLab-linux-x64.zip TraceLab
# Smoke test: the binary finds every library (offscreen, no display needed).
LD_LIBRARY_PATH="$PKG/lib" ldd "$PKG/TraceLab.bin" | grep "not found" || echo "all libraries found"
ls -la /out/Shockwave-TraceLab-linux-x64.zip
