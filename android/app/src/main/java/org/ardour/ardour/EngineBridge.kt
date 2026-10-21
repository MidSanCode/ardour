package org.ardour.ardour

import android.content.Context
import android.webkit.JavascriptInterface

/**
 * The bridge between the JavaScript frontend and the native engine.
 *
 * The frontend only knows this interface: it is the same contract that the
 * browser build uses to talk to the wasm engine, so the UI code does not have
 * to know whether it runs in a browser or on a device.
 *
 * Every method returns JSON, so that the frontend can parse it with
 * JSON.parse() and evolve without a native rebuild.
 */
class EngineBridge(private val context: Context) {

    @JavascriptInterface
    fun isEngineAvailable(): Boolean = NativeEngine.available

    @JavascriptInterface
    fun getEngineInfo(): String =
        if (NativeEngine.available) NativeEngine.nativeInfo() else missingEngineJson()

    @JavascriptInterface
    fun getAudioDevices(): String =
        if (NativeEngine.available) NativeEngine.nativeDevices() else "[]"

    private fun missingEngineJson(): String =
        "{\"error\":\"the Ardour engine library is not present in this build\"}"
}

/**
 * The native half of the bridge.
 *
 * The library is optional: an application built without the engine still
 * starts and reports that it is missing, which keeps the WebView shell usable
 * while the cross-compiled dependency stack is still being put together.
 */
object NativeEngine {

    @Volatile
    var available: Boolean = false
        private set

    init {
        try {
            System.loadLibrary("ardour_android")
            available = true
        } catch (e: UnsatisfiedLinkError) {
            available = false
        }
    }

    /** Engine version, target platform and build information, as JSON. */
    external fun nativeInfo(): String

    /** The audio devices the AAudio backend can see, as a JSON array. */
    external fun nativeDevices(): String
}
