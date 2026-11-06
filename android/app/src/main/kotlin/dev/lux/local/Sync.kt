package dev.lux.local

import android.app.AlarmManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.PowerManager
import android.util.Log
import java.net.HttpURLConnection
import java.net.URL

/**
 * Background sync with no foreground service, so no permanent notification: a chain of wake-up alarms.
 * Each alarm starts the Lux server if the process was gone, calls POST /api/sync, and schedules the next
 * one. New mail is announced by the Lux side through window.notify -> LuxLocal.notify.
 * IMAP has no push here (libcurl has no IDLE), so this is polling. The alarm is inexact (~15 min, Doze may stretch it); apps opt in with `background_sync`.
 * (JobScheduler was tried first: its delay timer does not wake the device.)
 */
object Sync {
    private const val EVERY_MS = 15 * 60_000L   // inexact: a 2-minute wake-up chain drains the battery; apps that need it opt in below

    private fun intent(c: Context, flags: Int): PendingIntent? =
        PendingIntent.getBroadcast(c, 0, Intent(c, SyncReceiver::class.java), flags or PendingIntent.FLAG_IMMUTABLE)

    // Opt-in per app: res/values/config.xml `background_sync` (default false, like needs_all_files).
    private fun enabled(c: Context) = c.resources.getBoolean(R.bool.background_sync)

    fun schedule(c: Context) = runCatching {
        if (!enabled(c)) return@runCatching
        val am = c.getSystemService(AlarmManager::class.java)
        val at = System.currentTimeMillis() + EVERY_MS
        val pi = intent(c, PendingIntent.FLAG_UPDATE_CURRENT)!!
        am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi)
    }.onFailure { Log.w("luxlocal", "cannot schedule sync: $it") }

    // On app start and after boot/update: only if no alarm is pending (a pending PendingIntent is our marker).
    fun ensureScheduled(c: Context) { if (enabled(c) && intent(c, PendingIntent.FLAG_NO_CREATE) == null) schedule(c) }

    // Runs on the calling thread: start the server (idempotent) and ask it to sync.
    fun syncNow(c: Context) {
        LuxLocal.appContext = c.applicationContext
        try {
            val port = LuxLocal.start(c.filesDir.path)
            if (port > 0) (URL("http://127.0.0.1:$port/api/sync?light=1").openConnection() as HttpURLConnection).run {
                requestMethod = "POST"; connectTimeout = 10_000; readTimeout = 45_000
                setRequestProperty("Connection", "close")   // a pooled keep-alive socket outlives Lux's idle timeout -> 408
                try {
                    val ok = responseCode < 400
                    Log.i("luxlocal", "sync $responseCode -> " + (if (ok) inputStream else errorStream)?.readBytes()?.decodeToString()?.take(200))
                } finally { disconnect() }
            }
        } catch (e: Exception) { Log.w("luxlocal", "sync failed: $e") }
    }
}

class SyncReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        Sync.schedule(c)   // next alarm first: a crash or a kill below must not end the chain
        val pending = goAsync()   // a receiver may run ~60 s in the background; the sync is bounded to ~55 s
        val wl = c.getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mail:sync")
        wl.acquire(65_000)
        Thread {
            try { Sync.syncNow(c) } finally { if (wl.isHeld) wl.release(); pending.finish() }
        }.start()
    }
}

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) = Sync.ensureScheduled(c)
}
