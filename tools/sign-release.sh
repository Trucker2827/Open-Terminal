#!/bin/zsh
# sign-release.sh — locally Developer-ID-sign + notarize the macOS DMG of a
# GitHub release and replace the asset in place.
#
# WHY THIS EXISTS: the Developer ID certificate is deliberately kept OFF
# GitHub (operator decision 2026-07-22), so CI builds the mac artifact with
# ad-hoc signing and skips notarization ("MACOS_SIGNING_MODE=adhoc" warning
# in the release log). Gatekeeper refuses those DMGs. This script downloads
# the CI DMG, re-signs the bundle inside-out with the local Developer ID
# identity, notarizes app AND dmg via the local keychain profile, staples
# both, verifies with Gatekeeper's own assessor, and uploads the fixed DMG
# over the release asset.
#
# Usage:   tools/sign-release.sh v0.3.31
# Options: SIGN_IDENTITY / NOTARY_PROFILE env vars override the defaults.
#
# Requirements (all local): a "Developer ID Application" identity in the
# login keychain, an `xcrun notarytool store-credentials` profile (default
# name: dinero-notarytool), Xcode CLT, gh CLI authenticated for the repo.
set -euo pipefail

TAG="${1:?usage: sign-release.sh <release-tag e.g. v0.3.31>}"
REPO="${REPO:-Trucker2827/Open-Terminal}"
NOTARY_PROFILE="${NOTARY_PROFILE:-dinero-notarytool}"
SIGN_IDENTITY="${SIGN_IDENTITY:-$(security find-identity -v -p codesigning | grep -m1 -o '"Developer ID Application: [^"]*"' | tr -d '"')}"
[ -n "$SIGN_IDENTITY" ] || { echo "ERROR: no 'Developer ID Application' identity in keychain" >&2; exit 1; }
echo "== tag: $TAG"
echo "== identity: $SIGN_IDENTITY"
echo "== notary profile: $NOTARY_PROFILE"

WORK=$(mktemp -d /tmp/sign-release.XXXXXX)
trap 'hdiutil detach "$WORK/mnt" -quiet 2>/dev/null || true; rm -rf "$WORK"' EXIT
cd "$WORK"

echo "== downloading DMG asset from $TAG"
gh release download "$TAG" --repo "$REPO" --pattern "*macos*dmg" -D .
DMG=$(ls *.dmg | head -1)
[ -n "$DMG" ] || { echo "ERROR: no macOS DMG asset on $TAG" >&2; exit 1; }
VOLNAME=$(hdiutil imageinfo "$DMG" | awk -F': ' '/partition-name|Volume Name/ {print $2; exit}')
[ -n "$VOLNAME" ] || VOLNAME="OpenTerminal"

echo "== extracting app"
hdiutil attach "$DMG" -nobrowse -quiet -mountpoint "$WORK/mnt"
APP_SRC=$(ls -d "$WORK"/mnt/*.app | head -1)
APP="$WORK/$(basename "$APP_SRC")"
ditto "$APP_SRC" "$APP"
hdiutil detach "$WORK/mnt" -quiet

sign() { codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$@"; }

echo "== signing inside-out"
# 1. QtWebEngine helper app first (deepest nesting; --deep is fine for it)
find "$APP" -type d -name "QtWebEngineProcess.app" | while read -r h; do
  codesign --force --options runtime --timestamp --deep --sign "$SIGN_IDENTITY" "$h"
done
# 2. Every loose Mach-O (dylibs, python .so extensions, plugin binaries)
find "$APP" -type f \( -name "*.dylib" -o -name "*.so" \) -print0 | while IFS= read -r -d '' m; do
  sign "$m" 2>/dev/null || echo "  warn: could not sign $m"
done
# 3. Extra executables next to the main binary + any Mach-O inside frameworks
find "$APP/Contents/MacOS" -type f -perm +111 -print0 | while IFS= read -r -d '' x; do
  file -b "$x" | grep -q Mach-O && sign "$x" 2>/dev/null || true
done
find "$APP/Contents/Frameworks" -type f -perm +111 -print0 2>/dev/null | while IFS= read -r -d '' x; do
  file -b "$x" | grep -q Mach-O && sign "$x" 2>/dev/null || true
done
# 3b. Mach-O tools hiding in Resources/ (e.g. bundled yt-dlp — Apple rejects
#     the whole submission if ANY unsigned Mach-O ships anywhere in the bundle)
find "$APP/Contents/Resources" -type f -perm +111 -print0 2>/dev/null | while IFS= read -r -d '' x; do
  file -b "$x" | grep -q Mach-O && sign "$x" 2>/dev/null || true
done
# 4. Nested .apps (other than the webengine helper handled above)
find "$APP" -type d -name "*.app" ! -path "$APP" ! -name "QtWebEngineProcess.app" | while read -r n; do
  codesign --force --options runtime --timestamp --deep --sign "$SIGN_IDENTITY" "$n" 2>/dev/null || true
done
# 5. Framework bundles (reseals their Resources — Python.framework needs this)
find "$APP/Contents/Frameworks" -maxdepth 1 -type d -name "*.framework" | while read -r fw; do
  sign "$fw"
done
# 6. Outer bundle last
sign "$APP"

echo "== verifying signature chain"
codesign --verify --strict --deep "$APP"
echo "   clean"

echo "== notarizing app (Apple round-trip, takes minutes)"
ditto -c -k --keepParent "$APP" app.zip
xcrun notarytool submit app.zip --keychain-profile "$NOTARY_PROFILE" --wait | tee notary-app.log
grep -q "status: Accepted" notary-app.log || { echo "ERROR: app notarization not Accepted" >&2; exit 1; }
xcrun stapler staple "$APP"

echo "== rebuilding DMG"
STAGE="$WORK/stage"; mkdir "$STAGE"
ditto "$APP" "$STAGE/$(basename "$APP")"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "$VOLNAME" -srcfolder "$STAGE" -ov -format UDZO "$WORK/$DMG.new" -quiet
mv "$WORK/$DMG.new" "$WORK/$DMG"
sign "$WORK/$DMG"

echo "== notarizing DMG"
xcrun notarytool submit "$WORK/$DMG" --keychain-profile "$NOTARY_PROFILE" --wait | tee notary-dmg.log
grep -q "status: Accepted" notary-dmg.log || { echo "ERROR: dmg notarization not Accepted" >&2; exit 1; }
xcrun stapler staple "$WORK/$DMG"

echo "== Gatekeeper assessment"
spctl -a -t open --context context:primary-signature -v "$WORK/$DMG"

echo "== replacing release asset"
gh release upload "$TAG" "$WORK/$DMG" --repo "$REPO" --clobber
echo "DONE: $TAG macOS DMG is signed, notarized, stapled, and replaced."
