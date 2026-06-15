#!/bin/bash
# macOS release pipeline: build → bundle-id guard → deploy → sign (hardened
# runtime + entitlements) → DMG → notarize → staple → verify.
#
# Tracked, parameterized version of the proven v0.1.0–v0.3.0 recipe. The release
# version is read from the BUILT bundle's Info.plist (single source of truth =
# the CMake project version) — never by launching the GUI (it has no --version
# flag and would start its event loop and hang the script).
#
# Usage: scripts/release/release-macos.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD=/tmp/ot-build-clean
APPID="Developer ID Application: Mirsad Hajdarevic (JXJS6ZA5FJ)"
PROFILE=dinero-notarytool
ENT=/tmp/ot-entitlements.plist
MACDEPLOYQT=/opt/homebrew/bin/macdeployqt

step() { echo; echo "==================== $* ===================="; }

step "1. reconfigure + build Release"
env -u CI cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOPENMARKETTERMINAL_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew >/dev/null || { echo "CONFIGURE FAILED"; exit 1; }
env -u CI cmake --build "$BUILD" --target OpenMarketTerminal openterminalcli 2>&1 | tail -3
APP="$BUILD/OpenTerminal.app"
[ -d "$APP" ] || { echo "APP NOT BUILT"; exit 1; }
VER="$(/usr/bin/plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist" 2>/dev/null || echo '?')"
echo "app version: $VER"
[ "$VER" != "?" ] || { echo "COULD NOT READ VERSION"; exit 1; }
DMG="/tmp/OpenTerminal-${VER}-arm64.dmg"

step "1b. bundle-id guard (credentials persistence)"
# Refuse to sign/ship if CFBundleIdentifier drifted — a changed id orphans every
# user's saved broker credentials. See verify-bundle-id.sh for the why.
"$SCRIPT_DIR/verify-bundle-id.sh" "$APP" || { echo "ABORTING RELEASE"; exit 1; }

step "2. entitlements"
cat > "$ENT" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
  <key>com.apple.security.cs.disable-executable-page-protection</key><true/>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
</dict>
</plist>
EOF
cat "$ENT"

step "3. macdeployqt (bundle Qt frameworks/plugins)"
"$MACDEPLOYQT" "$APP" -verbose=1 2>&1 | tail -4 || { echo "MACDEPLOYQT FAILED"; exit 1; }

step "4. codesign (deep, hardened runtime + timestamp + entitlements)"
codesign --force --deep --options runtime --timestamp --entitlements "$ENT" \
  --sign "$APPID" "$APP" 2>&1 | tail -3 || { echo "CODESIGN FAILED"; exit 1; }
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | tail -3 || { echo "CODESIGN VERIFY FAILED"; exit 1; }
codesign -dv --verbose=2 "$APP" 2>&1 | grep -iE "Authority=Developer|flags|Runtime|TeamID|Identifier="

step "5. build DMG + sign it"
rm -f "$DMG"
hdiutil create -volname "OpenTerminal $VER" -srcfolder "$APP" -ov -format UDZO "$DMG" 2>&1 | tail -2 || { echo "DMG FAILED"; exit 1; }
codesign --force --timestamp --sign "$APPID" "$DMG" 2>&1 | tail -2
echo "dmg: $(ls -la "$DMG")"

step "6. notarize (--wait)"
xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait 2>&1 | tail -20 || { echo "NOTARIZE FAILED"; exit 1; }

step "7. staple + verify"
xcrun stapler staple "$DMG" 2>&1 | tail -3 || { echo "STAPLE FAILED"; exit 1; }
xcrun stapler validate "$DMG" 2>&1 | tail -2
spctl -a -vvv -t install "$DMG" 2>&1 | tail -3 || spctl -a -vvv "$APP" 2>&1 | tail -3

step "DONE — notarized + stapled DMG at $DMG"
shasum -a 256 "$DMG"
