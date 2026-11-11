/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.app.Activity
import android.content.Intent
import java.io.InputStream
import java.util.concurrent.Executors
import org.kde.kdeconnect.Device
import org.kde.kdeconnect.NetworkPacket
import org.kde.kdeconnect.plugins.Plugin

abstract class ScrcpyPlugin : Plugin() {
    protected abstract val idKey: String
    protected abstract val statusType: String

    protected val executor = Executors.newCachedThreadPool()

    override fun checkRequiredPermissions(): Boolean = SelfAdb.isSetUp(context)
    override fun loadPluginWhenRequiredPermissionsMissing(): Boolean = true
    override fun openPermissionsSetup(activity: Activity): Boolean {
        activity.startActivity(Intent(activity, SetupActivity::class.java))
        return true
    }

    override fun onDestroy() {
        executor.shutdownNow()
    }

    protected fun status(id: String, state: String, message: String? = null) {
        val np = NetworkPacket(statusType)
        np[idKey] = id
        np["state"] = state
        message?.let { np["message"] = it }
        device.sendPacket(np)
    }

    protected fun relay(type: String, id: String, input: InputStream, onEnd: () -> Unit = {}) {
        val np = NetworkPacket(type)
        np[idKey] = id
        np.payload = NetworkPacket.Payload(input, -1)
        executor.execute {
            try {
                device.sendPacketBlocking(np, IGNORE, true)
            } finally {
                onEnd()
            }
        }
    }

    companion object {
        const val STATE_NEEDS_SETUP = "needs_setup"
        const val STATE_STOPPED = "stopped"
        const val STATE_ERROR = "error"

        private val IGNORE = object : Device.SendPacketStatusCallback() {
            override fun onSuccess() {}
            override fun onFailure(e: Throwable) {}
        }
    }
}
