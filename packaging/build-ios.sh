#!/bin/bash
# Builds opencreatures1 for iPad and iPhone and exports it for TestFlight.
#
#   packaging/build-ios.sh <team id> [build number] [--upload]
#
# Needs the full Xcode (not just the command-line tools), signed in to an Apple Developer
# Program account (Xcode > Settings > Accounts), and an app record in App Store Connect
# with the bundle ID below. Without --upload it stops at build-ios/export/Creatures.ipa, which can
# also be uploaded with Apple's Transporter app. With --upload, Xcode sends the build to App
# Store Connect, where it appears in TestFlight after Apple's processing (usually minutes).
#
# The game's files are not in the app: on the device, copy your own Creatures 1 folder into
# the app's folder (Finder > the device > Files, or the Files app).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT"
BUILD="$SRC/build-ios"
OUT="$ROOT/build-ios/export"
BUNDLE_ID="${OPENC1_IOS_BUNDLE_ID:-com.jeffdehut.opencreatures1}"

TEAM="${1:-}"
NUMBER="${2:-$(date +%Y%m%d%H%M)}"
UPLOAD=0
for arg in "$@"; do
	[ "$arg" = "--upload" ] && UPLOAD=1
done
if [ -z "$TEAM" ] || [ "$TEAM" = "--upload" ]; then
	echo "usage: $0 <team id> [build number] [--upload]" >&2
	echo "The team ID is the 10 characters in App Store Connect > Membership details." >&2
	exit 2
fi
if ! xcodebuild -version >/dev/null 2>&1; then
	echo "The full Xcode is needed: install it from the App Store, open it once, then run" >&2
	echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
	exit 1
fi

# openc2e turns on AddressSanitizer whenever the compiler supports it; off for a shippable build
cmake -S "$SRC" -B "$BUILD" -G Xcode \
	-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
	-Dhave_fsanitize_address=OFF -DOPENC2E_BUNDLED_SDL3=ON \
	-DOPENC1_IOS_TEAM="$TEAM" -DOPENC1_IOS_BUNDLE_ID="$BUNDLE_ID" -DOPENC1_IOS_BUILD="$NUMBER"

mkdir -p "$OUT"
ARCHIVE="$OUT/Creatures.xcarchive"
rm -rf "$ARCHIVE"
xcodebuild -project "$BUILD/openc2e.xcodeproj" -scheme opencreatures1 -configuration Release \
	-destination "generic/platform=iOS" -archivePath "$ARCHIVE" -allowProvisioningUpdates \
	DEVELOPMENT_TEAM="$TEAM" archive

DESTINATION=export
[ "$UPLOAD" = 1 ] && DESTINATION=upload
OPTIONS="$BUILD/ExportOptions.plist"
cat > "$OPTIONS" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>method</key>
	<string>app-store-connect</string>
	<key>destination</key>
	<string>$DESTINATION</string>
	<key>teamID</key>
	<string>$TEAM</string>
	<key>signingStyle</key>
	<string>automatic</string>
	<key>uploadSymbols</key>
	<true/>
	<key>manageAppVersionAndBuildNumber</key>
	<false/>
</dict>
</plist>
EOF
xcodebuild -exportArchive -archivePath "$ARCHIVE" -exportOptionsPlist "$OPTIONS" -exportPath "$OUT" -allowProvisioningUpdates
if [ "$UPLOAD" = 1 ]; then
	echo "Uploaded build $NUMBER to App Store Connect; it shows in TestFlight once Apple has processed it."
else
	echo "Exported $OUT/Creatures.ipa (build $NUMBER). Upload it with Transporter, or run again with --upload."
fi
