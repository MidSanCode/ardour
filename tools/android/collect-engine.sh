#!/bin/bash
#
# Collect an installed Android build of the engine into the layout the
# application consumes:
#
#   tools/android/collect-engine.sh <installed-prefix> <deps-prefix> <out>
#
#   <out>/jniLibs/<abi>/lib*.so   every shared library, in one flat directory:
#                                 that is the only way an APK carries native
#                                 code, and the directory the dynamic linker
#                                 searches for DT_NEEDED entries
#   <out>/assets/ardour/          the engine's data files (share/ardour*)
#
# The DT_NEEDED entries of every library are checked against that directory
# plus the libraries the platform provides, so a library that would fail to
# load on a device fails the build instead.

set -euo pipefail

INST=${1:?usage: collect-engine.sh <installed-prefix> <deps-prefix> <out>}
DEPS=${2:?usage: collect-engine.sh <installed-prefix> <deps-prefix> <out>}
OUT=${3:?usage: collect-engine.sh <installed-prefix> <deps-prefix> <out>}
NDK=${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT}
READELF=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf

ABI=$(basename "$OUT"); ABI=${ABI#engine-}
LIBS=$OUT/jniLibs/$ABI
rm -rf "$OUT"
mkdir -p "$LIBS" "$OUT/assets"

# the engine: libardour & co, the backends, the panners, the JNI bridge
find "$INST/lib" -name 'lib*.so' -type f -exec cp {} "$LIBS/" \;
# the dependency stack
find "$DEPS/lib" -maxdepth 1 -name 'lib*.so' -type f -exec cp {} "$LIBS/" \;

# the data files
if [ -d "$INST/share" ]; then
	for d in "$INST"/share/ardour*; do
		[ -d "$d" ] && cp -a "$d" "$OUT/assets/ardour"
	done
fi

# libraries the platform provides (the NDK's stable API)
SYSTEM="libc.so libm.so libdl.so liblog.so libz.so libandroid.so libaaudio.so
libOpenSLES.so libEGL.so libGLESv2.so libjnigraphics.so libmediandk.so
libamidi.so libnativewindow.so libvulkan.so libsync.so libbinder_ndk.so"
# one line, so that every name is matched between blanks
SYSTEM=$(echo $SYSTEM)

missing=0
for so in "$LIBS"/*.so; do
	for need in $("$READELF" -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
		case " $SYSTEM " in *" $need "*) continue ;; esac
		if [ ! -f "$LIBS/$need" ]; then
			echo "error: $(basename "$so") needs $need, which is not in the APK" >&2
			missing=1
		fi
	done
done
[ "$missing" -eq 0 ] || exit 1

echo "collected $(ls "$LIBS" | wc -l) libraries into $LIBS"
