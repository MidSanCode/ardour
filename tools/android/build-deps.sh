#!/bin/bash
#
# Cross-compile the dependency stack of the Ardour engine for Android.
#
#   tools/android/build-deps.sh <abi> <prefix>
#
#   <abi>     arm64-v8a or x86_64 (the NDK/gradle spelling)
#   <prefix>  where the libraries, headers and .pc files are installed
#
# Environment:
#   ANDROID_NDK_ROOT  the NDK (r26 is what CI uses)          - required
#   ANDROID_API       the API level to build against          - default 28
#   SRCDIR            where the source tarballs are unpacked  - default <prefix>/../src-<abi>
#   JOBS              parallel jobs                           - default nproc
#
# Everything is built with meson or cmake (and ninja): no autotools, so the
# host needs python3, meson, ninja, cmake and pkg-config, nothing else.
#
# Every library is a shared library linked against libc++_shared.  The GLib
# family (glib, gobject, gthread, sigc++, glibmm, giomm) has global state, so a
# static copy in more than one of Ardour's shared libraries would not work;
# the rest is shared too, for one rule rather than two.  Both meson and cmake
# know that Android has no versioned sonames, so every library is called
# lib<name>.so, which is the only spelling an APK can carry.
#
# The list is the set of mandatory autowaf.check_pkg() probes reachable with
#   --dist-target=android --with-frontends=none --no-tk-stack
# plus what those libraries need themselves.  Boost is header-only and is
# copied from the host (see the end of this file).

set -euo pipefail
shopt -s inherit_errexit

ABI=${1:?usage: build-deps.sh <abi> <prefix>}
PREFIX=$(mkdir -p "${2:?usage: build-deps.sh <abi> <prefix>}" && cd "$2" && pwd)
NDK=${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT}
API=${ANDROID_API:-28}
SRCDIR=${SRCDIR:-$(dirname "$PREFIX")/src-$ABI}
JOBS=${JOBS:-$(nproc)}
HERE=$(cd "$(dirname "$0")" && pwd)

case "$ABI" in
	arm64-v8a) TRIPLE=aarch64-linux-android; CPU_FAMILY=aarch64; CPU=aarch64 ;;
	x86_64)    TRIPLE=x86_64-linux-android;  CPU_FAMILY=x86_64;  CPU=x86_64 ;;
	*) echo "unsupported ABI: $ABI" >&2; exit 1 ;;
esac

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TC/bin/$TRIPLE$API-clang
CXX=$TC/bin/$TRIPLE$API-clang++
test -x "$CC" || { echo "no NDK compiler at $CC" >&2; exit 1; }

mkdir -p "$SRCDIR" "$PREFIX/lib/pkgconfig"

# pkg-config must only ever see the cross-compiled stack
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
unset PKG_CONFIG_PATH

CROSS=$PREFIX/meson-cross.ini
cat > "$CROSS" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$TC/bin/llvm-ar'
strip = '$TC/bin/llvm-strip'
ranlib = '$TC/bin/llvm-ranlib'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-I$PREFIX/include', '-fPIC']
cpp_args = ['-I$PREFIX/include', '-fPIC']
c_link_args = ['-L$PREFIX/lib']
cpp_link_args = ['-L$PREFIX/lib']

