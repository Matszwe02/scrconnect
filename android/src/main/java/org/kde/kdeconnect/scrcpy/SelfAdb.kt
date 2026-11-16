/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Build
import android.util.Log
import android.provider.Settings
import androidx.annotation.StringRes
import io.github.muntashirakon.adb.AbsAdbConnectionManager
import io.github.muntashirakon.adb.AdbStream
import io.github.muntashirakon.adb.android.AdbMdns
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.math.BigInteger
import java.net.InetSocketAddress
import java.net.Socket
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.PrivateKey
import java.security.cert.Certificate
import java.security.cert.CertificateFactory
import java.security.spec.PKCS8EncodedKeySpec
import java.util.Date
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread
import org.bouncycastle.asn1.x500.X500Name
import org.bouncycastle.cert.jcajce.JcaX509v3CertificateBuilder
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder
import org.kde.kdeconnect_tp.R

object SelfAdb {
    enum class Step(@StringRes val text: Int) {
        UNSUPPORTED(R.string.mirror_step_unsupported),
        CONNECT_WIFI(R.string.mirror_step_wifi),
        ENABLE_DEVELOPER_OPTIONS(R.string.mirror_step_developer),
        ENABLE_WIRELESS_DEBUGGING(R.string.mirror_step_wireless),
        PAIR(R.string.mirror_step_pair),
        DONE(R.string.mirror_step_done),
    }

    private const val TAG = "SelfAdb"
    private const val ADB_WIFI_ENABLED = "adb_wifi_enabled"
    private const val CALL_TIMEOUT_MS = 3_000L
    private const val OPEN_TIMEOUT_MS = 8_000L
    private const val SHELL_TIMEOUT_MS = 8_000L

    // Unlike wireless debugging, it stays open off Wi-Fi until reboot
    private const val TCP_PORT = 5555

    private class Manager(context: Context) : AbsAdbConnectionManager() {
        private val identity: Pair<PrivateKey, Certificate>

        init {
            setApi(Build.VERSION.SDK_INT)
            setHostAddress("127.0.0.1")
            setTimeout(10, TimeUnit.SECONDS)
            setThrowOnUnauthorised(true)
            val keyFile = File(context.filesDir, "mirror/adb_key.pk8")
            val certFile = File(context.filesDir, "mirror/adb_cert.der")
            fun certificate(der: ByteArray) = CertificateFactory.getInstance("X.509").generateCertificate(der.inputStream())
            identity = runCatching {
                KeyFactory.getInstance("RSA").generatePrivate(PKCS8EncodedKeySpec(keyFile.readBytes())) to certificate(certFile.readBytes())
            }.getOrElse {
                val pair = KeyPairGenerator.getInstance("RSA").apply { initialize(2048) }.generateKeyPair()
                val name = X500Name("CN=KDE Connect")
                val now = System.currentTimeMillis()
                val certBytes = JcaX509v3CertificateBuilder(name, BigInteger.valueOf(now), Date(now - 86_400_000L), Date(now + 30 * 365 * 86_400_000L), name, pair.public)
                    .build(JcaContentSignerBuilder("SHA256withRSA").build(pair.private)).encoded
                keyFile.parentFile?.mkdirs()
                keyFile.writeBytes(pair.private.encoded)
                certFile.writeBytes(certBytes)
                pair.private to certificate(certBytes)
            }
        }

        override fun getPrivateKey() = identity.first
        override fun getCertificate() = identity.second
        override fun getDeviceName() = "SCR Connect"
    }

    @Volatile
    private var manager: Manager? = null

    private fun manager(context: Context): Manager =
        manager ?: synchronized(this) { manager ?: Manager(context.applicationContext).also { manager = it } }

    fun isSupported(): Boolean = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R

    fun isSetUp(context: Context): Boolean =
        context.checkSelfPermission(Manifest.permission.WRITE_SECURE_SETTINGS) == PackageManager.PERMISSION_GRANTED

