#!/usr/bin/env bash
# Linux build wrapper around CMake (see CMakeLists.txt). Builds segytest,
# segybench, segyviewer and gen_fixture.
#
#   ./build.sh                 build (Ubuntu/Debian dev: Qt6; Rocky/RHEL 8: Qt5)
#   ./build.sh --install-deps  first install build dependencies (needs root/sudo)
#   ./build.sh --clean         wipe this platform's build directory first
#
# Output directory, so the two toolchains never share a CMake cache:
#   Ubuntu/Debian/other -> build-linux/             (Qt6 preferred, Qt5 fallback)
#   Rocky/RHEL/CentOS   -> build-linux-compatible/  (Qt5, the "runs on older
#                                                    glibc" build)
# Override with BUILD_DIR=... ; force a Qt major with SEGY_QT_MAJOR=5|6.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

INSTALL_DEPS=0
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --install-deps) INSTALL_DEPS=1 ;;
        --clean) CLEAN=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

OS_ID=""
if [ -f /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    OS_ID="${ID:-}"
fi

case "$OS_ID" in
    rocky|rhel|centos|almalinux) FAMILY=rhel ;;
    ubuntu|debian) FAMILY=debian ;;
    *) FAMILY=other ;;
esac
echo "Detected OS: ${OS_ID:-unknown} (family: $FAMILY)"

SUDO=""
if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then SUDO="sudo"; fi

if [ "$INSTALL_DEPS" -eq 1 ]; then
    if [ "$FAMILY" = rhel ]; then
        $SUDO yum install -y epel-release
        $SUDO yum groupinstall -y "Development Tools"
        $SUDO yum install -y cmake3 qt5-qtbase-devel qt5-qtsvg-devel gcc-toolset-12 \
            libxkbcommon-x11 mesa-libGL-devel
    elif [ "$FAMILY" = debian ]; then
        $SUDO apt update
        $SUDO apt install -y build-essential cmake qt6-base-dev libqt6svg6-dev libgl1-mesa-dev
    else
        echo "Unknown OS -- install a C++17 compiler, cmake >= 3.16 and Qt (5.15+ or 6) yourself." >&2
    fi
fi

if [ "$FAMILY" = rhel ]; then
    # Newer compiler than RHEL 8's stock GCC 8, when present (gcc-toolset-12).
    for tool in /opt/rh/gcc-toolset-12/enable /opt/rh/gcc-toolset-13/enable; do
        if [ -f "$tool" ]; then
            # shellcheck disable=SC1090
            . "$tool"
            echo "Using $(basename "$(dirname "$tool")")"
            break
        fi
    done
    DEFAULT_DIR="build-linux-compatible"
    DEFAULT_QT=5
else
    DEFAULT_DIR="build-linux"
    DEFAULT_QT=""
fi
BUILD_DIR="${BUILD_DIR:-$DEFAULT_DIR}"
QT_MAJOR="${SEGY_QT_MAJOR:-$DEFAULT_QT}"

if command -v cmake3 >/dev/null 2>&1; then
    CMAKE=cmake3
elif command -v cmake >/dev/null 2>&1; then
    CMAKE=cmake
else
    echo "ERROR: cmake not found (try ./build.sh --install-deps)." >&2
    exit 1
fi
echo "Using $($CMAKE --version | head -1), build dir: $BUILD_DIR"

if [ "$CLEAN" -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi

$CMAKE -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release ${QT_MAJOR:+-DSEGY_QT_MAJOR=$QT_MAJOR}
$CMAKE --build "$BUILD_DIR" -j"$(nproc)"

echo
echo "All builds succeeded. Binaries are in $ROOT/$BUILD_DIR (run $BUILD_DIR/segyviewer)."
