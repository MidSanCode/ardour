# aubio builds with its own copy of waf, which cannot target Android; this
# builds the library the way its wscript does with no optional dependency:
# the built-in ooura FFT and the built-in WAV reader/writer.
cmake_minimum_required(VERSION 3.10)
project(aubio C)

file(GLOB_RECURSE AUBIO_SOURCES src/*.c)
file(WRITE ${CMAKE_BINARY_DIR}/config.h "
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_MATH_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDARG_H 1
#define HAVE_C99_VARARGS_MACROS 1
#define HAVE_WAVREAD 1
#define HAVE_WAVWRITE 1
#define HAVE_MEMCPY_HACKS 1
")

add_library(aubio ${AUBIO_SOURCES})
target_include_directories(aubio PRIVATE ${CMAKE_BINARY_DIR} src)
target_compile_definitions(aubio PRIVATE HAVE_CONFIG_H=1)
target_link_libraries(aubio m)

install(TARGETS aubio LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY src/ DESTINATION include/aubio FILES_MATCHING PATTERN "*.h" PATTERN "aubio_priv.h" EXCLUDE)

file(WRITE ${CMAKE_BINARY_DIR}/aubio.pc "prefix=${CMAKE_INSTALL_PREFIX}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: aubio
Description: a library for audio labelling
Version: 0.4.9
Libs: -L\${libdir} -laubio
Cflags: -I\${includedir}
")
install(FILES ${CMAKE_BINARY_DIR}/aubio.pc DESTINATION lib/pkgconfig)