    private fun isWifiConnected(context: Context): Boolean {
        val cm = context.getSystemService(ConnectivityManager::class.java)
        @Suppress("DEPRECATION")
        return cm.allNetworks.any { network ->
            cm.getNetworkCapabilities(network)?.let {
                it.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) || it.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
            } == true
        }
    }

    private fun globalSetting(context: Context, key: String) = Settings.Global.getInt(context.contentResolver, key, 0) == 1

    private fun enableDebugging(context: Context): Boolean = isSetUp(context) && try {
        for (key in arrayOf(Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, Settings.Global.ADB_ENABLED, ADB_WIFI_ENABLED)) {
            Settings.Global.putInt(context.contentResolver, key, 1)
        }
        true
    } catch (_: SecurityException) {
        false
    }

    fun connect(context: Context): Boolean {
        val m = manager(context)
        if (isConnected(m) == null) reset(m) // wedged: see open()
        return connectFresh(context)
    }

    @Synchronized
    private fun connectFresh(context: Context): Boolean {
        val m = manager(context)
        if (isConnected(m) == true) return true
        if (!isSupported()) return false
        if (connectTcpPort(m)) return true
        if (!isWifiConnected(context)) return false
        if (!connectTls(context, m)) return false
        if (!tcpPortOpen()) switchToTcpPort(context, m)
        return m.isConnected
    }

    /** Null when the library does not answer: it asks under the same lock that a hung stream open holds. */
    private fun isConnected(m: Manager): Boolean? = bounded(CALL_TIMEOUT_MS) { m.isConnected }

    /**
     * Runs [block] on its own thread and waits [timeoutMs] for it. The library waits for the phone's reply to a stream open
     * without a limit, and a reply that comes too early is missed for good: that open would then hold the library's lock
     * forever, and with it every later ADB call. Null on timeout; a result that arrives late goes to [onLate].
     */
    private fun <T : Any> bounded(timeoutMs: Long, onLate: (T) -> Unit = {}, block: () -> T): T? {
        val result = ArrayBlockingQueue<Result<T>>(1)
        val abandoned = AtomicBoolean()
        thread(isDaemon = true, name = "adb-call") {
            val r = runCatching(block)
            synchronized(abandoned) {
                if (abandoned.get()) r.getOrNull()?.let(onLate) else result.offer(r)
            }
        }
        result.poll(timeoutMs, TimeUnit.MILLISECONDS)?.let { return it.getOrThrow() }
        synchronized(abandoned) {
            abandoned.set(true)
            result.poll()?.let { return it.getOrThrow() }
        }
        return null
    }

    /** Drops a connection that does not answer: the next call makes a new one. Streams on the old one end. */
    private fun reset(stuck: Manager) {
        Log.w(TAG, "ADB does not answer, reconnecting")
        synchronized(this) { if (manager === stuck) manager = null }
        thread(isDaemon = true, name = "adb-reset") { runCatching { stuck.disconnect() } } // closing it also frees the hung open
    }

    private fun connectTls(context: Context, m: Manager): Boolean {
        repeat(if (enableDebugging(context)) 3 else 1) { attempt ->
            if (attempt > 0) Thread.sleep(1000)
            if (runCatching { m.connectTls(context, 5000) }.getOrDefault(false)) return true
        }
        return false
    }

    private fun connectTcpPort(m: Manager): Boolean = tcpPortOpen() && runCatching { m.connect("127.0.0.1", TCP_PORT) }.getOrDefault(false)

    private fun tcpPortOpen(): Boolean =
        runCatching { Socket().use { it.connect(InetSocketAddress("127.0.0.1", TCP_PORT), 500) } }.isSuccess

    private fun switchToTcpPort(context: Context, m: Manager) {
        runCatching { m.openStream("tcpip:$TCP_PORT").use { readAll(it.openInputStream()) } }
        runCatching { m.disconnect() }
        repeat(20) {
            Thread.sleep(500)
            if (connectTcpPort(m)) return
        }
        connectTls(context, m)
    }

    @Throws(IOException::class)
    fun open(context: Context, service: String): AdbStream {
        if (!connect(context)) throw IOException("Wireless debugging is not reachable")
        val m = manager(context)
        return try {
            bounded(OPEN_TIMEOUT_MS, onLate = { runCatching { it.close() } }) { m.openStream(service) }
                ?: run {
                    reset(m)
                    throw IOException("ADB did not answer opening $service")
                }
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IOException(e)
        }
    }

    @Throws(IOException::class)
    fun shell(context: Context, command: String): String =
        open(context, "shell:$command").use { stream ->
            within(stream, SHELL_TIMEOUT_MS) { readAll(stream.openInputStream()).toString(Charsets.UTF_8) }
        }

    /**
     * Runs [block], which reads or writes [stream], for at most [timeoutMs]. The library can lose the phone's reply or the
     * end of a stream, and a read then waits for ever: the stream is closed and an IOException thrown instead.
     */
    @Throws(IOException::class)
    fun <T : Any> within(stream: AdbStream, timeoutMs: Long, block: () -> T): T =
        bounded(timeoutMs, block = block) ?: run {
            runCatching { stream.close() }
            throw IOException("ADB stream did not answer in ${timeoutMs / 1000} s")
        }

    fun step(context: Context): Step {
        if (!isSupported()) return Step.UNSUPPORTED
        if (connect(context)) {
            if (!isSetUp(context) || !Settings.canDrawOverlays(context)) grantSelf(context)
            return Step.DONE
        }
        if (!isWifiConnected(context)) return Step.CONNECT_WIFI
        if (!isSetUp(context)) {
            if (!globalSetting(context, Settings.Global.DEVELOPMENT_SETTINGS_ENABLED)) return Step.ENABLE_DEVELOPER_OPTIONS
            if (!globalSetting(context, ADB_WIFI_ENABLED)) return Step.ENABLE_WIRELESS_DEBUGGING
        }
        return Step.PAIR
    }

    fun pair(context: Context, code: String): String? {
        if (!Regex("\\d{6}").matches(code)) return context.getString(R.string.mirror_pairing_bad_code)
        val found = ArrayBlockingQueue<Pair<String, Int>>(1)
        val mdns = AdbMdns(context, AdbMdns.SERVICE_TYPE_TLS_PAIRING) { address, port ->
            found.offer((address?.hostAddress ?: "127.0.0.1") to port)
        }
        try {
            mdns.start()
            val (host, port) = found.poll(15, TimeUnit.SECONDS) ?: return context.getString(R.string.mirror_pairing_no_dialog)
            if (!runCatching { manager(context).pair(host, port, code) }.getOrDefault(false)) {
                return context.getString(R.string.mirror_pairing_failed)
            }
        } finally {
            mdns.stop()
        }
        repeat(10) {
            if (connect(context)) {
                grantSelf(context)
                return null
            }
            Thread.sleep(500)
        }
        return context.getString(R.string.mirror_pairing_failed)
    }

    private fun grantSelf(context: Context) {
        val p = context.packageName
        runCatching { shell(context, "pm grant $p ${Manifest.permission.WRITE_SECURE_SETTINGS}; appops set $p SYSTEM_ALERT_WINDOW allow") }
    }

    // libadb ends a finished stream with IOException("Stream closed") instead of -1
    fun readAll(input: InputStream): ByteArray {
        val out = ByteArrayOutputStream()
        try {
            input.copyTo(out)
        } catch (e: IOException) {
            if (e.message?.startsWith("Stream closed") != true) throw e
        }
        return out.toByteArray()
    }
}
