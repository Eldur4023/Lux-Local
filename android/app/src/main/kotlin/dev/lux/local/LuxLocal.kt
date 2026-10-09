package dev.lux.local

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
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
}
