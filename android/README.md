# Ardour for Android

The Android application is a **thin native shell around the real Ardour
engine**: the engine is `libardour` (plus the AAudio backend) cross-compiled
with the NDK, and the UI is the shared JavaScript frontend running in a
`WebView` and talking to the engine over a JSON bridge.

* the engine is `libardour` plus the AAudio backend, cross-compiled for
  `arm64-v8a` and `x86_64` (see `tools/android/build-deps.sh`);
* the UI is `frontend/` - the same files that run in a browser against the
  wasm engine run here against the native engine, packaged into the APK as
  `assets/web`;
* the two talk over `org.ardour.ardour.EngineBridge`, a `JavascriptInterface`
  that returns JSON, so the frontend can be changed without a native rebuild.

## Layout

| Path | What it is |
| --- | --- |
| `app/src/main/java/org/ardour/ardour/ArdourApplication.kt` | process setup |
| `app/src/main/java/org/ardour/ardour/MainActivity.kt` | the WebView activity |
| `app/src/main/java/org/ardour/ardour/EngineHost.kt` | starts/stops the engine, owns the session |
| `app/src/main/java/org/ardour/ardour/NativeEngine.kt` | the `external` JNI declarations |
| `app/src/main/java/org/ardour/ardour/EngineBridge.kt` | the JS <-> native bridge |
| `app/src/main/java/org/ardour/ardour/MidiInput.kt` | external MIDI devices |
| `app/src/main/java/org/ardour/ardour/SmokeTest.kt` | the emulator smoke test |
| `app/src/main/jniLibs/<abi>/` | the cross-compiled engine and its libraries |
| `app/src/main/engine-assets/ardour/` | the engine's data files (templates, scripts) |
| `frontend/` | the shared UI (copied to `assets/web` at build time) |

## The JSON protocol

`EngineBridge.call(method, params)` takes a method name and a JSON object and
returns one JSON object.  The methods are implemented in
`libs/android/engine_protocol.cc`; the families are:

| Family | Methods |
| --- | --- |
| engine | `engine.status`, `engine.buffer` |
| session | `session.list`, `session.create`, `session.open`, `session.save`, `session.close`, `session.info`, `session.rename`, `session.delete`, `session.save_as`, `session.templates`, `session.save_template`, `session.delete_template` |
| transport | `transport.play`, `transport.stop`, `transport.state`, `transport.record`, `transport.locate` |
| tracks / buses | `tracks.list`, `tracks.add`, `tracks.remove`, `buses.add` |
| mixer | `mixer.strips`, `strip.set` |
| timeline | `timeline` |
| regions | `regions.tone`, `regions.import`, `regions.midi`, `region.move`, `region.split`, `region.remove`, `region.mute`, `region.trim`, `region.fades`, `region.peaks` |
| MIDI notes | `region.notes`, `region.note.add`, `region.note.edit`, `region.note.remove` |
| plugins | `plugins.list`, `plugin.add`, `plugin.get`, `plugin.set`, `plugin.remove`, `plugin.presets`, `plugin.preset.load`, `plugin.preset.save` |
| automation | `automation.get`, `automation.state`, `automation.add`, `automation.remove`, `automation.clear` |
| editing | `edit.undo`, `edit.redo` |
| export | `export.presets`, `export.audio`, `export.start`, `export.status`, `export.cancel` |
| musical timeline | `tempo.get`, `tempo.set`, `meter.set`, `loop.set`, `loop.clear`, `click.set`, `grid` |
| markers | `markers.list`, `marker.add`, `marker.move`, `marker.remove` |
| meters / MIDI | `meters`, `midi.send` |

## Building

The native side is built by waf, from the top of the repository:

```sh
./waf configure --dist-target=android --with-ndk=/path/to/ndk \
  --android-deps=/path/to/cross-built/deps --with-frontends=none --no-tk-stack
./waf build
```

`tools/android/build-deps.sh <abi> <prefix>` cross-compiles the dependency
stack (glib, glibmm, boost, ...) into `<prefix>`; that prefix is what
`--android-deps` (or `$ANDROID_DEPS`) points at.
`tools/android/collect-engine.sh` then copies the built libraries and data
files into the Gradle project.  Both are what the `android` CI jobs run.

The application itself is a normal Gradle project; the wrapper jar is not
committed, so use a Gradle 8.7+ installed locally or `gradle wrapper` once:

```sh
cd android
gradle assembleDebug
```

The application **builds and starts without the engine**: `NativeEngine`
catches the `UnsatisfiedLinkError` and the UI reports that the engine library
is missing, so a shell-only build is still usable.

## What has actually been verified

Verified by CI (`.github/workflows/build.yml`, jobs `android`, `android-apk`,
`android-emulator`):

* the engine configures and builds for **`arm64-v8a` and `x86_64`** with the
  NDK and the cross-compiled dependency stack;
* the APK contains `lib/<abi>/libardour.so`, `lib/<abi>/libardour_android.so`
  and `assets/web/index.html`;
* on **API 28 and API 30 x86_64 emulators**, `SmokeTest.kt` starts the engine,
  creates a session, adds tracks and regions, rolls the transport and drives the
  mixer and plugin panels entirely through the JSON protocol, then the
  frontend's editor / mixer / piano-roll / plugin / automation views are checked
  and screenshotted.  The run asserts `ARDOUR_SMOKE PASS` and the structured
  lines: `ARDOUR_UI ...`, `ARDOUR_AUDIO ...`, `ARDOUR_EXPORT ...`,
  `ARDOUR_EXPORT_ASYNC ...` (a render that can be cancelled while it runs),
  `ARDOUR_SENDS ...`, `ARDOUR_SERVICE ...` (foreground, and the cpu kept awake
  while the transport rolls), `ARDOUR_BUFFER ...` (a block size change and back)
  and `ARDOUR_SESSIONS ...` (describe, copy, rename, template, delete).
* on an **arm64 phone** (Redmi Note 9, Android 12) the same smoke test passes
  with the engine on AAudio, which is what `tools/android/device-check.sh`
  automates.

**Not verified yet** - do not read more into the above than it says:

* **audio output.**  The emulator job runs with `-noaudio`, so the AAudio
  stream is never opened there; whether sound actually comes out of a device
  is untested.  The arm64 run opened a real stream and moved the meters, and
  AudioFlinger reported underruns - but nobody has listened to it.
* **real devices / real audio hardware**: USB and Bluetooth devices, sample
  rate changes, xruns, and the latency the AAudio backend asks for.
* **MIDI from real hardware** (`midi.send` exists; the input path is newer than
  the emulator coverage).
* API levels **below 28** (`minSdk` is 28): the AAudio device *enumeration*
  only exists from API 30 on and the code falls back to the default devices
  below that.  API 28 is exercised by CI; the fallback path is what it takes.

`tools/android/device-check.sh` is the starting point for the real-device
work: it installs the APK, runs the same engine startup and protocol smoke
test, and additionally asserts the things an emulator cannot show - that an
AAudio stream opened with the device's own sample rate, that the session rolls
without xruns, and that meters move.
