package org.ardour.ardour

/** The native half of the bridge: libardour_android.so, see libs/android. */
object NativeEngine {

    @Volatile
    var available: Boolean = false
        private set

    var loadError: String? = null
        private set

    init {
        try {
            // libardour_android.so needs libardour.so, glib, ... from the same
            // directory; the dynamic linker finds those on its own.
            System.loadLibrary("ardour_android")
            available = true
        } catch (e: UnsatisfiedLinkError) {
            loadError = e.message
            available = false
        }
    }

    /** Engine version, target platform and build information, as JSON. */
    external fun nativeInfo(): String

    /** The audio devices AAudio can see, as a JSON array. */
    external fun nativeDevices(): String

    /** Initialises libardour and starts the audio engine; returns its status as JSON. */
    external fun nativeInit(libDir: String, dataDir: String, userDir: String, cacheDir: String): String

    /** One request of the frontend protocol. */
    external fun nativeCall(method: String, params: String): String
}
