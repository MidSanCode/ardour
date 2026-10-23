package org.ardour.ardour

import android.os.Bundle
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

        if (intent?.getBooleanExtra("smoketest", false) == true) {
            SmokeTest.run(host)
        }

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
    }

    override fun onDestroy() {
        webView?.destroy()
        webView = null
        super.onDestroy()
    }
}
