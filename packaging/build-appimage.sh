#!/usr/bin/env bash
#
# Build a self-contained AppImage for Mailo.
#
# Prerequisites (not installed by this script):
#   - A working build toolchain + all of Mailo's build deps (Qt6, KPim6*, qtkeychain).
#   - Internet access on first run to download the linuxdeploy tools into ./tools.
#   - FUSE (for the resulting AppImage to run; not needed to build it).
#
# Usage:
#   packaging/build-appimage.sh            # release build, downloads tools if missing
#   OUTPUT=mailo.AppImage packaging/build-appimage.sh
#
# The heavy lifting is done by linuxdeploy + its Qt plugin, but three things
# need manual help because the plugin does not cover them for this app:
#   1. QtWebEngine       — helper process, ICU data, *.pak resources, locales.
#   2. org.kde.desktop   — the QtQuick Controls style the app forces in main.cpp,
#                          plus Kirigami and the QQC2 desktop implementation.
#   3. Breeze icons      — the UI references named icons (mail-attachment, …).
#
set -euo pipefail

# --- paths ---------------------------------------------------------------
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # project root
build_dir="${BUILD_DIR:-$here/build-appimage}"
appdir="$here/AppDir"
tools_dir="$here/tools"
output="${OUTPUT:-Mailo-x86_64.AppImage}"
jobs="${JOBS:-$(nproc)}"

# Where Qt keeps its bits on this distro. Override if autodetection is wrong.
qml_dir="${QML_DIR:-/usr/lib/x86_64-linux-gnu/qt6/qml}"
qt_libexec="${QT_LIBEXEC:-/usr/lib/qt6/libexec}"
qt_resources="${QT_RESOURCES:-/usr/share/qt6/resources}"
qt_translations="${QT_TRANSLATIONS:-/usr/share/qt6/translations}"

# linuxdeploy-plugin-qt queries qmake; the bare `qmake` wrapper may point at
# Qt5, so force the Qt6 one unless the caller overrides it.
export QMAKE="${QMAKE:-qmake6}"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# --- 1. fetch tooling ----------------------------------------------------
mkdir -p "$tools_dir"
fetch() { # url dest
  local url="$1" dest="$2"
  if [[ ! -x "$dest" ]]; then
    log "Downloading $(basename "$dest")"
    curl -fL# "$url" -o "$dest"
    chmod +x "$dest"
  fi
}
base="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
qtbase="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous"
fetch "$base/linuxdeploy-x86_64.AppImage"                 "$tools_dir/linuxdeploy"
fetch "$qtbase/linuxdeploy-plugin-qt-x86_64.AppImage"     "$tools_dir/linuxdeploy-plugin-qt"
export PATH="$tools_dir:$PATH"

# --- 2. configure + build + install into AppDir --------------------------
log "Configuring (Release)"
cmake -S "$here" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr

log "Building"
# mailo-docs too: `install` pulls in the gzipped man page, and building only
# the mailo target left it missing so the install step below failed.
cmake --build "$build_dir" --parallel "$jobs" --target mailo mailo-docs

log "Installing into AppDir"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build_dir" --component "" >/dev/null

# --- 3. bundle the pieces linuxdeploy-plugin-qt misses -------------------
# 3a. QtWebEngine helper process + data. It must live next to the Qt libexec
#     path the loader expects; we place it under usr/libexec and point to it.
log "Bundling QtWebEngine"
install -Dm755 "$qt_libexec/QtWebEngineProcess" "$appdir/usr/libexec/QtWebEngineProcess"
mkdir -p "$appdir/usr/resources" "$appdir/usr/translations"
cp -a "$qt_resources/." "$appdir/usr/resources/" 2>/dev/null || true
# WebEngine locales (.pak per language) live under translations/qtwebengine_locales
if [[ -d "$qt_translations/qtwebengine_locales" ]]; then
  cp -a "$qt_translations/qtwebengine_locales" "$appdir/usr/translations/"
fi

