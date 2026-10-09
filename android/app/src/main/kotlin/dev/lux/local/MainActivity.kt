package dev.lux.local

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient

class MainActivity : Activity() {
    private lateinit var web: WebView
    private var chooser: ValueCallback<Array<Uri>>? = null

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        LuxLocal.appContext = applicationContext
        askOnce()
        SyncService.start(this)   // keeps syncing (and notifying) after this Activity is gone
        web = WebView(this)
        web.settings.javaScriptEnabled = true
        web.settings.domStorageEnabled = true
        // Anything that is not the app's own server (links in an email) goes to the system browser.
        web.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(v: WebView, r: WebResourceRequest): Boolean {
                if (r.url.host == "127.0.0.1") return false
                runCatching { startActivity(Intent(Intent.ACTION_VIEW, r.url)) }
                return true
            }
        }
        // <input type=file> (attachments, inline images) needs the host to show a picker.
        web.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(v: WebView, cb: ValueCallback<Array<Uri>>, p: FileChooserParams): Boolean {
                chooser?.onReceiveValue(null)
                chooser = cb
                return try { startActivityForResult(p.createIntent(), 1); true }
                catch (e: Exception) { chooser = null; false }
            }
        }
        web.setBackgroundColor(getColor(R.color.app_background))   // no white flash while the server boots
        setContentView(web)
        // Compiling the app and opening SQLite can take a few seconds on a cold start: not on the UI thread.
        // start() is idempotent: if this process already runs the server (the Activity was only recreated), it returns its port.
        Thread {
            val port = LuxLocal.start(filesDir.path)
            runOnUiThread { if (port < 0) finish() else web.loadUrl("http://127.0.0.1:$port/") }
        }.start()
    }

    // First launch only: notification permission (Android 13+) and exemption from battery optimisation,
    // without which Doze delays the background sync by tens of minutes.
    private fun askOnce() {
        val prefs = getSharedPreferences("luxlocal", MODE_PRIVATE)
        if (prefs.getBoolean("asked", false)) return
        prefs.edit().putBoolean("asked", true).apply()
        if (android.os.Build.VERSION.SDK_INT >= 33) requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 2)
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, res: IntArray) {
        super.onRequestPermissionsResult(code, perms, res)
        val pm = getSystemService(android.os.PowerManager::class.java)
        if (code == 2 && !pm.isIgnoringBatteryOptimizations(packageName))
            runCatching { startActivity(Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))) }
    }

    override fun onActivityResult(code: Int, result: Int, data: Intent?) {
        if (code == 1) { chooser?.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(result, data)); chooser = null }
        else super.onActivityResult(code, result, data)
    }

    // The page decides what "back" means (close dialog / tab); it answers true if it handled it.
    @Deprecated("fine for a plain Activity")
    override fun onBackPressed() {
        web.evaluateJavascript("window.luxBack ? window.luxBack() : false") { handled ->
            if (handled != "true") super.onBackPressed()
        }
    }

    // The server is deliberately NOT stopped when the Activity finishes: the process outlives it, and the next
    // launch reuses the running server (restarting Lux inside the same process is not supported).
}
