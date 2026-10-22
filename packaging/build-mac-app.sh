#!/bin/bash
# Builds LibreC2e.app for Apple Silicon Macs in build-mac/. When it starts, it finds your own
# copy of Creatures 1 (or asks for its folder); none of the game is in the app.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-mac"

# openc2e turns on AddressSanitizer whenever the compiler supports it; off for a shippable build
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -Dhave_fsanitize_address=OFF \
	-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" --target opencreatures1

APP="$BUILD/LibreC2e.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/Licenses/LibreCreatures"
cp "$BUILD/opencreatures1" "$APP/Contents/MacOS/LibreC2e"

# the icon: the openc2e logo on a square
ICONSET="$(mktemp -d)/AppIcon.iconset"
mkdir -p "$ICONSET"
sips -s format png --padToHeightWidth 200 200 "$ROOT/Openc2e-logo2008.png" --out "$ICONSET/base.png" >/dev/null
for s in 16 32 128 256 512; do
	sips -z $s $s "$ICONSET/base.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
	sips -z $((s * 2)) $((s * 2)) "$ICONSET/base.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
rm "$ICONSET/base.png"
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" || echo "(no icon: iconutil couldn't run here)"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>LibreC2e</string>
	<key>CFBundleDisplayName</key><string>LibreC2e</string>
	<key>CFBundleIdentifier</key><string>org.librec2e.LibreC2e</string>
	<key>CFBundleExecutable</key><string>LibreC2e</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$(git -C "$ROOT" log -1 --format=%cs)</string>
	<key>CFBundleVersion</key><string>$(git -C "$ROOT" rev-parse --short HEAD)</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.simulation-games</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

# the licences go with the program
cp "$ROOT/LICENSE" "$ROOT/COPYING" "$ROOT/NOTICE.md" "$ROOT/AUTHORS" "$APP/Contents/Resources/Licenses/"
cp "$ROOT/third_party/librecreatures/LICENSE" "$ROOT/third_party/librecreatures/NOTICE.md" "$APP/Contents/Resources/Licenses/LibreCreatures/"

# an ad-hoc signature so Apple Silicon will run it
codesign --force --deep --sign - "$APP"
echo "Built $APP"
