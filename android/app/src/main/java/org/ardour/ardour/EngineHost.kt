package org.ardour.ardour

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.Future

/**
 * Owns the engine for the application: unpacks its data files, initialises
 * it once on a background thread, and forwards protocol requests to it.
 *
 * Until the engine is up, "engine.status" is answered here, so that the
 * frontend can show progress or the reason it failed.
 */
class EngineHost(private val context: Context) {

    private val executor = Executors.newSingleThreadExecutor()

    @Volatile private var initReply: String? = null
    @Volatile private var initError: String? = null
    private var initFuture: Future<*>? = null

    val ready: Boolean get() = initReply != null && initError == null

    @Synchronized
    fun start(): Future<*> {
        initFuture?.let { return it }
        val f = executor.submit {
            try {
                if (!NativeEngine.available) {
                    initError = "the engine library did not load: ${NativeEngine.loadError}"
                    return@submit
                }
                val dataDir = unpackData()
                val userDir = File(context.filesDir, "user").apply { mkdirs() }
                val reply = NativeEngine.nativeInit(
                    context.applicationInfo.nativeLibraryDir,
                    dataDir.absolutePath,
                    userDir.absolutePath,
                    context.cacheDir.absolutePath
                )
                val json = JSONObject(reply)
                if (json.optBoolean("ok", false)) {
                    initReply = reply
                } else {
                    initError = json.optString("error", reply)
                }
                Log.i(TAG, "engine init: $reply")
            } catch (t: Throwable) {
                initError = t.toString()
                Log.e(TAG, "engine init failed", t)
            }
        }
        initFuture = f
        return f
    }

    fun call(method: String, params: String): String {
        val err = initError
        if (err != null) {
            return JSONObject().put("ok", false).put("error", err).toString()
        }
        if (initReply == null) {
            if (method == "engine.status") {
                return JSONObject().put("ok", true).put("initialized", false)
                    .put("starting", true).toString()
            }
            return JSONObject().put("ok", false).put("error", "the engine is starting").toString()
        }
        return NativeEngine.nativeCall(method, params.ifEmpty { "{}" })
    }

    /**
     * The engine's data files are in the APK's assets (assets/ardour); the
     * engine reads them from the file system, so they are copied out once per
     * application version.
     */
    private fun unpackData(): File {
        val dest = File(context.filesDir, "share")
        val stamp = File(dest, ".version")
        val version = context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime.toString()
        if (stamp.isFile && stamp.readText() == version) {
            return dest
        }
        dest.deleteRecursively()
        dest.mkdirs()
        copyAssets(context.assets, "ardour", dest)
        /* the LV2 bundles name their binaries through a placeholder for the
         * native library directory (tools/android/collect-engine.sh) */
        val libDir = context.applicationInfo.nativeLibraryDir
        File(dest, "LV2").walkTopDown().filter { it.isFile && it.name.endsWith(".ttl") }.forEach { ttl ->
            val text = ttl.readText()
            if (text.contains("@ARDOUR_NATIVE_LIB_DIR@")) {
                ttl.writeText(text.replace("@ARDOUR_NATIVE_LIB_DIR@", libDir))
            }
        }
        stamp.writeText(version)
        return dest
    }

    private fun copyAssets(assets: AssetManager, path: String, dest: File) {
        val children = assets.list(path) ?: return
        if (children.isEmpty()) {
            // a file (or an empty directory, which assets cannot tell apart)
            try {
                assets.open(path).use { input -> dest.outputStream().use { input.copyTo(it) } }
            } catch (e: java.io.IOException) {
                dest.mkdirs()
            }
            return
        }
        dest.mkdirs()
        for (child in children) {
            copyAssets(assets, "$path/$child", File(dest, child))
        }
    }

    companion object {
        const val TAG = "ArdourEngine"
    }
}
