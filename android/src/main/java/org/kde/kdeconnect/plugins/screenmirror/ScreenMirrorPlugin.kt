/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.plugins.screenmirror

import android.app.Activity
import android.app.ActivityOptions
import android.app.Notification
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ApplicationInfo
import android.graphics.Bitmap
import android.graphics.Canvas
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.PowerManager
import android.os.SystemClock
import android.util.Base64
import android.util.Log
import android.view.Display
import androidx.core.content.ContextCompat
import io.github.muntashirakon.adb.AdbStream
import java.io.ByteArrayOutputStream
import java.io.FilterInputStream
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.kde.kdeconnect.NetworkPacket
import org.kde.kdeconnect.plugins.PluginFactory.LoadablePlugin
import org.kde.kdeconnect.plugins.notifications.NotificationReceiver
import org.kde.kdeconnect.plugins.notifications.NotificationsPlugin
import org.kde.kdeconnect.scrcpy.AppAudioCapture
import org.kde.kdeconnect.scrcpy.ControlSplitter
import org.kde.kdeconnect.scrcpy.ScrcpyControl
import org.kde.kdeconnect.scrcpy.ScrcpyPlugin
import org.kde.kdeconnect.scrcpy.ScrcpySession
import org.kde.kdeconnect.scrcpy.SelfAdb
import org.kde.kdeconnect.ui.PluginSettingsFragment
import org.kde.kdeconnect_tp.R
import kotlin.concurrent.thread

/**
 * Shows the phone's screen, or single apps on virtual displays, on the computer. The phone relays the scrcpy sockets as
 * packet payloads and decodes nothing.
 *
 *   computer -> phone
 *     mirror.apps.request {icons?: [package]}
 *     mirror.request  {sessionId, mode: "screen"|"app", package?, width?, height?, codec?, screenOff?, lockOnClose?, audio?,
 *                      notificationId? (Open on PC), handoff?, handoffTaskId?}
 *     mirror.control  {sessionId, data}  base64 scrcpy control messages
 *     mirror.stop     {sessionId}
 *     mirror.open     {sessionId, notificationId, package}  open a notification's target in an existing window
 *     mirror.handoff.return {sessionId}  the window's app goes back to the phone
 *   phone -> computer
 *     mirror.status   {sessionId, state: "needs_setup"|"stopped"|"error"|"screen_off"|"screen_on", message?}
 *     mirror.apps     {apps: [{label, package, system}]}
 *     mirror.icons    {icons: [{package, icon (base64 PNG)}]}
 *     mirror.video / mirror.audio  {sessionId} + endless payload
 */
@LoadablePlugin
class ScreenMirrorPlugin : ScrcpyPlugin() {
    override val displayName: String get() = context.getString(R.string.pref_plugin_screenmirror)
    override val description: String get() = context.getString(R.string.pref_plugin_screenmirror_desc)
    override val isCompatible: Boolean get() = super.isCompatible && SelfAdb.isSupported()
    override val supportedPacketTypes = arrayOf(APPS_REQUEST, REQUEST, CONTROL, STOP, OPEN, HANDOFF_RETURN)
    override val outgoingPacketTypes = arrayOf(STATUS, APPS, ICONS, VIDEO, AUDIO)
    override val hasSettings = true
    override val settingsScope = SettingsScope.PER_DEVICE
    override val idKey = "sessionId"
    override val statusType = STATUS

    override fun getSettingsFragment(activity: Activity): PluginSettingsFragment =
        PluginSettingsFragment.newInstance(pluginKey, device.deviceId, R.xml.screenmirrorplugin_preferences)

    fun openOnPcEnabled(): Boolean = preferences.getBoolean(PREF_OPEN_ON_PC, true) && SelfAdb.isSetUp(context)

    /** [handoff]: the task moved from the phone into this window, to bring it back. */
    private class Live(val session: ScrcpySession, val packageName: String?, handoff: Handoff.Task?, val app: Boolean) {
        /** Null once the task is gone (an app that closes itself when moved to another display). */
        @Volatile
        var handoff: Handoff.Task? = handoff

        var appAudio: AdbStream? = null

        @Volatile
        var lastWake = 0L

        val controlSplitter = ControlSplitter()

        fun close() {
            session.close()
            runCatching { appAudio?.close() }
        }
    }

    @Volatile
    private var lastForeground: Handoff.Task? = null

    private val sessions = ConcurrentHashMap<String, Live>()

    private val control = Executors.newSingleThreadExecutor()

