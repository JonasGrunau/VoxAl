#!/usr/bin/env bash
# Builds the macOS installer from a finished plugin build.
# Usage: packaging/macos/build-pkg.sh <artefacts-dir> <version> <out.pkg>
#   e.g. packaging/macos/build-pkg.sh build/Vocanal_artefacts/Release 1.2.0 dist/Vocanal-1.2.0-macOS.pkg
#
# One component package per format (VST3, AU), wrapped in a product archive
# whose "Customize" page lets the user untick either. The bundles go in as
# they are (sign them first). Set INSTALLER_IDENTITY to a
# "Developer ID Installer: ..." identity to sign the pkg; without it the pkg
# is unsigned.
set -euo pipefail

ARTEFACTS=$1
VERSION=$2
OUT=$3

ID=de.grunau.vocanal
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# component <name> <bundle> <install-location>
component()
{
    local name=$1 bundle=$2 location=$3
    local root="$WORK/root-$name"
    mkdir -p "$root"
    ditto "$bundle" "$root/$(basename "$bundle")"

    # pkgbuild marks bundles relocatable by default: if the user has another
    # copy anywhere on disk, the installer "upgrades" that one instead.
    pkgbuild --analyze --root "$root" "$WORK/$name.plist"
    plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/$name.plist"

    pkgbuild --root "$root" \
             --component-plist "$WORK/$name.plist" \
             --identifier "$ID.$name" \
             --version "$VERSION" \
             --install-location "$location" \
             "$WORK/pkgs/$name.pkg"
}

mkdir -p "$WORK/pkgs"
component vst3 "$ARTEFACTS/VST3/Vocanal.vst3"      /Library/Audio/Plug-Ins/VST3
component au   "$ARTEFACTS/AU/Vocanal.component"   /Library/Audio/Plug-Ins/Components
# STANDALONE (PsyBox): also ship the app, unticked by default (see the choice below).
# component app  "$ARTEFACTS/Standalone/Vocanal.app"   /Applications

# STANDALONE (PsyBox) additions to the XML below:
#   in <choices-outline>:  <line choice="app"/>
#   after the au choice:   <choice id="app" title="Standalone app" description="/Applications/Vocanal.app" start_selected="false">
#                              <pkg-ref id="$ID.app"/>
#                          </choice>
#   after the pkg-refs:    <pkg-ref id="$ID.app" version="$VERSION">app.pkg</pkg-ref>
cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Vocanal $VERSION</title>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <allowed-os-versions><os-version min="11.0"/></allowed-os-versions>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
    </choices-outline>
    <choice id="vst3" title="VST3 plug-in" description="/Library/Audio/Plug-Ins/VST3/Vocanal.vst3">
        <pkg-ref id="$ID.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit" description="/Library/Audio/Plug-Ins/Components/Vocanal.component">
        <pkg-ref id="$ID.au"/>
    </choice>
    <pkg-ref id="$ID.vst3" version="$VERSION">vst3.pkg</pkg-ref>
    <pkg-ref id="$ID.au" version="$VERSION">au.pkg</pkg-ref>
</installer-gui-script>
EOF

sign=()
if [[ -n "${INSTALLER_IDENTITY:-}" ]]; then
    sign=(--sign "$INSTALLER_IDENTITY" --timestamp)
fi

mkdir -p "$(dirname "$OUT")"
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK/pkgs" \
             ${sign[@]+"${sign[@]}"} "$OUT"