[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'

[host_machine]
system = 'android'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'
EOF

CMAKE_ARGS=(
	-G Ninja
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
	-DANDROID_ABI="$ABI"
	-DANDROID_PLATFORM="android-$API"
	-DANDROID_STL=c++_shared
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX="$PREFIX"
	-DCMAKE_INSTALL_LIBDIR=lib
	-DCMAKE_PREFIX_PATH="$PREFIX"
	-DCMAKE_FIND_ROOT_PATH="$PREFIX"
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DBUILD_SHARED_LIBS=ON
	-DBUILD_TESTING=OFF
	)

# fetch <url> -> unpacks into $SRCDIR and prints the source directory
fetch () {
	local url=$1 file dir
	file=$SRCDIR/$(basename "$url")
	if [ ! -f "$file" ]; then
		# the mirrors drop connections now and then: retry on any error
		curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	# (tar is cut short by head: its SIGPIPE is not a failure)
	dir=$( (tar tf "$file" || true) | head -1 | cut -d/ -f1)
	[ -d "$SRCDIR/$dir" ] || tar xf "$file" -C "$SRCDIR"
	echo "$SRCDIR/$dir"
}

# a package that is already installed (its stamp exists) is skipped, so that
# a failed run can be resumed and a cached prefix is not rebuilt
done_already () { [ -f "$PREFIX/.stamp-$1" ]; }
stamp () { touch "$PREFIX/.stamp-$1"; echo "=== $1 installed"; }

meson_pkg () {
	local name=$1 url=$2; shift 2
	done_already "$name" && return 0
	echo "=== $name"
	local src; src=$(fetch "$url")
	rm -rf "$src/build-android"
	meson setup "$src/build-android" "$src" --cross-file "$CROSS" \
		--prefix "$PREFIX" --libdir lib --buildtype release \
		-Ddefault_library=shared --wrap-mode=default "$@"
	ninja -C "$src/build-android" -j "$JOBS"
	ninja -C "$src/build-android" install
	stamp "$name"
}

cmake_pkg () {
	local name=$1 url=$2; shift 2
	done_already "$name" && return 0
	echo "=== $name"
	local src; src=$(fetch "$url")
	local top=$src
	if [ -n "${CMAKE_SUBDIR:-}" ]; then top=$src/$CMAKE_SUBDIR; fi
	rm -rf "$src/build-android"
	cmake -S "$top" -B "$src/build-android" "${CMAKE_ARGS[@]}" "$@"
	cmake --build "$src/build-android" -j "$JOBS"
	cmake --install "$src/build-android"
	stamp "$name"
}

# a package without a usable build system for Android gets a CMakeLists.txt
# of its own from tools/android/cmake/<name>
cmake_own_pkg () {
	local name=$1 url=$2; shift 2
	done_already "$name" && return 0
	echo "=== $name"
	local src; src=$(fetch "$url")
	cp "$HERE/cmake/$name/CMakeLists.txt" "$src/CMakeLists.txt"
	rm -rf "$src/build-android"
	cmake -S "$src" -B "$src/build-android" "${CMAKE_ARGS[@]}" "$@"
	cmake --build "$src/build-android" -j "$JOBS"
	cmake --install "$src/build-android"
	stamp "$name"
}

# --- zlib ---------------------------------------------------------------------

# zlib is part of the NDK, but it has no .pc file there, and packages that
# found it through CMake (curl, libxml2) name it in their own .pc files.
mkdir -p "$PREFIX/lib/pkgconfig"
cat > "$PREFIX/lib/pkgconfig/zlib.pc" <<'PC'
Name: zlib
Description: zlib from the Android NDK
Version: 1.2.13
Libs: -lz
Cflags:
PC

# --- GLib family --------------------------------------------------------------

# libffi, pcre2 and proxy-libintl come from glib's own meson subprojects;
# zlib and iconv (API 28 and later) come from the NDK.
meson_pkg glib https://download.gnome.org/sources/glib/2.78/glib-2.78.6.tar.xz \
	-Dtests=false -Dinstalled_tests=false -Dnls=disabled -Dman=false \
	-Dgtk_doc=false -Dselinux=disabled -Dxattr=false -Dlibmount=disabled \
	-Dlibelf=disabled -Ddtrace=false -Dsystemtap=false -Dsysprof=disabled \
	-Dglib_debug=disabled -Doss_fuzz=disabled

meson_pkg sigcpp https://download.gnome.org/sources/libsigc++/2.12/libsigc++-2.12.1.tar.xz \
	-Dbuild-examples=false -Dbuild-tests=false -Dbuild-documentation=false \
	-Dmaintainer-mode=false -Dvalidation=false

meson_pkg glibmm https://download.gnome.org/sources/glibmm/2.66/glibmm-2.66.7.tar.xz \
	-Dbuild-examples=false -Dbuild-documentation=false -Dmaintainer-mode=false

# --- codecs and audio files ---------------------------------------------------

cmake_pkg ogg https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz \
	-DINSTALL_DOCS=OFF
cmake_pkg vorbis https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz
cmake_pkg flac https://downloads.xiph.org/releases/flac/flac-1.4.3.tar.xz \
	-DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF \
	-DINSTALL_MANPAGES=OFF -DBUILD_CXXLIBS=OFF -DWITH_OGG=ON
cmake_pkg opus https://downloads.xiph.org/releases/opus/opus-1.4.tar.gz \
	-DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
cmake_pkg sndfile https://github.com/libsndfile/libsndfile/releases/download/1.2.2/libsndfile-1.2.2.tar.xz \
	-DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_EXTERNAL_LIBS=ON \
	-DENABLE_MPEG=OFF -DENABLE_CPACK=OFF -DINSTALL_MANPAGES=OFF
cmake_pkg samplerate https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz \
	-DLIBSAMPLERATE_EXAMPLES=OFF

if [ "$CPU_FAMILY" = aarch64 ]; then FFTW_SIMD=-DENABLE_NEON=ON; else FFTW_SIMD=-DENABLE_SSE2=ON; fi
cmake_pkg fftw3f https://www.fftw.org/fftw-3.3.10.tar.gz \
	-DENABLE_FLOAT=ON -DENABLE_THREADS=ON -DWITH_COMBINED_THREADS=OFF \
	-DBUILD_TESTS=OFF $FFTW_SIMD

# --- files, network, metadata -------------------------------------------------

cmake_pkg xml2 https://download.gnome.org/sources/libxml2/2.12/libxml2-2.12.9.tar.xz \
	-DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ICU=OFF \
	-DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF \
	-DLIBXML2_WITH_ZLIB=ON

# no TLS backend yet: http only (see android/README.md)
cmake_pkg curl https://curl.se/download/curl-8.10.1.tar.xz \
	-DBUILD_CURL_EXE=OFF -DCURL_ENABLE_SSL=OFF -DCURL_USE_OPENSSL=OFF \
	-DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_LIBIDN2=OFF \
	-DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
	-DENABLE_CURL_MANUAL=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF

cmake_pkg archive https://github.com/libarchive/libarchive/releases/download/v3.7.7/libarchive-3.7.7.tar.xz \
	-DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
	-DENABLE_UNZIP=OFF -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF \
	-DENABLE_NETTLE=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF \
	-DENABLE_LZMA=OFF -DENABLE_BZip2=OFF -DENABLE_ZSTD=OFF -DENABLE_LZ4=OFF \
	-DENABLE_LIBB2=OFF -DENABLE_ICONV=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF \
	-DENABLE_CNG=OFF -DENABLE_WERROR=OFF

CMAKE_SUBDIR=cmake cmake_pkg lo https://downloads.sourceforge.net/project/liblo/liblo/0.32/liblo-0.32.tar.gz \
	-DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF -DWITH_CPP_TESTS=OFF

cmake_pkg taglib https://taglib.org/releases/taglib-1.13.1.tar.gz \
	-DBUILD_EXAMPLES=OFF -DBUILD_BINDINGS=OFF -DWITH_ZLIB=ON

# --- analysis and time stretching ---------------------------------------------

meson_pkg rubberband https://breakfastquay.com/files/releases/rubberband-3.3.0.tar.bz2 \
	-Dfft=builtin -Dresampler=builtin -Djni=disabled -Dladspa=disabled \
	-Dvamp=disabled -Dcmdline=disabled -Dtests=disabled

cmake_own_pkg vamp https://github.com/vamp-plugins/vamp-plugin-sdk/archive/refs/tags/vamp-plugin-sdk-v2.10.tar.gz
cmake_own_pkg aubio https://aubio.org/pub/aubio-0.4.9.tar.bz2

# --- LV2 ----------------------------------------------------------------------

meson_pkg lv2 https://lv2plug.in/spec/lv2-1.18.10.tar.xz \
	-Ddocs=disabled -Dplugins=disabled -Dtests=disabled -Donline_docs=false
meson_pkg zix https://download.drobilla.net/zix-0.4.2.tar.xz \
	-Ddocs=disabled -Dtests=disabled -Dtests_cpp=disabled -Dbenchmarks=disabled
meson_pkg serd https://download.drobilla.net/serd-0.32.2.tar.xz \
	-Ddocs=disabled -Dtests=disabled -Dtools=disabled -Dman=disabled
meson_pkg sord https://download.drobilla.net/sord-0.16.16.tar.xz \
	-Ddocs=disabled -Dtests=disabled -Dtools=disabled
meson_pkg sratom https://download.drobilla.net/sratom-0.6.16.tar.xz \
	-Ddocs=disabled -Dtests=disabled
meson_pkg lilv https://download.drobilla.net/lilv-0.24.24.tar.xz \
	-Ddocs=disabled -Dtests=disabled -Dtools=disabled -Dbindings_py=disabled \
	-Dbindings_cpp=disabled

# --- boost (header-only) ------------------------------------------------------

if ! done_already boost; then
	BOOST_INC=${BOOST_INCLUDE:-/usr/include}
	test -f "$BOOST_INC/boost/version.hpp" || { echo "boost headers not found in $BOOST_INC (install libboost-dev or set BOOST_INCLUDE)" >&2; exit 1; }
	rm -rf "$PREFIX/include/boost"
	cp -a "$BOOST_INC/boost" "$PREFIX/include/boost"
	stamp boost
fi

# --- the C++ runtime ----------------------------------------------------------

# every library above links against libc++_shared.so, so the APK has to carry it
cp "$TC/sysroot/usr/lib/$TRIPLE/libc++_shared.so" "$PREFIX/lib/"

echo "Android dependency stack for $ABI (API $API) is in $PREFIX"
ls "$PREFIX/lib"/*.so
