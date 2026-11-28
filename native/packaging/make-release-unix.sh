#!/usr/bin/env bash
# Linux and macOS release of exFAT Image Builder: the counterpart of
# make-release.ps1 (Windows). Public edition only: a private build (Sony tools,
# fpkg-cli, YouTube packages) is made on Windows and never here.
#
#   packaging/make-release-unix.sh --build-dir <configured+built tree> [--out-dir <dir>] [--tools-dir <dir>]
#                                  [--skip-smoke]
#
# Linux (x86_64 / aarch64): <out>/exFAT-Image-Builder-linux-<arch>.AppImage and
#   exFAT-Image-Builder-linux-<arch>.tar.gz (the same files as a plain folder:
#   for systems without FUSE, and the portable edition: put portable.txt next
#   to the AppImage or inside the folder).
#   Needs linuxdeploy, linuxdeploy-plugin-qt, appimagetool and the AppImage
#   runtime in --tools-dir (pinned by SHA-256 below; fetch-appimage-tools
#   downloads them when the directory lacks them).
# macOS (arm64 / x86_64): <out>/exFAT-Image-Builder-macos-<arch>.dmg holding
#   "exFAT Image Builder.app" (macdeployqt, ad-hoc signed: not notarised).
#
# Both: LICENCE.txt, THIRD_PARTY_NOTICES.md (public edition), LICENSES/, the
# bundled engines (pins checked against native/cmake), a private-file guard,
# a --self-test of the packaged program with its own data folders, and
# <out>/SHA256SUMS-<platform>.txt (merge the lines into the release's one
# SHA256SUMS.txt; the update check reads that file).
set -euo pipefail

BUILD_DIR=""; OUT_DIR=""; TOOLS_DIR=""; SKIP_SMOKE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --tools-dir) TOOLS_DIR="$2"; shift 2 ;;
        --skip-smoke) SKIP_SMOKE=1; shift ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$BUILD_DIR" ] || { echo "--build-dir is required" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
NATIVE="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$NATIVE/.." && pwd)"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
OUT_DIR="${OUT_DIR:-$REPO/dist-unix}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
step() { echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

case "$(uname -s)" in
    Linux) PLATFORM=linux ;;
    Darwin) PLATFORM=macos ;;
    *) die "This script builds the Linux and macOS releases; use make-release.ps1 on Windows" ;;
esac
ARCH="$(uname -m)"
[ "$PLATFORM" = macos ] && [ "$ARCH" = aarch64 ] && ARCH=arm64
[ "$PLATFORM" = linux ] && [ "$ARCH" = arm64 ] && ARCH=aarch64
# The universal macOS app is not built: one per processor, like the Backport engine.
if [ "$PLATFORM" = macos ]; then
    archs="$(sed -n 's/^CMAKE_OSX_ARCHITECTURES:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" || true)"
    [ -n "$archs" ] && ARCH="$archs"
    case "$ARCH" in arm64|x86_64) ;; *) die "CMAKE_OSX_ARCHITECTURES must be one processor (arm64 or x86_64), not '$ARCH'" ;; esac
fi
NAME="exFAT-Image-Builder-$PLATFORM-$ARCH"

# <seconds> <command...>: the command, stopped after that long (macOS has no timeout(1)).
with_timeout() {
    local secs="$1"; shift
    "$@" &
    local pid=$!
    ( sleep "$secs"; kill -TERM "$pid" 2>/dev/null ) &
    local watchdog=$!
    local rc=0
    wait "$pid" || rc=$?
    kill "$watchdog" 2>/dev/null || true
    wait "$watchdog" 2>/dev/null || true
    return $rc
}

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# ---- the edition's texts and engines -----------------------------------------------
# Writes LICENCE.txt, THIRD_PARTY_NOTICES.md (the <!-- public-edition --> parts
# kept, their markers removed) and LICENSES/ into $1.
put_notices() {
    local dest="$1"
    mkdir -p "$dest/LICENSES"
    cp "$NATIVE/packaging/LICENCE.txt" "$dest/LICENCE.txt"
    grep -q '<!-- public-edition' "$NATIVE/packaging/THIRD_PARTY_NOTICES-native.md" \
        || die "THIRD_PARTY_NOTICES-native.md has no <!-- public-edition --> part"
    grep -v -E '^<!-- /?public-edition[^>]*-->[[:space:]]*$' "$NATIVE/packaging/THIRD_PARTY_NOTICES-native.md" \
        | tr -d '\r' | cat -s > "$dest/THIRD_PARTY_NOTICES.md"
    cp -R "$REPO/LICENSES/." "$dest/LICENSES/"
    cp -R "$NATIVE/packaging/licenses/." "$dest/LICENSES/"
}

