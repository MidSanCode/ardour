package org.ardour.ardour

import android.Manifest
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.ViewGroup
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject
import java.io.File

/**
 * The Ardour Android UI: the shared JavaScript frontend (frontend/ at the top
 * of the repository, packaged as the "web" assets) in a WebView, talking to
 * the native engine through EngineBridge.
 */
class MainActivity : AppCompatActivity() {

    private var webView: WebView? = null
    private var pendingPick = 0

    /* a sound file chosen by the user is copied into the cache, where the
     * engine can open it by path, and the path is handed to the page */
    private val openAudio = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val id = pendingPick
        Thread {
            val path = uri?.let { copyToCache(it) }
            runOnUiThread { answerPick(id, path) }
        }.start()
    }

    private fun answerPick(id: Int, path: String?) {
        val arg = if (path == null) "null" else JSONObject.quote(path)
        webView?.evaluateJavascript("window.ArdourBridge && ArdourBridge.picked($id, $arg)", null)
    }

    private fun displayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val name = c.getString(0)
                if (!name.isNullOrBlank()) return name.replace('/', '_')
            }
        }
        return "imported.wav"
    }

    private fun copyToCache(uri: Uri): String? = try {
        val dir = File(cacheDir, "imports").apply { mkdirs() }
        val out = File(dir, displayName(uri))
        contentResolver.openInputStream(uri)?.use { input ->
            out.outputStream().use { input.copyTo(it) }
        }
        out.absolutePath
    } catch (t: Throwable) {
        Log.w("Ardour", "cannot copy the chosen file: ${t.message}")
        null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val host = (application as ArdourApplication).engine
        host.start()

        val view = WebView(this)
        view.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )
        view.settings.javaScriptEnabled = true
        view.settings.domStorageEnabled = true
        view.settings.allowFileAccess = true
        view.webViewClient = WebViewClient()
        view.addJavascriptInterface(EngineBridge(host) { id ->
            runOnUiThread {
                pendingPick = id
                try {
                    openAudio.launch(arrayOf("audio/*"))
                } catch (t: Throwable) {
                    answerPick(id, null)
                }
            }
        }, "ArdourNative")
        view.loadUrl("file:///android_asset/web/index.html")

        webView = view
        setContentView(view)

        /* recording needs the microphone; the engine opens its input when it
         * starts, so a grant takes effect the next time the app starts */
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED &&
            intent?.getBooleanExtra("smoketest", false) != true) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        }

        if (intent?.getBooleanExtra("smoketest", false) == true) {
            SmokeTest.run(host, cacheDir) { passed -> if (passed) runOnUiThread { showViews() } }
        }
    }

    /**
     * After a passing smoke test: show the editor and then the mixer, and log
     * what the frontend drew for each, so CI can take a screenshot of both and
     * check that the views were filled in.
     */
    private fun showViews() {
        val handler = Handler(Looper.getMainLooper())
        val views = listOf("editor", "mixer")
        views.forEachIndexed { i, name ->
            handler.postDelayed({
                webView?.evaluateJavascript("window.ArdourApp && ArdourApp.show('$name')", null)
                if (name == "editor") {
                    handler.postDelayed({
                        webView?.evaluateJavascript("window.ArdourApp && ArdourApp.selectLastRegion()", null)
                    }, 1500L)
                }
            }, 2000L + i * 8000L)
            handler.postDelayed({
                webView?.evaluateJavascript("window.ArdourApp ? ArdourApp.describe() : 'no frontend'") { value ->
                    Log.i("ArdourUI", "ARDOUR_UI $name $value")
                }
            }, 5000L + i * 8000L)
        }
    }

    override fun onDestroy() {
        webView?.destroy()
        webView = null
        super.onDestroy()
    }
}
