package org.ardour.ardour

import android.os.Bundle
import android.view.ViewGroup
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.appcompat.app.AppCompatActivity

/**
 * The Ardour Android UI.
 *
 * The UI is the shared JavaScript frontend: the same code that runs in a
 * browser against the wasm engine runs here against the native engine, over
 * the JavascriptInterface bridge in EngineBridge.
 *
 * The page is loaded from the assets, so the application works without any
 * network access; the websocket control surface (if it is enabled) is
 * reachable on the loopback interface.
 */
class MainActivity : AppCompatActivity() {

    private var webView: WebView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val view = WebView(this)
        view.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )
        view.settings.javaScriptEnabled = true
        view.settings.domStorageEnabled = true
        view.settings.allowFileAccess = true
        view.webViewClient = WebViewClient()
        view.addJavascriptInterface(EngineBridge(this), "ArdourNative")
        view.loadUrl("file:///android_asset/index.html")

        webView = view
        setContentView(view)
    }

    override fun onBackPressed() {
        val view = webView
        if (view != null && view.canGoBack()) {
            view.goBack()
        } else {
            super.onBackPressed()
        }
    }

    override fun onDestroy() {
        webView?.destroy()
        webView = null
        super.onDestroy()
    }
}
