/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.plugins.webcam

import android.content.Context
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.os.Build
import android.util.Log
import androidx.core.content.edit
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.IOException
import java.io.InputStream
import java.io.SequenceInputStream
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.kde.kdeconnect.NetworkPacket
import org.kde.kdeconnect.plugins.PluginFactory.LoadablePlugin
import org.kde.kdeconnect.scrcpy.ScrcpyPlugin
import org.kde.kdeconnect.scrcpy.ScrcpySession
import org.kde.kdeconnect.scrcpy.SelfAdb
import org.kde.kdeconnect_tp.R

/**
 * The phone's cameras as webcams on the computer (scrcpy's camera capture, Android 12+). A camera runs only while an app
 * on the computer uses it.
 *
 *   computer -> phone: webcam.cameras.request, webcam.start {streamId, cameraId, width, height, codec?}, webcam.stop {streamId}
 *   phone -> computer: webcam.cameras {cameras: [{id, facing, sizes}]}, webcam.video {streamId} + payload,
 *                      webcam.status {streamId, state: "needs_setup"|"stopped"|"busy"|"error", message?}
 */
@LoadablePlugin
class WebcamPlugin : ScrcpyPlugin() {
    override val displayName: String get() = context.getString(R.string.pref_plugin_webcam)
    override val description: String get() = context.getString(R.string.pref_plugin_webcam_desc)
    override val isCompatible: Boolean
        get() = super.isCompatible && Build.VERSION.SDK_INT >= 31 && context.packageManager.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)
    override val isEnabledByDefault = false
    override val supportedPacketTypes = arrayOf(CAMERAS_REQUEST, START, STOP)
    override val outgoingPacketTypes = arrayOf(CAMERAS, VIDEO, STATUS)
    override val idKey = "streamId"
    override val statusType = STATUS

    private class Stream(val session: ScrcpySession, val camera: String)

    private val cameras get() = context.getSystemService(CameraManager::class.java)
    private val streams = ConcurrentHashMap<String, Stream>()

    private val stopped = ConcurrentHashMap.newKeySet<String>()

    // One start at a time: apps often open both cameras at once, and two starts would take each other's camera
    private val starter = Executors.newSingleThreadExecutor()

    // The camera and size option that worked last time: a failing camera is slow to fail, and on some phones drops all cameras
    private val prefs get() = context.getSharedPreferences("webcam", Context.MODE_PRIVATE)

    private fun working(camera: String): Pair<String, Boolean>? =
        prefs.getString("working_$camera", null)?.split(',')?.takeIf { it.size == 2 }?.let { it[0] to (it[1] == "size") }

    private fun setWorking(camera: String, value: Pair<String, Boolean>?) = prefs.edit {
        if (value == null) remove("working_$camera") else putString("working_$camera", value.first + if (value.second) ",size" else ",default")
    }

    override fun onDestroy() {
        streams.values.forEach { it.session.close() }
        streams.clear()
        starter.shutdownNow()
        super.onDestroy()
    }

    override fun onPacketReceived(np: NetworkPacket): Boolean {
        when (np.type) {
            CAMERAS_REQUEST -> executor.execute { sendCameras() }
            START -> starter.execute {
                try {
                    start(np)
                } catch (e: Exception) {
                    // A disconnect interrupts a start: an exception leaving this thread would end the whole app
                    if (!starter.isShutdown) Log.e(TAG, "Camera start failed", e)
                } finally {
                    stopped.remove(np.getString("streamId", ""))
                }
            }
            STOP -> np.getString("streamId", "").let { id -> streams.remove(id)?.session?.close() ?: stopped.add(id) }
        }
        return true
    }

    private fun facing(id: String) = when (cameras.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING)) {
        CameraCharacteristics.LENS_FACING_FRONT -> "front"
        CameraCharacteristics.LENS_FACING_BACK -> "back"
        else -> "external"
    }

    private fun sendCameras() {
        val np = NetworkPacket(CAMERAS)
        np["cameras"] = buildJsonArray {
            for (id in cameras.cameraIdList) {
                val sizes = cameras.getCameraCharacteristics(id).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    ?.getOutputSizes(SurfaceTexture::class.java).orEmpty().filter { it.width * it.height >= 320 * 240 }
                add(buildJsonObject {
                    put("id", id)
                    put("facing", facing(id))
                    put("sizes", JsonArray(sizes.map { JsonArray(listOf(JsonPrimitive(it.width), JsonPrimitive(it.height))) }))
                })
            }
        }
        device.sendPacket(np)
    }

    private fun start(np: NetworkPacket) {
        val id = np.getString("streamId", "")
        val camera = np.getString("cameraId", "")
        if (id.isEmpty()) return
        if (camera !in cameras.cameraIdList) return status(id, STATE_ERROR, "camera $camera: not on this phone")
        if (!SelfAdb.isSetUp(context)) return status(id, STATE_NEEDS_SETUP)
        val (width, height) = np.getInt("width", 0).coerceIn(0, 8192) to np.getInt("height", 0).coerceIn(0, 8192)
        val size = "camera_size=${width}x$height".takeIf { width > 0 && height > 0 }
        // Some phones refuse a camera's announced size or drop a camera opened by the shell (Galaxy S22): then the default size is
        // tried, then the other cameras facing the same way
        val facing = facing(camera)
        val candidates = (listOfNotNull(working(camera), (camera to true).takeIf { size != null }, camera to false) +
            cameras.cameraIdList.filter { it != camera && facing(it) == facing }.map { it to false }).distinct()
        streams.entries.filter { it.value.camera == camera }.forEach { (other, s) ->
            if (streams.remove(other, s)) {
                s.session.close()
                s.session.awaitExit()
                status(other, STATE_STOPPED)
            }
        }
        // A camera opened from the shell disconnects the running one, even where Android lists them as concurrent. A running
        // camera is not given up for another one: two apps would take it from each other for ever. The computer asks again.
        if (streams.isNotEmpty()) return status(id, STATE_BUSY)
        if (streams.isEmpty()) runCatching { SelfAdb.shell(context, FREE_CAMERAS) }
        var error: String? = null
        val dropped = HashSet<String>()
        var retries = 0
        val queue = ArrayDeque(candidates)
        while (queue.isNotEmpty()) {
            val (candidate, withSize) = queue.removeFirst()
            if (candidate in dropped) continue
            val options = buildList {
                addAll(listOf("video_source=camera", "camera_id=$candidate", "video_bit_rate=4000000"))
                ScrcpySession.codecOption(np.getStringOrNull("codec"))?.let { add(it) }
                if (withSize && size != null) addAll(listOf(size, "camera_fps=30")) else add("max_size=1280")
            }
            val session = ScrcpySession(context, options, control = false)
            // With a time limit: the ADB calls have none, and a hung server would hold up every later start
            val attempt = executor.submit<ByteArray?> {
                session.start()
                readUntilPictures(session.videoInput)
            }
            val begin = try {
                attempt.get(START_TIMEOUT_S, TimeUnit.SECONDS)
            } catch (e: ExecutionException) {
                val cause = e.cause as? IOException ?: run { session.close(); throw e }
                error = error ?: "camera $candidate: ${cause.message}"
                null
            } catch (_: TimeoutException) {
                null
            } catch (e: Exception) {
                attempt.cancel(true)
                session.close()
                throw e
            }
            attempt.cancel(true)
            if (id in stopped) return session.close()
            if (begin != null) {
                setWorking(camera, candidate to withSize)
                val stream = Stream(session, camera)
                streams.put(id, stream)?.session?.close()
                relay(VIDEO, id, SequenceInputStream(ByteArrayInputStream(begin), session.videoInput)) {
                    if (streams.remove(id, stream)) {
                        session.close()
                        status(id, STATE_STOPPED)
                    }
                }
                return
            }
            session.close()
            session.awaitExit()
            val log = session.log
            Log.w(TAG, "Camera $candidate did not stream:\n$log")
            if (working(camera) == candidate to withSize) setWorking(camera, null)
            val said = log.lines().filter { it.startsWith("[server] ERROR") || it.startsWith("[server] WARN") }.take(2)
            error = error ?: ("camera $candidate: " + said.joinToString(" | ") { it.removePrefix("[server] ") }.ifEmpty { "no picture" })
            if ("Camera disconnected" in log) dropped += candidate
            // After such a drop the S22 takes all its cameras away for about 3 s
            val retry = "not found" in log && retries++ < 4
            if (retry) queue.addFirst(candidate to withSize)
            Thread.sleep(if (retry || candidate in dropped) 1500 else 300)
        }
        status(id, STATE_ERROR, error)
    }

    companion object {
        private const val CAMERAS_REQUEST = "kdeconnect.webcam.cameras.request"
        private const val START = "kdeconnect.webcam.start"
        private const val STOP = "kdeconnect.webcam.stop"
        private const val CAMERAS = "kdeconnect.webcam.cameras"
        private const val VIDEO = "kdeconnect.webcam.video"
        private const val STATUS = "kdeconnect.webcam.status"
        private const val STATE_BUSY = "busy"

        private const val START_TIMEOUT_S = 12L
        private const val TAG = "WebcamPlugin"

        private const val FREE_CAMERAS = "pkill -f 'video_source=[c]amera'; " +
            "for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -f 'video_source=[c]amera' >/dev/null || break; sleep 0.2; done"

        /**
         * Reads a scrcpy video stream until it has carried pictures for [forMs]. Returns what was read, or null if the stream ended
         * or the server could not capture.
         */
        internal fun readUntilPictures(stream: InputStream, forMs: Long = 1000, clock: () -> Long = System::currentTimeMillis): ByteArray? {
            val read = ByteArrayOutputStream()
            val input = DataInputStream(stream)
            fun next(n: Int): ByteBuffer = ByteBuffer.wrap(ByteArray(n).also { input.readFully(it); read.write(it) })
            try {
                next(64)
                if (next(4).int in 0..1) return null // no codec: the server could not capture
                var firstPicture: Long? = null
                while (firstPicture == null || clock() - firstPicture < forMs) {
                    val header = next(12)
                    val flags = header.long
                    if (flags < 0) continue // a capture session starts: the 12 bytes hold its size
                    val length = header.int
                    if (length !in 0..(32 shl 20)) return null
                    next(length)
                    if ((flags and (1L shl 62)) == 0L && firstPicture == null) firstPicture = clock() // not a config packet
                }
            } catch (_: IOException) {
                return null
            }
            return read.toByteArray()
        }
    }
}