pinned_hash() { # <cmake file> <variable>: the first pin of that name
    sed -n "s/^[[:space:]]*set($2 \"\([0-9a-f]\{64\}\)\").*/\1/p" "$NATIVE/cmake/$1" | head -1
}

# The engines CMake bundled (bin/engines): the AMPR runtime always, the
# Backport engine for this platform (cmake/BackportEngine.cmake). A release
# without them would show "engine missing", so both are required.
check_engines() {
    local engines="$BUILD_DIR/bin/engines"
    local sprx="$engines/ampr-emu-ng/libSceAmpr.sprx"
    [ -f "$sprx" ] || die "Missing from the build: $sprx"
    local want; want="$(pinned_hash AmprRuntime.cmake FW_AMPR_RUNTIME_SHA256)"
    [ "$(sha256 "$sprx")" = "$want" ] || die "$sprx is not the pinned ampr-emu-ng runtime"
    [ -d "$engines/backport" ] && [ -n "$(ls -A "$engines/backport" 2>/dev/null)" ] \
        || die "The PS5 Backport engine is missing from $engines/backport (configure with -DFW_FETCH_BACKPORT_ENGINE=ON or -DFW_BACKPORT_ENGINE_EXE=<archive>)"
    # Its archive was checked against the pin when CMake unpacked it; the
    # stamp it leaves names the hash it checked.
    if [ -f "$engines/backport/.fw-engine-sha256" ]; then
        local stamp; stamp="$(tr -d ' \r\n' < "$engines/backport/.fw-engine-sha256")"
        grep -q "$stamp" "$NATIVE/cmake/BackportEngine.cmake" || die "The bundled Backport engine ($stamp) is not pinned in BackportEngine.cmake"
    fi
}

# What only a private build may carry (make-release.ps1's private-files.ps1):
# the private tools folders, and wherever they are the files those folders hold.
guard_private() {
    local root="$1" found
    found="$(cd "$root" && find . \( -path './tools/fpkg-toolkit' -o -path '*/tools/fpkg-toolkit' -o -path '*/tools/fpkg-cli' \
        -o -path '*/tools/youtube' -o -iname 'prospero-pub-cmd.exe' -o -iname 'fpkg-cli.exe' -o -iname 'fpkg-cli' \
        -o -iname 'libscepubtools.dll' -o -iname '*.pkg' \) -print | head -20)"
    [ -z "$found" ] || die "Private files in a public build ($root):
$found"
}

# --self-test of the packaged program, with its own data folders and the
# Backport engine's folders moved away from the user's.
smoke() {
    local program="$1" round="$2" box
    box="$(mktemp -d "${TMPDIR:-/tmp}/fw-release-smoke-XXXXXX")"
    local out rc=0
    out="$(cd "$box" && export FW_DATA_DIR="$box/data" FW_OFFLINE=1 FW_NO_POWER_ACTIONS=1         D97BETA_CONFIG_DIR="$box/backport-config" D97BETA_DATA_DIR="$box/backport-data" D97BETA_ENGINE_HOME="$box/backport-engine-home"         QT_QPA_PLATFORM=offscreen APPIMAGE_EXTRACT_AND_RUN=1 HOME="$box/home" && mkdir -p "$HOME" && with_timeout 180 "$program" --self-test)" || rc=$?
    rm -rf "$box"
    [ $rc -eq 0 ] || die "Self-test ($round) failed with exit code $rc"
    echo "$out" | grep -q "\"version\": *\"$VERSION\"" || die "Self-test ($round) reported another version: $out"
    echo "   $round: self-test passed"
}

VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD_DIR/CMakeCache.txt")"
[ -n "$VERSION" ] || die "No project version in $BUILD_DIR/CMakeCache.txt"
step "exFAT Image Builder $VERSION, $PLATFORM $ARCH"
check_engines

