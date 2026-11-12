/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.content.Context
import java.io.IOException
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/**
 * One control-only scrcpy server shared by the features that need the shell user without a mirror window. It runs while
 * someone has acquired it and is restarted with a backoff when it ends. It never turns the screen on (power_on=false).
 */
object ShellBridge {
    private val users = HashSet<String>()
    private var worker: Thread? = null

    private val writer = Executors.newSingleThreadExecutor { r -> Thread(r, "shell-bridge-writer").apply { isDaemon = true } }

    @Volatile
    private var session: ScrcpySession? = null

    @Volatile
    var onClipboard: ((String) -> Unit)? = null

    @Volatile
    var generation = 0
        private set

    fun acquire(context: Context, user: String) = synchronized(users) {
        if (users.add(user) && worker == null) {
            val app = context.applicationContext
            worker = thread(isDaemon = true, name = "shell-bridge") { run(app) }
        }
    }

    fun release(user: String) = synchronized(users) {
        if (users.remove(user) && users.isEmpty()) {
            worker?.interrupt()
            worker = null
            session?.close()
        }
    }

    fun isServing(prefix: String): Boolean = session != null && synchronized(users) { users.any { it.startsWith(prefix) } }

    fun send(message: ByteArray): Boolean {
        val s = session ?: return false
        writer.execute { runCatching { s.send(message) } }
        return true
    }

    private fun run(context: Context) {
        var backoff = MIN_BACKOFF_MS
        try {
            while (!Thread.interrupted()) {
                val started = System.currentTimeMillis()
                if (SelfAdb.isSetUp(context)) {
                    val s = ScrcpySession(context, listOf("power_on=false"), video = false)
                    try {
                        s.start()
                        if (!publish(s)) return
                        readClipboardMessages(s.controlInput) { onClipboard?.invoke(it) }
                    } catch (_: IOException) {
                    } finally {
                        unpublish(s)
                        s.close()
                    }
                }
                if (!isCurrentWorker()) return
                backoff = if (System.currentTimeMillis() - started > 10_000) MIN_BACKOFF_MS else minOf(backoff * 2, MAX_BACKOFF_MS)
                Thread.sleep(backoff)
            }
        } catch (_: InterruptedException) {
        }
    }

    // The locked sections of run() live in these helpers: a return inside synchronized {} within run()'s try/finally
    // compiles to a monitor range that Android 11's verifier rejects (VerifyError)

    private fun publish(s: ScrcpySession): Boolean = synchronized(users) {
        val current = worker == Thread.currentThread()
        if (current) {
            session = s
            generation++
        }
        current
    }

    private fun unpublish(s: ScrcpySession) = synchronized(users) {
        if (session === s) session = null
    }

    private fun isCurrentWorker(): Boolean = synchronized(users) { worker == Thread.currentThread() }

    private const val MIN_BACKOFF_MS = 2_000L
    private const val MAX_BACKOFF_MS = 60_000L
}
