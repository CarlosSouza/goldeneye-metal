#!/bin/bash
# Package the iOS GoldenEye build into a SideStore-installable .ipa.
#
# SideStore re-signs on install with the user's personal team, so an ad-hoc
# signature here is enough. Two hard-won rules from the GeneralsX port apply:
# strip extended attributes (AppleDouble ._* entries break install with
# 0xe8008017) and zip with plain `zip -X`, never `ditto`.
#
# Usage: ./scripts/build/ios/package-ipa.sh
# Output: out/ios-arm64/GoldenEye-iPad.ipa

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd -P)"

BUILD_DIR="$REPO_ROOT/vendor/GoldenEye-Recomp/out/build/ios-arm64-release"
RUNTIME_DYLIB="$REPO_ROOT/out/ios-arm64/librexruntime.dylib"
OUT_DIR="$REPO_ROOT/out/ios-arm64"
BUNDLE_ID="${GE_BUNDLE_ID:-digital.coopers.goldeneye}"
APP_VERSION="${GE_VERSION:-0.4.1}"
MIN_OS="${GE_MIN_OS:-16.0}"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

APP="$STAGE/Payload/GoldenEye.app"
mkdir -p "$APP/Frameworks"

cp "$BUILD_DIR/GoldenEye.app/GoldenEye" "$APP/GoldenEye"
cp "$RUNTIME_DYLIB" "$APP/Frameworks/librexruntime.dylib"

# The build-tree LC_RPATH points at the developer Mac; the bundle resolves
# the runtime from Frameworks/ instead.
install_name_tool -rpath "$REPO_ROOT/out/ios-arm64" "@executable_path/Frameworks" \
    "$APP/GoldenEye" 2>/dev/null || \
    install_name_tool -add_rpath "@executable_path/Frameworks" "$APP/GoldenEye"

cat > "$APP/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>GoldenEye</string>
    <key>CFBundleExecutable</key><string>GoldenEye</string>
    <key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>GoldenEye</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${APP_VERSION}</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>MinimumOSVersion</key><string>${MIN_OS}</string>
    <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
    <key>UILaunchScreen</key><dict/>
    <key>UIRequiresFullScreen</key><true/>
    <key>UIFileSharingEnabled</key><true/>
    <key>LSSupportsOpeningDocumentsInPlace</key><true/>
    <key>UIStatusBarHidden</key><true/>
    <key>UISupportedInterfaceOrientations</key>
    <array>
        <string>UIInterfaceOrientationLandscapeLeft</string>
        <string>UIInterfaceOrientationLandscapeRight</string>
    </array>
    <key>UISupportedInterfaceOrientations~ipad</key>
    <array>
        <string>UIInterfaceOrientationLandscapeLeft</string>
        <string>UIInterfaceOrientationLandscapeRight</string>
    </array>
</dict>
</plist>
PLIST

# The 4.5 GB guest address-space reservation needs extended virtual
# addressing; SideStore reads these from the signature and requests them in
# the personal provisioning profile on install (same mechanism Dolphin uses).
ENTITLEMENTS="$STAGE/entitlements.plist"
cat > "$ENTITLEMENTS" <<'EPLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.developer.kernel.extended-virtual-addressing</key><true/>
    <key>com.apple.developer.kernel.increased-memory-limit</key><true/>
</dict>
</plist>
EPLIST

codesign --force --sign - "$APP/Frameworks/librexruntime.dylib"
codesign --force --sign - --entitlements "$ENTITLEMENTS" "$APP"

xattr -cr "$STAGE/Payload"

IPA="$OUT_DIR/GoldenEye-iPad.ipa"
rm -f "$IPA"
(cd "$STAGE" && /usr/bin/zip -qr -X "$IPA" Payload)

echo "OK: $IPA"
du -sh "$IPA"
