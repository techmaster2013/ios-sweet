#!/bin/bash
# Build pistrap-helper for iOS arm64 with the Darwin SDK that xtool installed.
#   apps/PiStrap/helper/build.sh [out]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/pistrap-helper}
B=$HOME/.swiftpm/swift-sdks/darwin.artifactbundle
SDK=$B/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk
clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -fuse-ld=lld -B "$B/toolset/bin" -Wl,-adhoc_codesign \
	-o "$OUT" "$HERE/pistrap-helper.c"
echo "built $OUT"
