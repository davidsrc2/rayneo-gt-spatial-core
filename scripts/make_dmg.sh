#!/usr/bin/env bash
# Builds a Release app and packages it as a drag-to-install DMG in dist/.
#
#   scripts/make_dmg.sh
#   DEVELOPER_ID="Developer ID Application: Name (TEAMID)" NOTARY_PROFILE=rayneo scripts/make_dmg.sh
#
# Without DEVELOPER_ID the app is ad-hoc signed: it runs on this Mac, but on other Macs
# Gatekeeper asks to confirm it (right click > Open). With DEVELOPER_ID it is signed with the
# hardened runtime; with NOTARY_PROFILE (xcrun notarytool store-credentials) it is notarized
# and stapled, so it opens anywhere without warnings.
set -euo pipefail
cd "$(dirname "$0")/.."

APP_NAME="Nimbo"
VERSION=$(sed -nE 's/^project\(rayneo_gt_spatial_core VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)
DMG="dist/Nimbo-${VERSION}.dmg"

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-release --target nimbo

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -R build-release/Nimbo.app "$STAGE/$APP_NAME.app"
find "$STAGE/$APP_NAME.app/Contents/MacOS" -name "*.dSYM" -prune -exec rm -rf {} +

if [[ -n "${DEVELOPER_ID:-}" ]]; then
    codesign --force --options runtime --timestamp --sign "$DEVELOPER_ID" \
             --identifier es.gestorae.nimbo "$STAGE/$APP_NAME.app"
else
    codesign --force --sign - --identifier es.gestorae.nimbo "$STAGE/$APP_NAME.app"
fi
codesign --verify --strict "$STAGE/$APP_NAME.app"

ln -s /Applications "$STAGE/Applications"
mkdir -p dist
rm -f "$DMG"
# macOS 26+: `diskutil image`; older systems: hdiutil (deprecated there, still available).
if diskutil image create from --help >/dev/null 2>&1; then
    diskutil image create from --format UDZO --volumeName "$APP_NAME" "$STAGE" "$DMG" >/dev/null
else
    hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
fi

if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
fi

echo "$DMG ($(du -h "$DMG" | cut -f1))"
