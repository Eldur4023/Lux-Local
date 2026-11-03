package dev.lux.local

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import java.io.File
import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import android.content.Intent
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** JNI bridge to libluxlocal.so (src/android.cpp) plus the Keystore-backed `keyring` module backend. */
object LuxLocal {
    init { System.loadLibrary("luxlocal") }

    /** Starts the app's Lux server on 127.0.0.1; returns its port, or -1 on failure. */
    external fun start(dataDir: String): Int
    external fun stop()
    /** Sets an environment variable for the app; call before start(). */
    external fun putenv(key: String, value: String)

    // ---- keyring backend: AES-GCM key in the Android Keystore, ciphertext in private SharedPreferences ----
    // Called from native threads (JNI) by name: keep the signatures in sync with src/android.cpp.
    // Return null on "not found / failure"; secretSet/secretDelete return "1" on success.
    lateinit var appContext: Context

    private fun prefs() = appContext.getSharedPreferences("luxlocal-secrets", Context.MODE_PRIVATE)
    private fun entry(service: String, key: String) = "$service\u0000$key"

    private fun aesKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey("luxlocal-secrets", null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(KeyGenParameterSpec.Builder("luxlocal-secrets", KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
            generateKey()
        }
    }

    @JvmStatic fun secretSet(service: String, key: String, secret: String): String? = try {
        val c = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, aesKey()) }
        val blob = c.iv + c.doFinal(secret.toByteArray())          // 12-byte IV || ciphertext+tag
        prefs().edit().putString(entry(service, key), Base64.encodeToString(blob, Base64.NO_WRAP)).commit()
        "1"
    } catch (e: Exception) { null }

    @JvmStatic fun secretGet(service: String, key: String): String? {
        val stored = prefs().getString(entry(service, key), null) ?: return null
        return try {
        val blob = Base64.decode(stored, Base64.NO_WRAP)
        val c = Cipher.getInstance("AES/GCM/NoPadding")
            .apply { init(Cipher.DECRYPT_MODE, aesKey(), GCMParameterSpec(128, blob, 0, 12)) }
        String(c.doFinal(blob, 12, blob.size - 12))
        } catch (e: Exception) { null }
    }

    @JvmStatic fun secretDelete(service: String, key: String): String? =
        if (prefs().edit().remove(entry(service, key)).commit()) "1" else null

    // ---- window.notify backend (src/android.cpp calls this from native threads) ----
    private var notifId = 100

    fun openAppIntent(c: Context): PendingIntent = PendingIntent.getActivity(c, 0,
        Intent(c, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)

    @JvmStatic fun notify(title: String, body: String) {
        val nm = appContext.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel("mail", appContext.getString(R.string.notification_channel), NotificationManager.IMPORTANCE_HIGH))
        // POST_NOTIFICATIONS denied => the system silently drops it; nothing to handle here.
        nm.notify(notifId++, Notification.Builder(appContext, "mail").setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(title).setContentText(body).setContentIntent(openAppIntent(appContext)).setAutoCancel(true).build())
    }

    // ---- window.* hooks (src/android.cpp calls these from worker threads; the pickers BLOCK until answered) ----
    const val PICK = 10
    @Volatile var activity: Activity? = null          // the visible MainActivity, set by it
    private var pending: CompletableFuture<Intent?>? = null

    private fun askActivity(intent: Intent): Intent? {
        val act = activity ?: return null
        val answer = CompletableFuture<Intent?>()
        pending = answer
        act.runOnUiThread { try { act.startActivityForResult(intent, PICK) } catch (e: Exception) { answer.complete(null) } }
        return try { answer.get(10, TimeUnit.MINUTES) } catch (e: Exception) { null }
    }

    /** MainActivity forwards its onActivityResult for requestCode PICK here. */
    fun onPickResult(resultCode: Int, data: Intent?) { pending?.complete(if (resultCode == Activity.RESULT_OK) data else null) }

    private fun safeName(n: String) = n.replace(Regex("[/\\\u0000]"), "_").trim().ifEmpty { "archivo" }
    private fun unique(f: File): File {
        if (!f.exists()) return f
        val dot = f.name.lastIndexOf('.').let { if (it <= 0) f.name.length else it }
        var i = 1
        while (true) { val c = File(f.parentFile, f.name.substring(0, dot) + " ($i)" + f.name.substring(dot)); if (!c.exists()) return c; i++ }
    }

    /** save: where a download goes (the public Downloads folder; needs "All files access"), no dialog.
     *  open: the system file picker; the chosen document is copied to the cache and its path returned ("" if cancelled). */
    @JvmStatic fun pickFile(suggested: String, save: Boolean): String {
        if (save) {
            val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS).also { it.mkdirs() }
            return unique(File(dir, safeName(suggested))).path
        }
        val uri = askActivity(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"))?.data ?: return ""
        return try {
            val name = appContext.contentResolver.query(uri, null, null, null, null)?.use { c ->
                if (c.moveToFirst()) c.getString(c.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME)) else null } ?: "archivo"
            val dir = File(appContext.cacheDir, "uploads").also { it.mkdirs() }
            dir.listFiles()?.filter { System.currentTimeMillis() - it.lastModified() > 24 * 3600_000L }?.forEach { it.delete() }
            val out = unique(File(dir, safeName(name)))
            appContext.contentResolver.openInputStream(uri)?.use { i -> out.outputStream().use { o -> i.copyTo(o) } }
            out.path
        } catch (e: Exception) { "" }
    }

    /** A folder of the shared storage, as a real path (works for the primary volume and for removable ones). */
    @JvmStatic fun pickFolder(unused: String): String {
        val uri: Uri = askActivity(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE))?.data ?: return ""
        val id = DocumentsContract.getTreeDocumentId(uri)            // "primary:Documents/Foo" or "1A2B-3C4D:Foo"
        val volume = id.substringBefore(':'); val rel = id.substringAfter(':', "")
        val root = if (volume == "primary") Environment.getExternalStorageDirectory().path else "/storage/$volume"
        return if (rel.isEmpty()) root else "$root/$rel"
    }

    @JvmStatic fun clipboardWrite(text: String): String {
        android.os.Handler(android.os.Looper.getMainLooper()).post {
            appContext.getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText(appContext.applicationInfo.loadLabel(appContext.packageManager), text))
        }
        return ""
    }
}
