#!/bin/bash
# Builds a real, self-contained AppImage from an already-built zephyrftp,
# using linuxdeploy + linuxdeploy-plugin-qt. Every step here exists
# because a real, disposable-container test caught it missing — not
# guessed at in advance:
#
# - fontconfig, harfbuzz, and freetype are bundled explicitly
#   (--library=) because linuxdeploy-plugin-qt's automatic dependency
#   scan (ldd-based) misses them — Qt reaches them via dlopen at
#   runtime, not a direct link-time dependency, so a fresh, completely
#   unrelated distro (no Qt/libssh2/libsecret installed at all) failed
#   to even start without them, one missing library at a time.
# - The GL/EGL/GLX/OpenGL stack (mesa-libEGL, mesa-libGL,
#   libglvnd-opengl, libglvnd-glx) is DELIBERATELY NOT bundled — it has
#   to match the host's actual GPU driver stack (proprietary NVIDIA vs.
#   Mesa vs. whatever else), and every real desktop already has some
#   working version of it (needed for the desktop environment's own
#   rendering) or this AppImage wouldn't be running on a graphical
#   session in the first place. Confirmed directly: a minimal container
#   missing these is not representative of a real desktop; installing
#   just those five packages (nothing else) was enough to make the
#   AppImage run cleanly on a system with zero Qt/libssh2/libsecret.
# - fontconfig's own CONFIGURATION (/etc/fonts/fonts.conf + actual font
#   files) is similarly relied on from the host, not bundled — same
#   reasoning as the GL stack: no real desktop lacks it, and bundling a
#   fixed set of fonts would silently override the user's own choices
#   for no real benefit.
# - The "offscreen" Qt platform plugin is bundled manually because
#   linuxdeploy-plugin-qt only auto-bundles platform plugins it detects
#   as actually loaded during a normal (xcb) run — "offscreen" is a
#   headless-testing-only backend real end users never select, but this
#   project's own test/CI story runs everything via
#   QT_QPA_PLATFORM=offscreen, so it needs to be usable the same way
#   here too.
#
# Usage: tools/build-appimage.sh <build-dir>
# Assumes `cmake --build <build-dir> --target zephyrftp` already ran.
# Produces ZephyrFTP-<version>-x86_64.AppImage in the current directory.
set -euo pipefail

BUILD_DIR="${1:?usage: build-appimage.sh <build-dir>}"
LINUXDEPLOY_VERSION=1-alpha-20251107-1
LINUXDEPLOY_QT_VERSION=1-alpha-20250213-1

rm -rf AppDir
mkdir -p AppDir
cmake --install "$BUILD_DIR" --prefix AppDir/usr

if [ ! -x linuxdeploy-x86_64.AppImage ]; then
    wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage"
    chmod +x linuxdeploy-x86_64.AppImage
fi
if [ ! -x linuxdeploy-plugin-qt-x86_64.AppImage ]; then
    wget -q "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${LINUXDEPLOY_QT_VERSION}/linuxdeploy-plugin-qt-x86_64.AppImage"
    chmod +x linuxdeploy-plugin-qt-x86_64.AppImage
fi

# --appimage-extract-and-run instead of relying on FUSE — not available
# (or not worth the extra privileges) inside most CI/container
# environments this needs to run in.
export APPIMAGE_EXTRACT_AND_RUN=1

FONTCONFIG=$(find / -xdev -name 'libfontconfig.so.1' 2>/dev/null | head -1)
HARFBUZZ=$(find / -xdev -name 'libharfbuzz.so.0' 2>/dev/null | head -1)
FREETYPE=$(find / -xdev -name 'libfreetype.so.6' 2>/dev/null | head -1)
: "${FONTCONFIG:?libfontconfig.so.1 not found on this build system}"
: "${HARFBUZZ:?libharfbuzz.so.0 not found on this build system}"
: "${FREETYPE:?libfreetype.so.6 not found on this build system}"

./linuxdeploy-x86_64.AppImage --appdir AppDir --plugin qt \
    --library="$FONTCONFIG" --library="$HARFBUZZ" --library="$FREETYPE" \
    -d resources/zephyrftp.desktop -i resources/icons/app-icon-256.png

OFFSCREEN=$(find / -xdev -name 'libqoffscreen.so' -path '*/plugins/*' 2>/dev/null | head -1)
: "${OFFSCREEN:?libqoffscreen.so not found on this build system}"
cp "$OFFSCREEN" AppDir/usr/plugins/platforms/

./linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage
