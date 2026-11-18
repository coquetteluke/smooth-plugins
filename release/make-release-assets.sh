#!/bin/bash
# Makes every file a release carries, from a folder of built .component files:
#   Smooth-Plugins-App.zip         the Smooth Plugins app, which installs and updates the plug-ins
#   Smooth-Plugins-Installer.pkg   the macOS Installer with a checkbox per plug-in
#   Smooth-Plugins.zip             every plug-in plus INSTALL.txt, for installing by hand
#   <Name>.component.zip           one zip per plug-in; the app downloads these
#   manifest.json                  versions, checksums and links; the app reads this
#
# Usage: release/make-release-assets.sh <folder with .component files> <release tag> <output folder>
# Set ASSET_BASE_URL to point the manifest's links somewhere other than the GitLab release (tests).
set -euo pipefail

src=${1:?usage: make-release-assets.sh <components folder> <tag> <output folder>}
tag=${2:?usage: make-release-assets.sh <components folder> <tag> <output folder>}
out=${3:?usage: make-release-assets.sh <components folder> <tag> <output folder>}
root=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$src" && pwd)
base=${ASSET_BASE_URL:-https://gitlab.com/coquettelabs/smooth-plugins/-/releases/$tag/downloads}

mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# One zip per plug-in, and a row for the manifest.
: > "$work/rows.tsv"
mkdir -p "$work/all/Smooth-Plugins"
while IFS='|' read -r -u 3 slug name kind desc; do
    comp="$src/$name.component"
    if [ ! -d "$comp" ]; then echo "skipped (not in $src): $name"; continue; fi
    asset="${name// /-}.component.zip"
    ditto -c -k --sequesterRsrc --keepParent "$comp" "$out/$asset"
    ditto "$comp" "$work/all/Smooth-Plugins/$name.component"
    plist="$comp/Contents/Info.plist"
    version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$plist")
    bundle=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$plist")
    sha=$(shasum -a 256 "$out/$asset" | cut -d' ' -f1)
    size=$(stat -f %z "$out/$asset")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$slug" "$name" "$kind" "$desc" "$version" "$bundle" "$asset" "$sha" "$size" >> "$work/rows.tsv"
done 3< <(grep -v '^#' "$root/installer/plugins.txt" | grep .)

# Everything in one zip, for installing by hand.
cat > "$work/all/Smooth-Plugins/INSTALL.txt" <<'EOF'
Smooth Plugins: free plug-ins for Logic Pro only (Apple Silicon Mac, macOS 13 or newer)

Easiest: download Smooth-Plugins-App.zip from the release page instead. The app installs the
plug-ins you pick and keeps them up to date. macOS blocks it the first time; the steps to open it
are in "How to open Smooth Plugins.txt" inside that zip, and with pictures at
https://gitlab.com/coquettelabs/smooth-plugins#opening-the-app-the-first-time

By hand:
1. In Finder, choose Go > Go to Folder, paste ~/Library/Audio/Plug-Ins/Components and press Return.
2. Drag the .component files you want into that folder.
3. Open Terminal and run this once (macOS blocks plug-ins copied out of a downloaded zip until you do):

   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/*.component

4. Open Logic Pro. The plug-ins are under Audio Units > Luke Garito.

Source code, manuals and license (GNU AGPL v3): https://gitlab.com/coquettelabs/smooth-plugins
EOF
( cd "$work/all" && ditto -c -k --sequesterRsrc --keepParent Smooth-Plugins "$out/Smooth-Plugins.zip" )

# The installer package and the app.
"$root/installer/build-installer.sh" "$src" "$out/Smooth-Plugins-Installer.pkg" | tail -1
"$root/updater/build-app.sh" "$work/app" | tail -1
app_version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$work/app/Smooth Plugins.app/Contents/Info.plist")
cat > "$work/app/How to open Smooth Plugins.txt" <<'EOF'
How to open Smooth Plugins the first time
=========================================

1. Drag "Smooth Plugins" into your Applications folder and double-click it.
2. macOS says it was not opened. Click Done (not Move to Trash).
3. Open System Settings > Privacy & Security, scroll down to Security, and click Open Anyway
   next to "Smooth Plugins.app was blocked to protect your Mac".
4. macOS asks once more. Click Open Anyway, then enter your password or use Touch ID.
   Careful: the blue button there is Move to Trash, so don't just press Return.

You only do this once. From then on Smooth Plugins opens normally, and the plug-ins it installs
don't need this step.

Why: macOS only trusts an app straight away if its developer pays Apple $99 a year and sends every
version to Apple to be checked. Smooth Plugins is free and that fee isn't paid, so Apple hasn't
checked it. The warning does not mean anything wrong was found. All of the code is public:
https://gitlab.com/coquettelabs/smooth-plugins

Pictures of each step: https://gitlab.com/coquettelabs/smooth-plugins#opening-the-app-the-first-time
EOF
( cd "$work/app" && ditto -c -k --sequesterRsrc . "$out/Smooth-Plugins-App.zip" )

# The manifest the app reads.
app_sha=$(shasum -a 256 "$out/Smooth-Plugins-App.zip" | cut -d' ' -f1)
app_size=$(stat -f %z "$out/Smooth-Plugins-App.zip")
/usr/bin/python3 -I - "$work/rows.tsv" "$out/manifest.json" "$tag" "$base" "$app_version" "$app_sha" "$app_size" <<'EOF'
import json, sys
rows_path, out_path, tag, base, app_version, app_sha, app_size = sys.argv[1:8]
plugins = []
for line in open(rows_path, encoding="utf-8"):
    slug, name, kind, desc, version, bundle, asset, sha, size = line.rstrip("\n").split("\t")
    plugins.append({"id": slug, "name": name, "kind": kind, "description": desc, "version": version,
                    "bundleId": bundle, "url": f"{base}/{asset}", "sha256": sha, "size": int(size)})
manifest = {"formatVersion": 1, "release": tag,
            "app": {"version": app_version, "url": f"{base}/Smooth-Plugins-App.zip",
                    "sha256": app_sha, "size": int(app_size)},
            "plugins": plugins}
with open(out_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
print(f"manifest.json: {len(plugins)} plug-ins, app {app_version}")
EOF
