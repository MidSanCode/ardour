#!/bin/sh
#
# Copy the cross-compiled Android engine into the Gradle project, so that the
# application can be built and installed.
#
# Usage: tools/copy-android-engine.sh [build-dir]
#
# The build directory is the one waf used (build/ by default).  The shared
# objects are copied into android/app/src/main/jniLibs/<abi>/, which is where
# the Gradle build picks them up.

set -e

BUILD_DIR=${1:-build}
ABI=${ANDROID_ABI:-arm64-v8a}
DEST="$(dirname "$0")/../android/app/src/main/jniLibs/$ABI"

if [ ! -d "$BUILD_DIR" ]; then
    echo "copy-android-engine: no build directory $BUILD_DIR" >&2
    exit 1
fi

mkdir -p "$DEST"

# the engine, the control protocol and the audio backend
found=0
for so in $(find "$BUILD_DIR" -name '*.so' \( -name 'libardour*.so*' -o -name 'libardour_android.so*' \) 2>/dev/null); do
    base=$(basename "$so")
    case "$base" in
        *.so.*)
            # versioned sonames: the loader wants the unversioned name too
            cp -f "$so" "$DEST/$base"
            cp -f "$so" "$DEST/$(echo "$base" | sed 's/\.so\..*/.so/')"
            ;;
        *)
            cp -f "$so" "$DEST/$base"
            ;;
    esac
    echo "copy-android-engine: $so -> $DEST"
    found=1
done

if [ "$found" -eq 0 ]; then
    echo "copy-android-engine: no Ardour shared libraries found under $BUILD_DIR" >&2
    echo "copy-android-engine: build them first:" >&2
    echo "  ./waf configure --dist-target=android --with-ndk=<ndk> --with-frontends=none" >&2
    echo "  ./waf build" >&2
    exit 1
fi
