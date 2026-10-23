package org.ardour.ardour

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.ViewGroup
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.appcompat.app.AppCompatActivity

/**
 * The Ardour Android UI: the shared JavaScript frontend (frontend/ at the top
 * of the repository, packaged as the "web" assets) in a WebView, talking to
 * the native engine through EngineBridge.
 */
class MainActivity : AppCompatActivity() {

    private var webView: WebView? = null

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
        view.addJavascriptInterface(EngineBridge(host), "ArdourNative")
        view.loadUrl("file:///android_asset/web/index.html")

        webView = view
        setContentView(view)

        if (intent?.getBooleanExtra("smoketest", false) == true) {
            SmokeTest.run(host) { passed -> if (passed) runOnUiThread { showViews() } }
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