    private val screenReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val off = intent.action == Intent.ACTION_SCREEN_OFF
            sessions.filterValues { !it.app }.keys.forEach { status(it, if (off) STATE_SCREEN_OFF else STATE_SCREEN_ON) }
            val apps = sessions.filterValues { it.app }.keys
            if (off && apps.isNotEmpty() && !OWN_DISPLAY_POWER) {
                // Before Android 13 an app window's display is off whenever the phone's is: wake the phone again, dark
                runCatching {
                    executor.execute {
                        wakePhone()
                        Thread.sleep(300)
                        apps.forEach { send(it, ScrcpyControl.displayPower(false)) }
                    }
                }
            }
        }
    }

    /*
     * Android 13+: locking the phone turns app windows' displays off, and turning one on turns the phone's screen on too, so
     * it is left off until a press in the window. Meanwhile the phone's screen must not time out.
     */
    @Suppress("DEPRECATION")
    private val awake: PowerManager.WakeLock by lazy {
        context.getSystemService(PowerManager::class.java).newWakeLock(PowerManager.SCREEN_DIM_WAKE_LOCK, "scrconnect:app windows")
            .apply { setReferenceCounted(false) }
    }

    private fun updateAwake() {
        if (!OWN_DISPLAY_POWER) return
        synchronized(awake) {
            if (sessions.values.any { it.app }) {
                if (!awake.isHeld) awake.acquire()
            } else if (awake.isHeld) {
                awake.release()
            }
        }
    }

    private fun displayOff(displayId: Int) =
        context.getSystemService(DisplayManager::class.java).getDisplay(displayId)?.state == Display.STATE_OFF

    /** Input for a dark app window is dropped; a press wakes it instead. */
    private fun control(id: String, data: ByteArray) {
        val live = sessions[id] ?: return
        if (!live.app || !OWN_DISPLAY_POWER) {
            send(id, data)
            return
        }
        val messages = live.controlSplitter.push(data)
        val display = live.session.displayId
        if (!live.controlSplitter.splitting || display == null || !displayOff(display)) {
            messages.forEach { send(id, it) }
            return
        }
        var pressed = false
        for (message in messages) {
            if (!ScrcpyControl.isInput(message)) {
                send(id, message)
            } else if (ScrcpyControl.isPress(message)) {
                pressed = true
            }
        }
        if (pressed) wake(id, live)
    }

    private fun wake(id: String, live: Live) {
        val now = SystemClock.uptimeMillis()
        // On a display that is already on, BACK_OR_SCREEN_ON presses BACK
        if (now - live.lastWake < WAKE_INTERVAL_MS) return
        live.lastWake = now
        Log.i(TAG, "Waking the display of app window $id")
        send(id, ScrcpyControl.backOrScreenOn(ScrcpyControl.ACTION_DOWN))
    }

    override fun onCreate() {
        val filter = IntentFilter(Intent.ACTION_SCREEN_OFF).apply { addAction(Intent.ACTION_SCREEN_ON) }
        ContextCompat.registerReceiver(context, screenReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    override fun onDestroy() {
        context.unregisterReceiver(screenReceiver)
        sessions.values.forEach { it.close() }
        sessions.clear()
        updateAwake()
        control.shutdownNow()
        super.onDestroy()
    }

    override fun onPacketReceived(np: NetworkPacket): Boolean {
        val id = np.getString("sessionId", "")
        when (np.type) {
            APPS_REQUEST -> executor.execute { sendApps(np.getStringList("icons")) }
            REQUEST -> executor.execute { start(id, np) }
            CONTROL -> runCatching { Base64.decode(np.getString("data", ""), Base64.DEFAULT) }.getOrNull()?.let { control(id, it) }
            STOP -> stop(id)
            OPEN -> executor.execute { sessions[id]?.let { openNotification(id, it, np.getString("notificationId", ""), np.getStringOrNull("package")) } }
            HANDOFF_RETURN -> executor.execute { handoffReturn(id) }
        }
        return true
    }

    private fun send(id: String, message: ByteArray) {
        val live = sessions[id] ?: return
        runCatching { control.execute { runCatching { live.session.send(message) } } }
    }

    /** Icons are slow to draw, so they are sent separately, only for the apps asked for. */
    private fun sendApps(iconsFor: List<String>?) {
        val pm = context.packageManager
        val apps = pm.queryIntentActivities(Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER), 0)
            .map { it.activityInfo.applicationInfo }
            .distinctBy { it.packageName }
            .filter { it.packageName != context.packageName }
        if (iconsFor == null) {
            val np = NetworkPacket(APPS)
            np["apps"] = buildJsonArray {
                apps.forEach {
                    add(buildJsonObject {
                        put("label", it.loadLabel(pm).toString())
                        put("package", it.packageName)
                        put("system", it.flags and ApplicationInfo.FLAG_SYSTEM != 0)
                    })
                }
            }
            device.sendPacket(np)
            return
        }
        for (batch in apps.filter { it.packageName in iconsFor }.chunked(20)) {
            val np = NetworkPacket(ICONS)
            np["icons"] = buildJsonArray { batch.forEach { add(buildJsonObject { put("package", it.packageName); put("icon", icon(it)) }) } }
            device.sendPacket(np)
        }
    }

    private fun icon(app: ApplicationInfo): String {
        val bitmap = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888)
        app.loadIcon(context.packageManager).apply { setBounds(0, 0, 64, 64) }.draw(Canvas(bitmap))
        val png = ByteArrayOutputStream()
        bitmap.compress(Bitmap.CompressFormat.PNG, 100, png)
        bitmap.recycle()
        return Base64.encodeToString(png.toByteArray(), Base64.NO_WRAP)
    }

    private fun start(id: String, np: NetworkPacket) {
        if (id.isEmpty()) return
        val step = SelfAdb.step(context)
        if (step != SelfAdb.Step.DONE) {
            status(id, STATE_NEEDS_SETUP, context.getString(step.text))
            return
        }
        val app = np.getString("mode", "screen") == "app"
        val pkg = np.getStringOrNull("package")?.takeIf(ScrcpySession::isValidPackageName)
        val notificationId = np.getStringOrNull("notificationId")?.takeIf { it.isNotEmpty() }
        val handoff = np.getBoolean("handoff", false)
        if (app && pkg == null && notificationId == null) {
            status(id, STATE_ERROR, "Invalid app")
            return
        }
        // Android 13+ app windows capture only their own app's sound; otherwise the phone's whole output
        val audio = np.getBoolean("audio", false)
        val ownAppAudio = audio && app && Build.VERSION.SDK_INT >= 33
        val interactive = context.getSystemService(PowerManager::class.java).isInteractive
        // Before Android 13 an app opened with the phone's screen off needs the phone awake: it stays dark
        val keepDark = app && !interactive && !OWN_DISPLAY_POWER
        val options = buildList {
            ScrcpySession.codecOption(np.getStringOrNull("codec"))?.let { add(it) }
            add("keep_active=true")
            if (np.getBoolean("lockOnClose", false) || keepDark) add("power_off_on_close=true")
            if (app && OWN_DISPLAY_POWER) add("power_on=false")
            if (audio && !ownAppAudio) add("audio_codec=raw")
            if (app) {
                val (w, h) = np.getInt("width", 0).coerceIn(0, 0xFFFF) to np.getInt("height", 0).coerceIn(0, 0xFFFF)
                add("new_display=" + if (w > 0 && h > 0) "${w}x$h" else "")
                add("vd_system_decorations=false")
                add("flex_display=true")
                add("vd_destroy_content=false")
            }
        }
        stop(id)
        val session = ScrcpySession(context, options, audio = audio && !ownAppAudio)
        try {
            session.start()
        } catch (e: Exception) {
            status(id, STATE_ERROR, e.message)
            return
        }
        val handoffTask = lastForeground?.takeIf { handoff && it.id == np.getInt("handoffTaskId", -1) }
        val live = Live(session, pkg, handoffTask, app)
        sessions[id] = live
        updateAwake()
        // The server turns the screen on as it starts: turn it off once the video flows
        val screenOff = keepDark || (np.getBoolean("screenOff", false) && interactive)
        val video = object : FilterInputStream(session.videoInput) {
            private var first = true

            override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, len).also {
                if (first && it > 0) {
                    first = false
                    if (screenOff) send(id, ScrcpyControl.displayPower(false))
                }
            }
        }
        relay(VIDEO, id, video) {
            // A new request may have replaced it under the same id
            if (sessions.remove(id, live)) {
                live.close()
                updateAwake()
                status(id, STATE_STOPPED)
            }
        }
        session.discardDeviceMessages()
        if (session.audio) relay(AUDIO, id, session.audioInput)
        if (ownAppAudio && pkg != null) {
            runCatching {
                val uid = context.packageManager.getApplicationInfo(pkg, 0).uid
                val capture = SelfAdb.open(context, "exec:" + AppAudioCapture.command(context, uid))
                live.appAudio = capture
                relay(AUDIO, id, capture.openInputStream())
            }
        }
        if (app && OWN_DISPLAY_POWER) executor.execute { session.awaitDisplayId()?.takeIf(::displayOff)?.let { wake(id, live) } }
        when {
            // Move the task with its state; where that is not possible, start the app there
            handoff -> if (handoffTask != null && Handoff.move(context, handoffTask, session.awaitDisplayId() ?: -1)) {
                checkHandoffSurvived(id, live, handoffTask)
            } else {
                pkg?.let { send(id, ScrcpyControl.startApp(it)) }
            }
            notificationId != null -> openNotification(id, live, notificationId, pkg)
            app -> send(id, ScrcpyControl.startApp(pkg!!))
        }
    }

    /** Some apps close themselves when moved to another display: start them afresh in the window then. */
    private fun checkHandoffSurvived(id: String, live: Live, task: Handoff.Task) = thread(isDaemon = true, name = "handoff-check") {
        Thread.sleep(HANDOFF_CHECK_MS)
        if (sessions[id] !== live) return@thread
        val exists = Handoff.exists(context, task.id)
        Log.i(TAG, "Hand-off of ${task.packageName} (task ${task.id}) to app window $id: task ${if (exists) "kept" else "gone, starting the app there"}")
        if (exists) return@thread
        live.handoff = null
        send(id, ScrcpyControl.startApp(task.packageName))
    }

    /** Fires the notification's own intent on the session's display, so that the exact chat or mail opens. */
    private fun openNotification(id: String, live: Live, notificationId: String, packageName: String?) {
        val display = live.session.awaitDisplayId() ?: return
        val pkg = packageName?.takeIf(ScrcpySession::isValidPackageName) ?: live.packageName
        // A running app would only come to the front on the phone's screen
        pkg?.let { Handoff.find(context, it) }?.let { Handoff.move(context, it, display) }
        val opened = CountDownLatch(1)
        var ok = false
        NotificationReceiver.RunCommand(context) { service ->
            runCatching {
                val sbn = service.activeNotifications.firstOrNull { NotificationsPlugin.notificationKey(it) == notificationId }
                val options = ActivityOptions.makeBasic().setLaunchDisplayId(display)
                // Allowed from the background: the setup granted SYSTEM_ALERT_WINDOW
                if (Build.VERSION.SDK_INT >= 36) {
                    options.setPendingIntentBackgroundActivityStartMode(ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS)
                } else if (Build.VERSION.SDK_INT >= 34) {
                    @Suppress("DEPRECATION")
                    options.setPendingIntentBackgroundActivityStartMode(ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED)
                }
                sbn?.notification?.contentIntent?.send(context, 0, null, null, null, null, options.toBundle())?.also {
                    if (sbn.notification.flags and Notification.FLAG_AUTO_CANCEL != 0) service.cancelNotification(sbn.key)
                    ok = true
                }
            }
            opened.countDown()
        }
        opened.await(3, TimeUnit.SECONDS)
        if (!ok && pkg != null) send(id, ScrcpyControl.startApp(pkg))
    }

    /** Phone Leap's pointer left the phone with the button held: passes the app in front to [offer] (package, task, label). */
    fun offerHandoff(offer: (String, Int, String) -> Unit) = runCatching {
        executor.execute {
            val task = Handoff.foreground(context) ?: return@execute
            lastForeground = task
            val label = runCatching {
                context.packageManager.run { getApplicationLabel(getApplicationInfo(task.packageName, 0)).toString() }
            }.getOrDefault(task.packageName)
            offer(task.packageName, task.id, label)
        }
    }

    private fun handoffReturn(id: String) {
        val live = sessions[id] ?: return
        val task = live.handoff ?: live.session.displayId?.let { Handoff.top(context, it) }
        val moved = task != null && Handoff.move(context, task, 0)
        stop(id)
        status(id, STATE_STOPPED)
        // The display hands its apps to the phone's screen as the server exits
        live.session.awaitExit()
        wakePhone()
        val pkg = task?.packageName ?: live.packageName
        if (!moved && pkg != null) Handoff.bringToFront(context, pkg, task?.component)
    }

    /** KEYCODE_WAKEUP, on the phone's own screen. */
    private fun wakePhone() {
        runCatching { SelfAdb.shell(context, "input keyevent 224") }
    }

    private fun stop(id: String) {
        sessions.remove(id)?.close()
        updateAwake()
    }

    companion object {
        private const val APPS_REQUEST = "kdeconnect.mirror.apps.request"
        private const val REQUEST = "kdeconnect.mirror.request"
        private const val CONTROL = "kdeconnect.mirror.control"
        private const val STOP = "kdeconnect.mirror.stop"
        private const val OPEN = "kdeconnect.mirror.open"
        private const val HANDOFF_RETURN = "kdeconnect.mirror.handoff.return"
        private const val HANDOFF_CHECK_MS = 2000L
        private const val PREF_OPEN_ON_PC = "mirror_open_on_pc"
        private const val STATUS = "kdeconnect.mirror.status"
        private const val APPS = "kdeconnect.mirror.apps"
        private const val ICONS = "kdeconnect.mirror.icons"
        private const val VIDEO = "kdeconnect.mirror.video"
        private const val AUDIO = "kdeconnect.mirror.audio"
        private const val STATE_SCREEN_OFF = "screen_off"
        private const val STATE_SCREEN_ON = "screen_on"

        /** scrcpy puts virtual displays in their own display group, with their own power state, from Android 13. */
        private val OWN_DISPLAY_POWER = Build.VERSION.SDK_INT >= 33

        private const val TAG = "KDE/ScreenMirror"

        private const val WAKE_INTERVAL_MS = 1500L
    }
}