# 3b. The org.kde.desktop QtQuick Controls style + Kirigami + QQC2 impl.
#     linuxdeploy-plugin-qt scans imports it can see, but the style is loaded
#     by string at runtime (QQuickStyle::setStyle) so copy the trees wholesale.
log "Bundling KDE QML style + Kirigami"
dest_qml="$appdir/usr/lib/x86_64-linux-gnu/qt6/qml"
mkdir -p "$dest_qml/org/kde"
for mod in org/kde/desktop org/kde/kirigami org/kde/kirigamiaddons; do
  if [[ -d "$qml_dir/$mod" ]]; then
    mkdir -p "$dest_qml/$(dirname "$mod")"
    cp -a "$qml_dir/$mod" "$dest_qml/$(dirname "$mod")/"
  fi
done

# 3c. The SVG *icon engine*. Breeze ships SVGs, and rendering them as icons
#     needs iconengines/libqsvgicon.so — imageformats/libqsvg.so is a different
#     plugin and does not cover it. linuxdeploy-plugin-qt bundles the latter
#     but not the former, so every named icon in the UI came out as an empty
#     square no matter how the theme was configured.
log "Bundling the SVG icon engine"
for plugin_root in /usr/lib/x86_64-linux-gnu/qt6/plugins /usr/lib/qt6/plugins; do
  if [[ -d "$plugin_root/iconengines" ]]; then
    mkdir -p "$appdir/usr/plugins/iconengines"
    cp -a "$plugin_root/iconengines/." "$appdir/usr/plugins/iconengines/"
    break
  fi
done

# 3d. Breeze icon theme so named icons in the UI actually render.
log "Bundling Breeze icons"
for base_theme in /usr/share/icons/breeze /usr/share/icons/breeze-dark; do
  if [[ -d "$base_theme" ]]; then
    dest="$appdir/usr/share/icons/$(basename "$base_theme")"
    mkdir -p "$dest"
    cp -a "$base_theme/." "$dest/"
  fi
done

# --- 4. runtime hook: env for the bundled Qt/WebEngine/style -------------
# linuxdeploy runs apprun-hooks/*.sh before launching the app.
log "Writing AppRun hooks"
hooks="$appdir/apprun-hooks"
mkdir -p "$hooks"
cat > "$hooks/mailo-env.sh" <<'HOOK'
#!/bin/bash
here="$(dirname "$(readlink -f "$0")")"
# WebEngine sandbox needs a userns; AppImages often run where it's unavailable.
export QTWEBENGINE_DISABLE_SANDBOX=1
export QTWEBENGINEPROCESS_PATH="$here/usr/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$here/usr/resources"
export QTWEBENGINE_LOCALES_PATH="$here/usr/translations/qtwebengine_locales"
# Force the bundled KDE style; without a KDE session it would fall back to Basic.
export QT_QUICK_CONTROLS_STYLE="org.kde.desktop"
export QML2_IMPORT_PATH="$here/usr/lib/x86_64-linux-gnu/qt6/qml${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"
# Prefer the bundled Breeze icons.
export XDG_DATA_DIRS="$here/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
HOOK
chmod +x "$hooks/mailo-env.sh"

# --- 5. deploy Qt + pack the AppImage ------------------------------------
log "Running linuxdeploy + Qt plugin"
# EXTRA_QT_MODULES ensures WebEngine/QuickControls2 libs are pulled even if the
# import scanner can't see them. QML_SOURCES_PATHS points the Qt plugin at our
# QML so it can trace imports.
export EXTRA_QT_MODULES="waylandcompositor"   # harmless if unused; helps on wayland
export QML_SOURCES_PATHS="$here/src/qml"
"$tools_dir/linuxdeploy" \
  --appdir "$appdir" \
  --plugin qt \
  --desktop-file "$appdir/usr/share/applications/org.mailo.Mailo.desktop" \
  --icon-file "$appdir/usr/share/icons/hicolor/scalable/apps/org.mailo.Mailo.svg" \
  --output appimage

# linuxdeploy names it from the desktop file; normalise to $output.
produced="$(ls -t Mailo*.AppImage 2>/dev/null | head -1 || true)"
if [[ -n "$produced" && "$produced" != "$output" ]]; then
  mv -f "$produced" "$output"
fi
log "Done: $output"
