#!/bin/bash
# Substitute for windeployqt when cross-compiling with Fedora's mingw64
# toolchain (windeployqt itself isn't packaged for that toolchain, and
# even the upstream Qt one only runs on Windows/under wine, not as a
# cross tool). windeployqt's actual job is two things: (1) copy every
# DLL a set of .exes needs, transitively, and (2) copy the Qt plugin
# DLLs (platform, styles, image formats, ...) into the subdirectories
# Qt's plugin loader expects relative to the exe. This does both by
# walking objdump -p output instead, since that's the same information
# windeployqt itself works from.
#
# Usage: tools/collect-win-runtime.sh <build-dir>
# Copies every runtime DLL next to every *.exe found directly in
# <build-dir>, plus a platforms/ (and other plugin-kind) subdirectory
# next to each exe.
set -euo pipefail

BUILD_DIR="${1:?usage: collect-win-runtime.sh <build-dir>}"
MINGW_SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw
MINGW_BIN="$MINGW_SYSROOT/bin"
MINGW_PLUGINS="$MINGW_SYSROOT/lib/qt6/plugins"
OBJDUMP=x86_64-w64-mingw32-objdump

# Plugins actually exercised by this app: qwindows (real Windows
# rendering) + qoffscreen (headless, matches QT_QPA_PLATFORM=offscreen
# used throughout this project's own test suite) for platforms;
# qsvgicon/qsvg because IconTheme renders vendored SVG icons; the
# native style so the app doesn't fall back to Qt's generic Fusion
# look; all three TLS backends since FtpBackend now drives FTPS
# (explicit AUTH TLS) through QSslSocket and which backend Qt picks at
# runtime isn't this script's call to make.
PLUGIN_DLLS=(
    platforms/qwindows.dll
    platforms/qoffscreen.dll
    iconengines/qsvgicon.dll
    imageformats/qsvg.dll
    styles/qmodernwindowsstyle.dll
    tls/qopensslbackend.dll
    tls/qschannelbackend.dll
    tls/qcertonlybackend.dll
)

# Resolves the transitive closure of vendor DLLs (mingw sysroot bin/)
# a set of PE files depend on, printing one absolute path per line.
# Anything not found in the sysroot is assumed to be a genuine Windows
# system DLL (kernel32, user32, d3d11, ...) present on any real
# Windows machine, and is silently skipped rather than treated as an
# error — there is nothing to bundle for those.
resolve_closure() {
    local -A seen=()
    local queue=("$@")
    while [ ${#queue[@]} -gt 0 ]; do
        local f="${queue[0]}"
        queue=("${queue[@]:1}")
        local key="${f,,}"
        [ -n "${seen[$key]:-}" ] && continue
        seen[$key]=1
        local match
        match=$(find "$MINGW_BIN" -maxdepth 1 -iname "$f" | head -1)
        [ -z "$match" ] && continue
        echo "$match"
        local dep
        for dep in $("$OBJDUMP" -p "$match" | awk '/DLL Name:/{print $3}'); do
            queue+=("$dep")
        done
    done
}

shopt -s nullglob
exes=("$BUILD_DIR"/*.exe)
shopt -u nullglob
if [ ${#exes[@]} -eq 0 ]; then
    echo "collect-win-runtime.sh: no .exe files directly under $BUILD_DIR" >&2
    exit 1
fi

# Seed the closure from every exe's own direct imports plus every
# plugin DLL's direct imports, so e.g. libssl-3-x64.dll (only pulled
# in by tls/qopensslbackend.dll, not by any exe directly) is still
# included.
direct_deps=()
for exe in "${exes[@]}"; do
    while IFS= read -r dll; do
        direct_deps+=("$dll")
    done < <("$OBJDUMP" -p "$exe" | awk '/DLL Name:/{print $3}')
done
for p in "${PLUGIN_DLLS[@]}"; do
    plugin_path="$MINGW_PLUGINS/$p"
    [ -f "$plugin_path" ] || continue
    while IFS= read -r dll; do
        direct_deps+=("$dll")
    done < <("$OBJDUMP" -p "$plugin_path" | awk '/DLL Name:/{print $3}')
done

mapfile -t vendor_dlls < <(resolve_closure "${direct_deps[@]}" | sort -u)

echo "Vendor DLLs to bundle (${#vendor_dlls[@]}):"
printf '  %s\n' "${vendor_dlls[@]##*/}"

for exe in "${exes[@]}"; do
    dest_dir=$(dirname "$exe")
    echo "-- Populating $dest_dir for $(basename "$exe")"
    for dll in "${vendor_dlls[@]}"; do
        cp -u "$dll" "$dest_dir/"
    done
    for p in "${PLUGIN_DLLS[@]}"; do
        src="$MINGW_PLUGINS/$p"
        [ -f "$src" ] || continue
        kind_dir="$dest_dir/$(dirname "$p")"
        mkdir -p "$kind_dir"
        cp -u "$src" "$kind_dir/"
    done
done

echo "Done. Each .exe's directory now carries its own runtime DLLs + plugins."
