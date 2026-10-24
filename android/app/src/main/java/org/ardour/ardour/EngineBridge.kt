package org.ardour.ardour

import android.webkit.JavascriptInterface

/**
 * The bridge between the JavaScript frontend and the native engine.
 *
 * The frontend (frontend/ at the top of the repository, shared with the iOS
 * and browser builds) only knows one entry point, call(method, params), which
 * takes and returns JSON; frontend/bridge.js maps it onto whatever the host
 * provides.  The methods are documented in libs/android/engine_protocol.h.
 *
 * pickAudioFile(id) asks the host to let the user choose a sound file; the
 * answer arrives as ArdourBridge.picked(id, path) in the page, with path null
 * when nothing was chosen.
 */
class EngineBridge(private val host: EngineHost, private val picker: (Int) -> Unit) {

    @JavascriptInterface
    fun call(method: String, params: String): String = host.call(method, params)

    @JavascriptInterface
    fun platform(): String = "android"

    @JavascriptInterface
    fun pickAudioFile(id: Int) = picker(id)
}
