#!/bin/bash
# Assemble SonoRocket.app (run on macOS).
#   build-app.sh <version> <SonoRocket.vst3> <SonoRocket.component|-> <rocket-relay> <cloudflared> <outdir>
# Optional signing/notarization (Developer ID), via env:
#   SIGN_IDENTITY="Developer ID Application: Daniel Belquer (TEAMID)"
#   NOTARY_PROFILE=<keychain profile created with `xcrun notarytool store-credentials`>
set -euo pipefail
VERSION=$1 VST3=$2 AU=$3 RELAY=$4 CLOUDFLARED=$5 OUT=$6
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$OUT/SonoRocket.app"
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

sed "s/@VERSION@/$VERSION/g" "$HERE/Info.plist.in" > "$APP/Contents/Info.plist"
cp "$HERE/SonoRocket" "$APP/Contents/MacOS/SonoRocket"; chmod +x "$APP/Contents/MacOS/SonoRocket"
echo "$VERSION" > "$APP/Contents/Resources/VERSION"
cp "$RELAY" "$APP/Contents/Resources/rocket-relay"; chmod +x "$APP/Contents/Resources/rocket-relay"
cp "$CLOUDFLARED" "$APP/Contents/Resources/cloudflared"; chmod +x "$APP/Contents/Resources/cloudflared"
cp -R "$VST3" "$APP/Contents/Resources/SonoRocket.vst3"
[ "$AU" != "-" ] && cp -R "$AU" "$APP/Contents/Resources/SonoRocket.component"

# icon
ICONSET="$(mktemp -d)/AppIcon.iconset"; mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
  sips -z $s $s "$HERE/AppIcon.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) "$HERE/AppIcon.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

if [ -n "${SIGN_IDENTITY:-}" ]; then
  sign() { codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" "$@"; }
  sign "$APP/Contents/Resources/rocket-relay"
  sign "$APP/Contents/Resources/cloudflared"
  sign --deep "$APP/Contents/Resources/SonoRocket.vst3"
  [ -d "$APP/Contents/Resources/SonoRocket.component" ] && sign --deep "$APP/Contents/Resources/SonoRocket.component"
  sign "$APP"
  codesign --verify --deep --strict "$APP"
fi

# fixed name, so ".../releases/latest/download/SonoRocket-mac.zip" is a permanent link
ZIP="$OUT/SonoRocket-mac.zip"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"

if [ -n "${SIGN_IDENTITY:-}" ] && [ -n "${NOTARY_PROFILE:-}" ]; then
  xcrun notarytool submit "$ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$APP"
  rm -f "$ZIP"; ditto -c -k --keepParent "$APP" "$ZIP"
fi
echo "built $ZIP"
