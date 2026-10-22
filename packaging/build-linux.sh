#!/bin/bash
# Builds LibreC2e (opencreatures1) for 64-bit Linux inside an Ubuntu 22.04 container, so it
# runs on most Linux systems from 2022 on, in build-linux/. Needs Docker (on a Mac:
# brew install colima docker; colima start --vm-type vz --vz-rosetta).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
docker run --rm --platform linux/amd64 -v "$ROOT":/src ubuntu:22.04 bash -c '
	set -e
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	apt-get install -y -qq build-essential python3-pip ninja-build git pkg-config zlib1g-dev \
		libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev libxrender-dev libxinerama-dev libdecor-0-dev \
		libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev \
		libgles2-mesa-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev libdbus-1-dev \
		libudev-dev libpipewire-0.3-dev > /dev/null
	# a newer CMake than Ubuntu 22.04 has, for SDL3
	pip3 install -q cmake==3.31.6
	git config --global --add safe.directory /src
	cmake -S /src -B /src/build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENC2E_BUNDLED_SDL3=ON -Dhave_fsanitize_address=OFF > /dev/null
	cmake --build /src/build-linux --target opencreatures1
'
echo "Built $ROOT/build-linux/opencreatures1"
