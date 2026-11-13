package org.ardour.ardour

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.IBinder
import android.os.Process
import android.util.Log
import androidx.core.app.NotificationCompat

/**
 * Keeps the engine running while the application is not on screen.
 *
 * A DAW on a phone is expected to keep playing and keep recording with the
 * screen off, and since Android 8 a process without a foreground service is
 * free to be killed at any moment - which for this application means the
 * AAudio stream disappearing mid-take.  So the engine runs under a
 * "mediaPlayback" foreground service with an ongoing notification.
 *
 * The service also owns the audio focus: when something else takes the output
 * (a call, another player) the transport is stopped rather than left to fight
 * for the device, and unplugging headphones stops it too, which is what every
 * other audio application does.
 */
class ArdourService : Service() {

    private var focusRequest: AudioFocusRequest? = null
    private var noisy: BroadcastReceiver? = null
    private var removed = false

    private val engine: EngineHost
        get() = (application as ArdourApplication).engine

    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> stopTransport("audio focus")
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()

        val notification = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.notification_engine))
            .setOngoing(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(
                PendingIntent.getActivity(
                    this, 0, Intent(this, MainActivity::class.java),
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                )
            )
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        Log.i(TAG, "ARDOUR_SERVICE foreground")

        val audio = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        focusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setOnAudioFocusChangeListener(focusListener)
            .build()
            .also { audio.requestAudioFocus(it) }

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (intent?.action == AudioManager.ACTION_AUDIO_BECOMING_NOISY) {
                    stopTransport("headphones unplugged")
                }
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(receiver, IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY), Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(receiver, IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY))
        }
        noisy = receiver
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        /* the engine is owned by the application, so starting it here only
         * makes sure it is up even if the activity is recreated */
        engine.start()
        return START_STICKY
    }

    /** The user swiped the application away: save, and let go of everything. */
    override fun onTaskRemoved(rootIntent: Intent?) {
        removed = true
        Thread {
            try {
                engine.call("session.save", "{}")
            } catch (t: Throwable) {
                Log.w(TAG, "could not save on the way out: ${t.message}")
            }
            stopSelf()
        }.start()
    }

    override fun onDestroy() {
        noisy?.let { unregisterReceiver(it) }
        noisy = null
        focusRequest?.let { (getSystemService(Context.AUDIO_SERVICE) as AudioManager).abandonAudioFocusRequest(it) }
        focusRequest = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        Log.i(TAG, "ARDOUR_SERVICE gone removed=$removed")
        super.onDestroy()
        if (removed) {
            /* libardour has no orderly teardown in this embedding yet, and the
             * process must not sit there holding the audio device; a native
             * shutdown is the follow-up, this releases everything now */
            Process.killProcess(Process.myPid())
        }
    }

    private fun stopTransport(why: String) {
        Log.i(TAG, "ARDOUR_SERVICE stopping the transport: $why")
        Thread {
            try {
                engine.call("transport.stop", "{}")
            } catch (t: Throwable) {
                Log.w(TAG, "could not stop the transport: ${t.message}")
            }
        }.start()
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (manager.getNotificationChannel(CHANNEL) != null) return
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.notification_channel), NotificationManager.IMPORTANCE_LOW)
                .apply { setShowBadge(false) }
        )
    }

    companion object {
        const val TAG = "ArdourService"
        private const val CHANNEL = "engine"
        private const val NOTIFICATION_ID = 1
    }
}