if [ "$PLATFORM" = linux ]; then
    # ---- Linux: AppDir → AppImage + tar.gz -------------------------------------------
    TOOLS_DIR="${TOOLS_DIR:-$BUILD_DIR/_appimage-tools}"
    # Pinned tools (x86_64; an aarch64 build pins its own the same way).
    LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-$ARCH.AppImage"
    LINUXDEPLOY_SHA256_x86_64="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
    QTPLUGIN_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-$ARCH.AppImage"
    QTPLUGIN_SHA256_x86_64="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724" # GitHub publishes no digest for this release: hash of the download of 28 Nov 2026 (12,317,424 bytes, as listed)
    APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-$ARCH.AppImage"
    APPIMAGETOOL_SHA256_x86_64="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
    RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-$ARCH"
    RUNTIME_SHA256_x86_64="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
    RUNTIME_SHA256_aarch64="00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444"
    mkdir -p "$TOOLS_DIR"
    fetch() { # <url> <file> <sha256>
        local url="$1" file="$TOOLS_DIR/$2" want="$3"
        [ -n "$want" ] && [ "${want#@}" = "$want" ] || die "No pinned SHA-256 for $2 on $ARCH"
        if [ ! -f "$file" ] || [ "$(sha256 "$file")" != "$want" ]; then
            echo "   downloading $2"
            curl -fsSL --retry 3 -o "$file.part" "$url"
            [ "$(sha256 "$file.part")" = "$want" ] || { rm -f "$file.part"; die "$2 does not match its pinned SHA-256"; }
            mv "$file.part" "$file"
        fi
        chmod +x "$file"
    }
    eval "fetch \"\$LINUXDEPLOY_URL\" linuxdeploy-$ARCH.AppImage \"\${LINUXDEPLOY_SHA256_$ARCH:-}\""
    eval "fetch \"\$QTPLUGIN_URL\" linuxdeploy-plugin-qt-$ARCH.AppImage \"\${QTPLUGIN_SHA256_$ARCH:-}\""
    eval "fetch \"\$APPIMAGETOOL_URL\" appimagetool-$ARCH.AppImage \"\${APPIMAGETOOL_SHA256_$ARCH:-}\""
    eval "fetch \"\$RUNTIME_URL\" runtime-$ARCH \"\${RUNTIME_SHA256_$ARCH:-}\""
    export APPIMAGE_EXTRACT_AND_RUN=1 # the tools are AppImages too; no FUSE needed on the build machine

    QMAKE="$(sed -n 's/^QT_QMAKE_EXECUTABLE:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [ -z "$QMAKE" ]; then
        qtdir="$(sed -n 's/^Qt6_DIR:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")"
        QMAKE="$(cd "$qtdir/../../.." && pwd)/bin/qmake"
    fi
    [ -x "$QMAKE" ] || die "qmake of the Qt the build used was not found ($QMAKE)"
    export QMAKE
    # Both window systems: X11 and Wayland; nothing Qt Quick.
    export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
    export EXTRA_QT_MODULES="waylandclient;svg"

    step "AppDir"
    APPDIR="$OUT_DIR/$NAME.AppDir"
    rm -rf "$APPDIR"
    mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
    cp "$BUILD_DIR/bin/exFAT Image Builder" "$APPDIR/usr/bin/exfat-image-builder"
    cp "$NATIVE/packaging/linux/exfat-image-builder.desktop" "$APPDIR/usr/share/applications/"
    cp "$NATIVE/src/app/resources/app/app-256.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/exfat-image-builder.png"
    # Shipped files beside the program (Paths::resourceDir() is the program's folder on Linux).
    cp -R "$BUILD_DIR/bin/engines" "$APPDIR/usr/bin/engines"
    put_notices "$APPDIR/usr/bin"
    # Portable mode: portable.txt beside the AppImage file, not inside it (the
    # program's folder in a running AppImage is read-only).
    mkdir -p "$APPDIR/apprun-hooks"
    cat > "$APPDIR/apprun-hooks/fw-portable.sh" <<'HOOK'
# exFAT Image Builder: the AppImage file the user starts and its folder
# (portable.txt is looked for there; a restart starts the AppImage again,
# because this mount goes away when the app ends).
if [ -n "${APPIMAGE:-}" ]; then
    export FW_LAUNCHER_DIR="$(dirname "$APPIMAGE")"
    export FW_LAUNCHER_EXE="$APPIMAGE"
