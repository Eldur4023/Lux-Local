package dev.lux.local

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.os.PowerManager
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Foreground service: owns the process while the app is closed. It starts the Lux server and calls
 * POST /api/sync every few minutes; the Lux side notifies new mail through window.notify -> LuxLocal.notify.
 * IMAP has no push here (libcurl has no IDLE), so this is polling, driven by alarms that survive Doze loosely.
 */
class SyncService : Service() {
    private val worker = Executors.newSingleThreadExecutor()   // start + syncs, one at a time
    private var port = -1

    override fun onBind(i: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        LuxLocal.appContext = applicationContext
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Sincronización", NotificationManager.IMPORTANCE_MIN))
        startForeground(1, Notification.Builder(this, CHANNEL).setSmallIcon(R.drawable.ic_notification)
            .setContentTitle("Lux Local").setContentText("Buscando correo nuevo")
            .setContentIntent(LuxLocal.openAppIntent(this)).setOngoing(true).build())
        worker.execute { port = LuxLocal.start(filesDir.path); sync() }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, id: Int): Int {
        if (intent?.action == ACTION_SYNC) worker.execute { sync() }
        return START_STICKY
    }

    private fun sync() {
        val wl = getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mail:sync")
        wl.acquire(3 * 60_000L)
        try {
            if (port < 0) port = LuxLocal.start(filesDir.path)
            if (port > 0) (URL("http://127.0.0.1:$port/api/sync").openConnection() as HttpURLConnection).run {
                requestMethod = "POST"; connectTimeout = 10_000; readTimeout = 150_000
                try { inputStream.readBytes() } finally { disconnect() }
            }
        } catch (e: Exception) { /* offline or server busy: the next alarm retries */ }
        finally { if (wl.isHeld) wl.release(); scheduleNext(this) }
    }

    override fun onDestroy() { worker.shutdownNow(); super.onDestroy() }

    companion object {
        const val CHANNEL = "sync"
        const val ACTION_SYNC = "dev.lux.mail.SYNC"
        private const val EVERY_MS = 3 * 60_000L   // same period as the desktop app

        // setAndAllowWhileIdle: needs no exact-alarm permission; in deep Doze it fires at most every ~9 min.
        fun scheduleNext(c: Context) {
            val pi = PendingIntent.getBroadcast(c, 0, Intent(c, SyncAlarmReceiver::class.java),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            c.getSystemService(AlarmManager::class.java)
                .setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, System.currentTimeMillis() + EVERY_MS, pi)
        }

        fun start(c: Context) {
            runCatching { c.startForegroundService(Intent(c, SyncService::class.java)) }
        }
    }
}

class SyncAlarmReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        // The service is already foreground, so a plain startService is allowed; if it died, bring it back.
        runCatching { c.startService(Intent(c, SyncService::class.java).setAction(SyncService.ACTION_SYNC)) }
            .onFailure { SyncService.start(c) }
    }
}

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) = SyncService.start(c)
}
