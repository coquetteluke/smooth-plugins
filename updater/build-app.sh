#!/bin/bash
# Builds "Smooth Plugins.app", the installer and updater, into the given folder.
# Usage: updater/build-app.sh <output folder>
# Needs Xcode or the Command Line Tools (swiftc). The version is CFBundleShortVersionString in Info.plist.
set -euo pipefail

out=${1:?usage: build-app.sh <output folder>}
here=$(cd "$(dirname "$0")" && pwd)
app="$out/Smooth Plugins.app"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$here/Info.plist" "$app/Contents/Info.plist"
cp "$here/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
printf 'APPL????' > "$app/Contents/PkgInfo"

xcrun --sdk macosx swiftc -O -swift-version 6 -parse-as-library \
    -target arm64-apple-macos13.0 \
    "$here"/Sources/*.swift \
    -o "$app/Contents/MacOS/Smooth Plugins"

codesign --force --sign - "$app"
echo "built $app ($(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist"))"
