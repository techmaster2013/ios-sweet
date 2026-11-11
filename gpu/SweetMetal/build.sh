#!/bin/bash
# Build SweetMetal.bundle (iOS arm64) with the Darwin SDK that xtool installed.
#   gpu/SweetMetal/build.sh [outdir]   -> <outdir>/SweetMetal.bundle
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
B=$HOME/.swiftpm/swift-sdks/darwin.artifactbundle
SDK=$B/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk
mkdir -p "$OUT/SweetMetal.bundle"
cp "$HERE/Info.plist" "$OUT/SweetMetal.bundle/Info.plist"
clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -fobjc-arc \
	-fuse-ld=lld -B "$B/toolset/bin" -Wl,-adhoc_codesign \
	-bundle -framework Foundation \
	-o "$OUT/SweetMetal.bundle/SweetMetal" "$HERE/SweetMetal.m"
echo "built $OUT/SweetMetal.bundle"