fi
HOOK
    "$TOOLS_DIR/linuxdeploy-$ARCH.AppImage" --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/exfat-image-builder" \
        --desktop-file "$APPDIR/usr/share/applications/exfat-image-builder.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/exfat-image-builder.png" \
        --plugin qt
    guard_private "$APPDIR"

    step "AppImage"
    rm -f "$OUT_DIR/$NAME.AppImage"
    ARCH="$ARCH" "$TOOLS_DIR/appimagetool-$ARCH.AppImage" --no-appstream --runtime-file "$TOOLS_DIR/runtime-$ARCH" \
        "$APPDIR" "$OUT_DIR/$NAME.AppImage"
    chmod +x "$OUT_DIR/$NAME.AppImage"

    step "Folder edition (tar.gz)"
    FOLDER="$OUT_DIR/$NAME"
    rm -rf "$FOLDER"
    cp -a "$APPDIR" "$FOLDER"
    rm -rf "$FOLDER/apprun-hooks"
    cat > "$FOLDER/exfat-image-builder" <<'RUN'
#!/bin/sh
# exFAT Image Builder, folder edition: a portable.txt in this folder keeps the
# settings and logs in ./data instead of your home folder.
here="$(cd "$(dirname "$0")" && pwd)"
export FW_LAUNCHER_DIR="$here" FW_LAUNCHER_EXE="$here/exfat-image-builder"
exec "$here/AppRun" "$@"
RUN
    chmod +x "$FOLDER/exfat-image-builder"
    cp "$NATIVE/packaging/linux/README-Linux.txt" "$FOLDER/README.txt"
    (cd "$OUT_DIR" && tar -czf "$NAME.tar.gz" "$NAME")

    if [ $SKIP_SMOKE -eq 0 ]; then
        step "Smoke test"
        smoke "$OUT_DIR/$NAME.AppImage" "AppImage"
        smoke "$FOLDER/exfat-image-builder" "folder edition"
    fi
    rm -rf "$APPDIR" "$FOLDER"
    ASSETS=("$NAME.AppImage" "$NAME.tar.gz")
else
    # ---- macOS: .app → .dmg ----------------------------------------------------------
    MACDEPLOYQT="$(sed -n 's/^Qt6_DIR:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt")"
    MACDEPLOYQT="$(cd "$MACDEPLOYQT/../../.." && pwd)/bin/macdeployqt"
    [ -x "$MACDEPLOYQT" ] || die "macdeployqt of the Qt the build used was not found ($MACDEPLOYQT)"
    step "App bundle"
    STAGE="$OUT_DIR/$NAME"
    rm -rf "$STAGE"
    mkdir -p "$STAGE"
    APP="$STAGE/exFAT Image Builder.app"
    cp -R "$BUILD_DIR/bin/exFAT Image Builder.app" "$APP"
    # Shipped files in Contents/Resources (Paths::resourceDir()): code
    # signing allows no data files next to the executable.
    RES="$APP/Contents/Resources"
    ditto "$BUILD_DIR/bin/engines" "$RES/engines" # keeps the engine .app's links and modes
    put_notices "$RES"
    "$MACDEPLOYQT" "$APP" -always-overwrite
    guard_private "$APP"
    # Ad-hoc signature: required to run on Apple Silicon at all. It is not a
    # Developer ID: macOS asks the user to allow the app once (README).
    codesign --force --deep --sign - --timestamp=none "$APP"
    codesign --verify --deep --strict "$APP"
    if [ $SKIP_SMOKE -eq 0 ]; then
        step "Smoke test"
        smoke "$APP/Contents/MacOS/exFAT Image Builder" "app bundle"
    fi
    step "Disk image"
    ln -s /Applications "$STAGE/Applications"
    cp "$NATIVE/packaging/macos/README-macOS.txt" "$STAGE/README.txt"
    rm -f "$OUT_DIR/$NAME.dmg"
    hdiutil create -volname "exFAT Image Builder" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$OUT_DIR/$NAME.dmg" >/dev/null
    rm -rf "$STAGE"
    ASSETS=("$NAME.dmg")
fi

step "Checksums"
SUMS="$OUT_DIR/SHA256SUMS-$PLATFORM-$ARCH.txt"
: > "$SUMS"
for a in "${ASSETS[@]}"; do
    printf '%s  %s\n' "$(sha256 "$OUT_DIR/$a")" "$a" >> "$SUMS"
    echo "   $OUT_DIR/$a ($(du -h "$OUT_DIR/$a" | cut -f1))"
done
echo "   $SUMS (add these lines to the release's SHA256SUMS.txt)"
