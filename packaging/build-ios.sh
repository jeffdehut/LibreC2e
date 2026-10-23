#!/bin/bash
# Builds opencreatures1 for iPad and iPhone and exports it for TestFlight, or installs a
# development copy on a connected device.
#
#   packaging/build-ios.sh <team id> [build number] [--upload]
#   packaging/build-ios.sh <team id> --device [device name or id]
#
# With --device it builds a development copy and installs it on a connected iPad or iPhone
# (the only one connected, or the one named), with no App Store Connect record. The device
# needs Developer Mode on (Settings > Privacy & Security), which shows once it has been
# connected to this Mac with Xcode open; the team's certificate signs it.
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
NUMBER="$(date +%Y%m%d%H%M)"
UPLOAD=0
DEVICE=""
DEVICE_MODE=0
shift || true
while [ $# -gt 0 ]; do
	case "$1" in
		--upload) UPLOAD=1 ;;
		--device)
			DEVICE_MODE=1
			if [ $# -gt 1 ] && [ "${2#--}" = "$2" ]; then
				DEVICE="$2"
				shift
			fi
			;;
		*) NUMBER="$1" ;;
	esac
	shift
done
if [ -z "$TEAM" ] || [ "${TEAM#--}" != "$TEAM" ]; then
	echo "usage: $0 <team id> [build number] [--upload]" >&2
	echo "       $0 <team id> --device [device name or id]" >&2
	echo "The team ID is the 10 characters in App Store Connect > Membership details, or the" >&2
	echo "OU of the Apple Development certificate in Keychain Access." >&2
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

if [ "$DEVICE_MODE" = 1 ]; then
	# the device: the one named, or the only iPad or iPhone connected
	LIST="$BUILD/devices.json"
	xcrun devicectl list devices --json-output "$LIST" > /dev/null
	UDID="$(python3 -I - "$LIST" "$DEVICE" <<'PY'
import json, sys
devices = json.load(open(sys.argv[1]))["result"]["devices"]
wanted = sys.argv[2]
found = []
for d in devices:
	hw, props = d.get("hardwareProperties", {}), d.get("deviceProperties", {})
	if hw.get("platform") != "iOS":
		continue
	if wanted and wanted not in (props.get("name"), hw.get("udid"), d.get("identifier")):
		continue
	found.append((props.get("name", "?"), hw.get("udid", "")))
if len(found) != 1:
	names = ", ".join(n for n, _ in found) or "none"
	sys.exit(f"Connect one iPad or iPhone (found: {names}); or name it after --device.")
print(found[0][1])
PY
)"
	xcodebuild -project "$BUILD/openc2e.xcodeproj" -scheme opencreatures1 -configuration Release \
		-destination "id=$UDID" -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
		DEVELOPMENT_TEAM="$TEAM" build
	APP="$(find "$BUILD" -maxdepth 3 -type d -path "*Release-iphoneos/*.app" | head -1)"
	xcrun devicectl device install app --device "$UDID" "$APP"
	echo "Installed $(basename "$APP") (build $NUMBER) on the device. Copy your Creatures 1 folder"
	echo "into the app's folder (Finder > the device > Files) before opening it."
	exit 0
fi

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
