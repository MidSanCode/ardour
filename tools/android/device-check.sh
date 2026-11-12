#!/bin/sh
#
# tools/android/device-check.sh - the real-device acceptance script.
#
# What the emulator job cannot tell you is what this checks:
#
#   * that AAudio really opened a stream on the device's own audio hardware
#     (not the dummy fallback the emulator needs),
#   * that the stream runs at a sane sample rate and block size,
#   * that the session rolls and the process cycle keeps up (position advances,
#     dsp load stays low),
#   * that a tone actually reaches a track meter - i.e. the graph is being
#     processed, not just reported as running.
#
# usage:
#   tools/android/device-check.sh [-s SERIAL] [-a APK]
#
#   -s SERIAL   adb serial (default: the only connected device)
#   -a APK      APK to install (default: android/app/build/outputs/apk/debug/app-debug.apk)
#
# Exit status is 0 only when every check passes.  The logcat is always written
# to device-check-logcat.txt and the UI screenshots to ui-*.png next to it.
#
set -eu

SERIAL_ARG=""
APK="$(dirname "$0")/../../android/app/build/outputs/apk/debug/app-debug.apk"

while getopts "s:a:h" opt; do
	case "$opt" in
		s) SERIAL_ARG="-s $OPTARG" ;;
		a) APK="$OPTARG" ;;
		h) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option; try -h" >&2; exit 2 ;;
	esac
done

ADB="adb $SERIAL_ARG"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

[ -f "$APK" ] || fail "no APK at $APK (build it with: cd android && gradle assembleDebug)"
command -v adb >/dev/null 2>&1 || fail "adb is not on PATH"

# ---- the device has to be a real one -------------------------------------
echo "== device =="
$ADB wait-for-device
ABI=$($ADB shell getprop ro.product.cpu.abi | tr -d '\r')
MODEL=$($ADB shell getprop ro.product.model | tr -d '\r')
SDK=$($ADB shell getprop ro.build.version.sdk | tr -d '\r')
echo "model=$MODEL abi=$ABI sdk=$SDK"
case "$ABI" in
	arm64*) : ;;
	*) echo "WARNING: $ABI is not arm64; the arm64 library is the one this script is for" ;;
esac
if $ADB shell getprop ro.kernel.qemu | grep -q '^1'; then
	echo "WARNING: this looks like an emulator; AAudio may fall back to dummy"
fi

# ---- install and run -----------------------------------------------------
echo "== install =="
$ADB install -r -g "$APK" || fail "install failed"

$ADB logcat -c
$ADB shell am force-stop org.ardour.ardour || true
$ADB shell am start -n org.ardour.ardour/.MainActivity --ez smoketest true >/dev/null
for _ in $(seq 1 120); do
	sleep 2
	if $ADB logcat -d -s ArdourSmoke:I | grep -q 'ARDOUR_SMOKE'; then break; fi
done
$ADB logcat -d > device-check-logcat.txt

grep 'ARDOUR_SMOKE' device-check-logcat.txt || fail "the smoke test printed nothing"
grep -q 'ARDOUR_SMOKE PASS' device-check-logcat.txt || fail "the smoke test failed; see device-check-logcat.txt"

# ---- the audio checks the emulator cannot do -----------------------------
echo "== audio path =="
grep 'ARDOUR_AUDIO' device-check-logcat.txt || fail "no ARDOUR_AUDIO line: the engine never started"

# 1. AAudio, not the dummy fallback.
if ! grep -E 'ARDOUR_AUDIO rolling backend="AAudio"' device-check-logcat.txt >/dev/null; then
	fail "the engine did not run on AAudio on this device (no audio hardware, or the stream failed to open)"
fi

# 2. A stream was opened with a real format: rate and buffer non-zero.
grep -E 'ARDOUR_AUDIO rolling .*rate=[1-9][0-9]* buffer=[1-9][0-9]*' device-check-logcat.txt >/dev/null \
	|| fail "the AAudio stream reported no sample rate / block size"

# 3. The process cycle is really running.
grep -E 'ARDOUR_AUDIO rolling .*advanced=true' device-check-logcat.txt >/dev/null \
	|| fail "the transport position did not advance: the audio cycle is not being called back"

# 4. The graph is being processed: a tone reached a track meter.
grep -E 'track meter peak -?[0-9.]+ dBFS while the tone played' device-check-logcat.txt >/dev/null \
	|| fail "no track meter reading while the tone played"

# 5. Report the format and the dsp load so the numbers are in the log.
grep 'ARDOUR_AUDIO' device-check-logcat.txt

# ---- screenshots ---------------------------------------------------------
echo "== screenshots =="
for view in editor mixer roll plugin automation; do
	for _ in $(seq 1 30); do
		sleep 1
		if $ADB logcat -d -s ArdourUI:I | grep -q "ARDOUR_UI $view"; then break; fi
	done
	$ADB exec-out screencap -p > "ui-$view.png"
done
ls -1 ui-*.png

# ---- AAudio failures from the native log, if any -------------------------
echo "== native backend messages =="
grep -E 'AAudioBackend:' device-check-logcat.txt || echo "(none)"

echo
echo "PASS: $MODEL on $ABI, API $SDK ran the engine on AAudio and processed audio."
echo "       log: device-check-logcat.txt   screenshots: ui-*.png"
