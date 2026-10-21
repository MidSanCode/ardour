# Ardour for Android

The Android application is a **WebView shell around the native Ardour engine**:

* the engine is `libardour` (plus the AAudio backend) cross-compiled for
  `arm64-v8a` with the Android NDK;
* the UI is the shared JavaScript frontend - the same code that runs in a
  browser against the wasm engine runs here against the native engine;
* the two talk over a small `JavascriptInterface` bridge
  (`org.ardour.ardour.EngineBridge`), which returns JSON so that the frontend
  can be evolved without rebuilding the native side.

## Layout

| Path | What it is |
| --- | --- |
| `app/src/main/java/org/ardour/ardour/MainActivity.kt` | the WebView activity |
| `app/src/main/java/org/ardour/ardour/EngineBridge.kt` | the JS <-> native bridge |
| `app/src/main/assets/index.html` | the placeholder page (the shared frontend replaces it) |
| `app/src/main/jniLibs/` | where the cross-compiled engine is copied to |

## Building

The native side is built by waf, from the top of the repository:

```sh
./waf configure --dist-target=android --with-ndk=/path/to/ndk --with-frontends=none
./waf build
```

That needs a cross-compiled dependency stack (glib, glibmm, boost, ...) and
`PKG_CONFIG_LIBDIR` pointing at it - see `_portability/plan.md` for the
dependency table.  The resulting `libardour_android.so` is copied into
`app/src/main/jniLibs/arm64-v8a/` by `tools/copy-android-engine.sh`.

The application itself is a normal Gradle project:

```sh
cd android
gradle assembleDebug          # or: ./gradlew assembleDebug
```

The Gradle wrapper is not committed (the wrapper jar is a binary); run
`gradle wrapper` once to generate it, or use a locally installed Gradle 8.6+.

The application **builds and starts without the engine**: `NativeEngine`
catches the `UnsatisfiedLinkError` and the page reports that the engine
library is missing.  That keeps the shell usable while the cross-compiled
dependency stack is still being assembled.

## Status

This is the transport skeleton, not a working DAW on a phone yet:

* the bridge exposes the engine information and the list of audio devices;
* the transport, the session and the editing operations are the next step
  (they need the protocol layer to be separated from the websocket server,
  see `_portability/plan.md` section 6);
* the audio backend (`libs/backends/aaudio`) is written but has never been run
  on a device.
