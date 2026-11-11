/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.content.Context
import io.github.muntashirakon.adb.AdbStream
import java.io.Closeable
import java.io.DataInputStream
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * A scrcpy server (Apache-2.0, Genymobile; bundled as an asset) running as the shell user on this phone. Only pass options
 * that differ from the server's defaults: some phones abort when the command line is longer than about 175 characters.
 */
class ScrcpySession(
    private val context: Context,
    options: List<String>,
    val video: Boolean = true,
    val audio: Boolean = false,
    val control: Boolean = true,
) : Closeable {
    private val scid = "%08x".format(SecureRandom().nextInt(0x7FFFFFFF))

    // The server deletes its jar shortly after starting: one path per session
    private val remotePath = "/data/local/tmp/kdeconnect-scrcpy-$scid.jar"
    private val args = buildList {
        add("scid=$scid")
        add("tunnel_forward=true")
        // Without video, the device name would end up in the next socket
        if (!video) addAll(listOf("video=false", "send_device_meta=false"))
        if (!audio) add("audio=false")
        if (!control) add("control=false")
        addAll(options)
    }
    private val streams = CopyOnWriteArrayList<AdbStream>()
    @Volatile
    private var controlStream: AdbStream? = null
    @Volatile
    private var serverStream: AdbStream? = null
    private val displayCreated = CountDownLatch(1)
    private val exited = CountDownLatch(1)
    private val output = StringBuilder()

    @Volatile
    private var closed = false

    lateinit var videoInput: InputStream
        private set
    lateinit var audioInput: InputStream
        private set
    lateinit var controlInput: InputStream
        private set

    @Volatile
    var displayId: Int? = null
        private set

    val log: String get() = synchronized(output) { output.toString() }

    @Throws(IOException::class)
    fun start() {
        try {
            pushServer(context, remotePath)
            val server = track(SelfAdb.open(context, "shell:" + serverCommand(remotePath, args)))
            serverStream = server
            thread(isDaemon = true, name = "scrcpy-server") { readOutput(server.openInputStream()) }
            // The server accepts the enabled sockets in this order; the first one starts with a dummy byte
            val sockets = listOf(video, audio, control).count { it }
            val opened = List(sockets) { i -> openSocket(expectDummyByte = i == 0) }
            var i = 0
            if (video) videoInput = opened[i++].openInputStream()
            if (audio) audioInput = opened[i++].openInputStream()
            if (control) {
                controlStream = opened[i]
                controlInput = opened[i].openInputStream()
            }
        } catch (e: Exception) {
            close()
            throw e as? IOException ?: IOException(e)
        }
    }

    private fun track(stream: AdbStream): AdbStream {
        streams += stream
        if (closed) {
            stream.close()
            throw IOException("Session closed")
        }
        return stream
    }

    private fun openSocket(expectDummyByte: Boolean): AdbStream {
        repeat(50) {
            if (closed) throw IOException("Session closed")
            val stream = runCatching { SelfAdb.open(context, "localabstract:scrcpy_$scid") }.getOrNull()
            if (stream != null && (!expectDummyByte || runCatching { stream.openInputStream().read() >= 0 }.getOrDefault(false))) {
                return track(stream)
            }
            stream?.close()
            Thread.sleep(100)
        }
        throw IOException("The scrcpy server did not start: $log")
    }

    private fun readOutput(input: InputStream) {
        val lines = input.bufferedReader()
        try {
            while (true) {
                val line = lines.readLine() ?: break
                DISPLAY_ID.find(line)?.let {
                    displayId = it.groupValues[1].toInt()
                    displayCreated.countDown()
                }
                synchronized(output) {
                    output.append(line).append('\n')
                    if (output.length > 4096) output.delete(0, output.length - 4096)
                }
            }
        } catch (_: IOException) {
        }
        exited.countDown()
        displayCreated.countDown()
    }

    fun awaitDisplayId(timeoutMs: Long = 3000): Int? {
        displayCreated.await(timeoutMs, TimeUnit.MILLISECONDS)
        return displayId
    }

    fun awaitExit(timeoutMs: Long = 3000): Boolean = exited.await(timeoutMs, TimeUnit.MILLISECONDS)

    fun discardDeviceMessages() {
        thread(isDaemon = true) {
            val buffer = ByteArray(4096)
            runCatching { while (controlInput.read(buffer) >= 0) Unit }
        }
    }

    @Throws(IOException::class)
    fun send(message: ByteArray) {
        val stream = controlStream?.takeIf { !closed } ?: throw IOException("No control socket")
        synchronized(stream) {
            stream.write(message, 0, message.size)
            stream.flush()
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        streams.filter { it !== serverStream }.forEach { runCatching { it.close() } }
        thread(isDaemon = true) {
            if (!awaitExit(1000)) {
                // The brackets keep pkill from matching the shell that runs it
                runCatching { SelfAdb.shell(context, "pkill -f 'scid=${scid.dropLast(1)}[${scid.last()}]'") }
                awaitExit(2000)
            }
            runCatching { serverStream?.close() }
        }
    }

    companion object {
        private const val VERSION = "5.0.1"
        private const val ASSET = "scrcpy-server-v$VERSION.jar"
        private val DISPLAY_ID = Regex("New display: .* \\(id=(\\d+)\\)")
        private val PACKAGE_NAME = Regex("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+")

        fun codecOption(codec: String?): String? = codec?.takeIf { it == "h265" || it == "av1" }?.let { "video_codec=$it" }

        fun isValidPackageName(name: String?): Boolean = name != null && PACKAGE_NAME.matches(name)

        private fun serverCommand(path: String, args: List<String>) =
            "CLASSPATH=$path app_process / com.genymobile.scrcpy.Server $VERSION ${args.joinToString(" ")} 2>&1"

        private fun pushServer(context: Context, path: String) {
            val jar = context.assets.open(ASSET).use { it.readBytes() }
            SelfAdb.open(context, "sync:").use { stream ->
                fun header(id: String, value: Int) =
                    ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).put(id.toByteArray()).putInt(value).array()
                val out = stream.openOutputStream()
                val target = "$path,420".toByteArray()
                out.write(header("SEND", target.size) + target)
                for (offset in jar.indices step 65536) {
                    val n = minOf(65536, jar.size - offset)
                    out.write(header("DATA", n))
                    out.write(jar, offset, n)
                }
                out.write(header("DONE", 0))
                out.flush()
                val reply = ByteArray(8)
                DataInputStream(stream.openInputStream()).readFully(reply)
                if (String(reply, 0, 4) != "OKAY") throw IOException("Could not copy the scrcpy server to the phone")
            }
        }
    }
}
