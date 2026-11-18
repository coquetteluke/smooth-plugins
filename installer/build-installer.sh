#!/bin/bash
# Builds Smooth-Plugins-Installer.pkg: the macOS Installer with a checkbox for each plug-in.
#
# Usage: installer/build-installer.sh <folder with the .component files> <output .pkg>
#
# Each plug-in becomes its own small package. The installer opens on the list of plug-ins
# (customize="always"), installs for all users (/Library/Audio/Plug-Ins/Components) or for the
# current user only (~/Library/Audio/Plug-Ins/Components), and refuses Intel Macs and macOS
# older than 13. A plug-in missing from the folder is left out of the list.
set -euo pipefail

src=${1:?usage: build-installer.sh <folder with .component files> <output .pkg>}
out=${2:?usage: build-installer.sh <folder with .component files> <output .pkg>}
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$src" && pwd)

# Order shown in the installer: slug|component name|description
PLUGINS="tune-pro|Tune Pro|Advanced automatic vocal tuning with two engines, MIDI targets and a pitch window.
tune|Tune|Low latency real time vocal pitch correction with formant preservation.
glitter|Glitter|Fast creative vocal tuner with effect modules.
choir-loft|Choir Loft|Up to eight formant preserving harmony voices from one lead vocal.
duo|Duo|Automatic vocal doubling.
alter-ego|Alter Ego|Real time voice changer with independent pitch and formants, characters, vibrato and whisper.
throat|Throat|Reshapes the vocal tract of a voice without moving its pitch.
velvet-rope|Velvet Rope|Finds the narrow peaks that stick out of a voice and turns them down only while they are there.
punch|Punch|Vocal impact: transient shaper, compressors and output ceiling.
key|Key|Live key, scale and tempo detection.
cutup|Cutup|Vocal sample slicer played from MIDI (an instrument).
tape-color|Tape Color|Oversampled tape saturation with wow, flutter, noise and age.
tape-chorus|Tape Chorus|Tape style chorus with 1 to 8 voices, feedback, wow, flutter and saturation.
tape-flanger|Tape Flanger|Through-zero tape style flanger.
switchboard|Switchboard|Put it on Stereo Out to see and set the controls of these plug-ins on every track from one window."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/pkgs" "$work/scripts"

# Lets an already running host see the new plug-ins without a restart.
cat > "$work/scripts/postinstall" <<'EOF'
#!/bin/sh
killall -9 AudioComponentRegistrar >/dev/null 2>&1
exit 0
EOF
chmod 755 "$work/scripts/postinstall"

xml_escape() { sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g'; }

outline=""; choices=""; refs=""; count=0
while IFS='|' read -r slug name desc; do
    comp="$src/$name.component"
    if [ ! -d "$comp" ]; then echo "skipped (not in $src): $name"; continue; fi
    plist="$comp/Contents/Info.plist"
    version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$plist")
    bundle=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$plist")
    id="$bundle.pkg"

    root="$work/root-$slug"; mkdir -p "$root"
    ditto "$comp" "$root/$name.component"
    # Never relocate: without this, Installer puts the update wherever it finds another copy of
    # the bundle (a build folder, the other Components folder) instead of the Components folder.
    pkgbuild --analyze --root "$root" "$work/$slug.plist" >/dev/null
    n=$(/usr/libexec/PlistBuddy -c "Print" "$work/$slug.plist" | grep -c "RootRelativeBundlePath" || true)
    for ((i = 0; i < n; i++)); do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$work/$slug.plist"
        plutil -replace "$i.BundleOverwriteAction" -string upgrade "$work/$slug.plist"
    done
    pkgbuild --quiet --root "$root" --component-plist "$work/$slug.plist" \
        --identifier "$id" --version "$version" \
        --install-location /Library/Audio/Plug-Ins/Components \
        --scripts "$work/scripts" "$work/pkgs/$slug.pkg"

    t=$(printf '%s %s' "$name" "$version" | xml_escape); d=$(printf '%s' "$desc" | xml_escape)
    outline+="        <line choice=\"$slug\"/>"$'\n'
    choices+="    <choice id=\"$slug\" title=\"$t\" description=\"$d\" start_selected=\"true\">"$'\n'
    choices+="        <pkg-ref id=\"$id\"/>"$'\n'
    choices+="    </choice>"$'\n'
    refs+="    <pkg-ref id=\"$id\" version=\"$version\" onConclusion=\"none\">$slug.pkg</pkg-ref>"$'\n'
    count=$((count + 1))
    echo "added: $name $version"
done <<< "$PLUGINS"

[ "$count" -gt 0 ] || { echo "no plug-ins found in $src"; exit 1; }

cat > "$work/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Smooth Plugins</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="always" require-scripts="false" hostArchitectures="arm64" rootVolumeOnly="true"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="13.0"/>
        </allowed-os-versions>
    </volume-check>
    <choices-outline>
$outline    </choices-outline>
$choices$refs</installer-gui-script>
EOF

mkdir -p "$(dirname "$out")"
productbuild --quiet --distribution "$work/distribution.xml" --resources "$here/resources" \
    --package-path "$work/pkgs" "$out"
echo "built $out with $count plug-ins"
