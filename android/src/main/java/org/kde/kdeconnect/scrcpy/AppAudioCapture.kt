/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 * SPDX-FileCopyrightText: 2018 Genymobile, Romain Vimont (the audio policy and app environment set-up, from scrcpy's
 * AudioPlaybackCapture, FakeContext and Workarounds, Apache-2.0)
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.annotation.SuppressLint
import android.app.Application
import android.app.Instrumentation
import android.content.AttributionSource
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ApplicationInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.os.Build
import android.os.Looper
import java.io.BufferedOutputStream
import java.io.DataOutputStream
import java.io.FileDescriptor
import java.io.FileOutputStream

/**
 * The sound of one app, run as the shell user with app_process (Android 13+). An audio policy mix matching the app's uid
 * loops it back, so the app is silent on the phone. Writes what scrcpy's audio socket carries with the raw codec.
 */
object AppAudioCapture {
    private const val SHELL_PACKAGE = "com.android.shell"
    private const val SHELL_UID = 2000

    fun command(context: Context, uid: Int) =
        "CLASSPATH=${context.applicationInfo.sourceDir} app_process / ${AppAudioCapture::class.java.name} $uid 2>/dev/null"

    @JvmStatic
    fun main(args: Array<String>) {
        Looper.prepareMainLooper()
        val record = createRecord(shellContext(), args.single().toInt())
        record.startRecording()
        val out = DataOutputStream(BufferedOutputStream(FileOutputStream(FileDescriptor.out)))
        out.write(byteArrayOf(0, 'r'.code.toByte(), 'a'.code.toByte(), 'w'.code.toByte()))
        val buffer = ByteArray(4096)
        while (true) {
            val n = record.read(buffer, 0, buffer.size)
            if (n < 0) break
            if (n == 0) continue
            out.writeLong(0)
            out.writeInt(n)
            out.write(buffer, 0, n)
            out.flush()
        }
    }

    private fun audioPolicy(name: String) = Class.forName("android.media.audiopolicy.$name")

    /** Calls a public method of a hidden class; every argument comes with its declared type. */
    private fun Any.call(name: String, vararg args: Pair<Class<*>, Any?>): Any? =
        javaClass.getMethod(name, *args.map { it.first }.toTypedArray()).invoke(this, *args.map { it.second }.toTypedArray())

    @SuppressLint("PrivateApi")
    private fun createRecord(context: Context, uid: Int): AudioRecord {
        val int = Int::class.java
        val ruleClass = audioPolicy("AudioMixingRule")
        val rule = audioPolicy("AudioMixingRule\$Builder").getConstructor().newInstance().apply {
            call("setTargetMixRole", int to ruleClass.getField("MIX_ROLE_PLAYERS").getInt(null))
            call("addMixRule", int to ruleClass.getField("RULE_MATCH_UID").getInt(null), Any::class.java to uid)
        }.call("build")

        val mixClass = audioPolicy("AudioMix")
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(48000)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()
        val mix = audioPolicy("AudioMix\$Builder").getConstructor(ruleClass).newInstance(rule).apply {
            call("setFormat", AudioFormat::class.java to format)
            call("setRouteFlags", int to mixClass.getField("ROUTE_FLAG_LOOP_BACK").getInt(null))
        }.call("build")

        val policyClass = audioPolicy("AudioPolicy")
        val policy = audioPolicy("AudioPolicy\$Builder").getConstructor(Context::class.java).newInstance(context).apply {
            call("addMix", mixClass to mix)
        }.call("build")!!
        val register = AudioManager::class.java.getDeclaredMethod("registerAudioPolicyStatic", policyClass).apply { isAccessible = true }
        val result = register.invoke(null, policy) as Int
        check(result == 0) { "registerAudioPolicy() returned $result" }
        return policy.call("createAudioRecordSink", mixClass to mix) as AudioRecord
    }

    @SuppressLint("PrivateApi", "DiscouragedPrivateApi")
    private fun shellContext(): Context {
        val threadClass = Class.forName("android.app.ActivityThread")
        fun field(name: String) = threadClass.getDeclaredField(name).apply { isAccessible = true }
        val thread = threadClass.getDeclaredConstructor().apply { isAccessible = true }.newInstance()
        field("sCurrentActivityThread").set(null, thread)
        field("mSystemThread").setBoolean(thread, true)
        if (Build.VERSION.SDK_INT >= 31) {
            runCatching {
                val controllerClass = Class.forName("android.app.ConfigurationController")
                val controller = controllerClass.getDeclaredConstructor(Class.forName("android.app.ActivityThreadInternal"))
                    .apply { isAccessible = true }.newInstance(thread)
                field("mConfigurationController").set(thread, controller)
            }
        }
        runCatching {
            val bindDataClass = Class.forName("android.app.ActivityThread\$AppBindData")
            val bindData = bindDataClass.getDeclaredConstructor().apply { isAccessible = true }.newInstance()
            bindDataClass.getDeclaredField("appInfo").apply { isAccessible = true }.set(bindData, ApplicationInfo().apply { packageName = SHELL_PACKAGE })
            field("mBoundApplication").set(thread, bindData)
        }
        val systemContext = threadClass.getDeclaredMethod("getSystemContext").invoke(thread) as Context
        val context = ShellContext(systemContext)
        runCatching { field("mInitialApplication").set(thread, Instrumentation.newApplication(Application::class.java, context)) }
        return context
    }

    private class ShellContext(base: Context) : ContextWrapper(base) {
        override fun getPackageName() = SHELL_PACKAGE
        override fun getOpPackageName() = SHELL_PACKAGE
        override fun getApplicationContext() = this

        override fun getAttributionSource(): AttributionSource =
            if (Build.VERSION.SDK_INT >= 31) AttributionSource.Builder(SHELL_UID).setPackageName(SHELL_PACKAGE).build() else super.getAttributionSource()
    }
}
