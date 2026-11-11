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

    private const val ADB_WIFI_ENABLED = "adb_wifi_enabled"

    // Unlike wireless debugging, it stays open off Wi-Fi until reboot
    private const val TCP_PORT = 5555

    private class Manager(context: Context) : AbsAdbConnectionManager() {
        private val key: PrivateKey
        private val cert: Certificate

        init {
            setApi(Build.VERSION.SDK_INT)
            setHostAddress("127.0.0.1")
            setTimeout(10, TimeUnit.SECONDS)
            setThrowOnUnauthorised(true)
            val keyFile = File(context.filesDir, "mirror/adb_key.pk8")
            val certFile = File(context.filesDir, "mirror/adb_cert.der")
            val stored = runCatching {
                KeyFactory.getInstance("RSA").generatePrivate(PKCS8EncodedKeySpec(keyFile.readBytes())) to
                    CertificateFactory.getInstance("X.509").generateCertificate(certFile.inputStream())
            }.getOrNull()
            if (stored != null) {
                key = stored.first
                cert = stored.second
            } else {
                val pair = KeyPairGenerator.getInstance("RSA").apply { initialize(2048) }.generateKeyPair()
                val name = X500Name("CN=KDE Connect")
                val now = System.currentTimeMillis()
                val certBytes = JcaX509v3CertificateBuilder(name, BigInteger.valueOf(now), Date(now - 86_400_000L), Date(now + 30 * 365 * 86_400_000L), name, pair.public)
                    .build(JcaContentSignerBuilder("SHA256withRSA").build(pair.private)).encoded
                keyFile.parentFile?.mkdirs()
                keyFile.writeBytes(pair.private.encoded)
                certFile.writeBytes(certBytes)
                key = pair.private
                cert = CertificateFactory.getInstance("X.509").generateCertificate(certBytes.inputStream())
            }
        }

        override fun getPrivateKey() = key
        override fun getCertificate() = cert
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

    @Synchronized
    fun connect(context: Context): Boolean {
        val m = manager(context)
        if (m.isConnected) return true
        if (!isSupported()) return false
        if (connectTcpPort(m)) return true
        if (!isWifiConnected(context)) return false
        if (!connectTls(context, m)) return false
        if (!tcpPortOpen()) switchToTcpPort(context, m)
        return m.isConnected
    }

    private fun connectTls(context: Context, m: Manager): Boolean {
        val attempts = if (enableDebugging(context)) 3 else 1
        repeat(attempts) { attempt ->
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
        return try {
            manager(context).openStream(service)
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            throw IOException(e)
        }
    }

    @Throws(IOException::class)
    fun shell(context: Context, command: String): String =
        open(context, "shell:$command").use { readAll(it.openInputStream()).toString(Charsets.UTF_8) }

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
        val buf = ByteArray(8192)
        try {
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                out.write(buf, 0, n)
            }
        } catch (e: IOException) {
            if (e.message?.startsWith("Stream closed") != true) throw e
        }
        return out.toByteArray()
    }
}
