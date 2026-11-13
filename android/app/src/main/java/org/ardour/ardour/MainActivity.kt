package org.ardour.ardour

import android.Manifest
import android.content.Intent
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
import androidx.core.content.ContextCompat
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
    private var pendingSave = 0
    private var saving: String? = null

    /* a sound file chosen by the user is copied into the cache, where the
     * engine can open it by path, and the path is handed to the page */
    private val openAudio = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val id = pendingPick
        Thread {
            val path = uri?.let { copyToCache(it) }
            runOnUiThread { answerPick(id, path) }
        }.start()
    }

    /* an export the user wants to keep is copied out of the cache into the
     * place they pick (Downloads, Drive, ...); the engine only writes inside
     * the application's own directories */
    private val createFile = registerForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri ->
        val id = pendingSave
        val source = saving
        saving = null
        Thread {
            val where = if (uri != null && source != null && copyToDocument(source, uri)) uri.toString() else null
            runOnUiThread { answerSave(id, where) }
        }.start()
    }

    private fun answerPick(id: Int, path: String?) {
        val arg = if (path == null) "null" else JSONObject.quote(path)
        webView?.evaluateJavascript("window.ArdourBridge && ArdourBridge.picked($id, $arg)", null)
    }

    private fun answerSave(id: Int, where: String?) {
        val arg = if (where == null) "null" else JSONObject.quote(where)
        webView?.evaluateJavascript("window.ArdourBridge && ArdourBridge.saved($id, $arg)", null)
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

    private fun copyToDocument(path: String, uri: Uri): Boolean = try {
        val source = File(path)
        if (!source.isFile) {
            false
        } else {
            contentResolver.openOutputStream(uri, "wt")?.use { output ->
                source.inputStream().use { input -> input.copyTo(output) }
            }
            Log.i("Ardour", "saved $path to $uri")
            true
        }
    } catch (t: Throwable) {
        Log.w("Ardour", "cannot save the file: ${t.message}")
        false
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        /* the engine runs under a foreground service so that it survives the
         * screen going off and the user switching away; the service also owns
         * the audio focus */
        ContextCompat.startForegroundService(this, Intent(this, ArdourService::class.java))

        val host = (application as ArdourApplication).engine
        host.start()
        (application as ArdourApplication).midi

        val view = WebView(this)
        view.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )
        view.settings.javaScriptEnabled = true
        view.settings.domStorageEnabled = true
        view.settings.allowFileAccess = true
        view.webViewClient = WebViewClient()
        view.addJavascriptInterface(EngineBridge(host, { id ->
            runOnUiThread {
                pendingPick = id
                try {
                    openAudio.launch(arrayOf("audio/*"))
                } catch (t: Throwable) {
                    answerPick(id, null)
                }
            }
        }, { id, path, name ->
            runOnUiThread {
                pendingSave = id
                saving = path
                try {
                    createFile.launch(name.ifEmpty { File(path).name })
                } catch (t: Throwable) {
                    saving = null
                    answerSave(id, null)
                }
            }
        }), "ArdourNative")
        view.loadUrl("file:///android_asset/web/index.html")

        webView = view
        setContentView(view)

        /* recording needs the microphone; the engine opens its input when it
         * starts, so a grant takes effect the next time the app starts */
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED &&
            intent?.getBooleanExtra("smoketest", false) != true) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 1)
        }

        if (android.os.Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED &&
            intent?.getBooleanExtra("smoketest", false) != true) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 2)
        }

        if (intent?.getBooleanExtra("smoketest", false) == true) {
            SmokeTest.run(host, cacheDir) { passed -> if (passed) runOnUiThread { showViews() } }
        }
    }

    /**
     * After a passing smoke test: show the editor, the mixer, the piano roll
     * and a plugin's controls, and log
     * what the frontend drew for each, so CI can take a screenshot of both and
     * check that the views were filled in.
     */
    private fun showViews() {
        val handler = Handler(Looper.getMainLooper())
        val views = listOf("editor", "mixer", "roll", "plugin", "automation")
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
