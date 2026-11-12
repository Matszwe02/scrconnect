/*
 * SPDX-FileCopyrightText: 2014 Albert Vaca Cintora <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.plugins.clipboard

import android.Manifest
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.os.Build
import android.widget.Toast
import androidx.annotation.VisibleForTesting
import androidx.core.content.ContextCompat
import androidx.core.content.getSystemService
import org.kde.kdeconnect.NetworkPacket
import org.kde.kdeconnect.plugins.Plugin
import org.kde.kdeconnect.plugins.PluginFactory.LoadablePlugin
import org.kde.kdeconnect.plugins.clipboard.ClipboardListener.ClipboardObserver
import org.kde.kdeconnect.scrcpy.SelfAdb
import org.kde.kdeconnect.scrcpy.ShellBridge
import org.kde.kdeconnect.ui.PluginSettingsFragment
import org.kde.kdeconnect_tp.R

@LoadablePlugin
class ClipboardPlugin : Plugin() {
    override val displayName: String
        get() = context.resources.getString(R.string.pref_plugin_clipboard)

    override val description: String
        get() = context.resources.getString(R.string.pref_plugin_clipboard_desc)

    override val hasSettings: Boolean = true

    override val settingsScope = SettingsScope.PER_DEVICE

    override fun getSettingsFragment(activity: Activity): PluginSettingsFragment {
        return PluginSettingsFragment.newInstance(pluginKey, device.deviceId, R.xml.clipboardplugin_preferences)
    }

    override fun onPacketReceived(np: NetworkPacket): Boolean {
        val content = np.getString("content")
        when (np.type) {
            (PACKET_TYPE_CLIPBOARD) -> {
                ClipboardListener.instance(context).setText(content)
                return true
            }
            (PACKET_TYPE_CLIPBOARD_CONNECT) -> {
                val packetTime = np.getLong("timestamp")
                // If the packetTime is 0, it means the timestamp is unknown (so do nothing).
                if (packetTime == 0L || packetTime < ClipboardListener.instance(context).updateTimestamp) {
                    return false
                }
                if ("content" in np) { // change clipboard if content is in NetworkPacket
                    ClipboardListener.instance(context).setText(content)
                }
                return true
            }
            else -> throw UnsupportedOperationException("Unknown packet type: ${np.type}")
        }
    }

    private val observer: ClipboardObserver = object : ClipboardObserver {
        override fun clipboardChanged(content: String, contentType: ClipboardListener.ClipboardContentType) {
            if (contentType == ClipboardListener.ClipboardContentType.Password &&
                preferences.getBoolean(
                    context.getString(R.string.clipboard_preference_key_skip_sensitive),
                    false,
                )
            ) {
                return
            }
            propagateClipboard(content)
        }
    }

    @VisibleForTesting
    fun propagateClipboard(content: String) {
        val np = NetworkPacket(PACKET_TYPE_CLIPBOARD)
        np["content"] = content
        device.sendPacket(np)
    }

    private fun sendConnectPacket() {
        val content = ClipboardListener.instance(context).currentContent ?: return // Send clipboard only if it had been initialized
        if (ClipboardListener.instance(context).currentContentType == ClipboardListener.ClipboardContentType.Password &&
            preferences.getBoolean(
                context.getString(R.string.clipboard_preference_key_skip_sensitive),
                false,
            )
        ) {
            return
        }
        val np = NetworkPacket(PACKET_TYPE_CLIPBOARD_CONNECT)
        val timestamp = ClipboardListener.instance(context).updateTimestamp
        np["timestamp"] = timestamp
        np["content"] = content
        device.sendPacket(np)
    }


    private val bridgeUser get() = "$SHELL_BRIDGE_USER/${device.deviceId}"

    /**
     * Only the focused app may read the clipboard, but the shell user may too: with the ADB setup a scrcpy server reports
     * every change. It sees plain text only, so it is off while sensitive content must be skipped.
     */
    private fun updateShellBridge() {
        val skipSensitive = preferences.getBoolean(context.getString(R.string.clipboard_preference_key_skip_sensitive), false)
        val enabled = preferences.getBoolean(PREF_SHELL_BRIDGE, true)
        if (SelfAdb.isSupported() && enabled && !skipSensitive) ShellBridge.acquire(context, bridgeUser) else ShellBridge.release(bridgeUser)
    }

    // A field: SharedPreferences only keeps weak references to its listeners
    private val preferenceListener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ -> updateShellBridge() }

    override fun onCreate() {
        ClipboardListener.instance(context).registerObserver(observer)
        sendConnectPacket()
        preferences.registerOnSharedPreferenceChangeListener(preferenceListener)
        updateShellBridge()
    }

    override fun onDestroy() {
        ClipboardListener.instance(context).removeObserver(observer)
        preferences.unregisterOnSharedPreferenceChangeListener(preferenceListener)
        ShellBridge.release(bridgeUser)
    }

    override val supportedPacketTypes: Array<String> = arrayOf(PACKET_TYPE_CLIPBOARD, PACKET_TYPE_CLIPBOARD_CONNECT)

    override val outgoingPacketTypes: Array<String> = arrayOf(PACKET_TYPE_CLIPBOARD, PACKET_TYPE_CLIPBOARD_CONNECT)

    override fun getUiButtons(): List<PluginUiButton> {
        return if (!canSyncAutomatically(context)) {
            listOf(PluginUiButton(context.getString(R.string.send_clipboard), R.drawable.ic_baseline_content_paste_24) { _: Activity? ->
                userInitiatedSendClipboard()
            })
        } else {
            emptyList()
        }
    }

    override fun getUiMenuEntries(): List<PluginUiMenuEntry> {
        return if (canSyncAutomatically(context)) {
            // If automatic sync is supported, add the manual action less prominently, as a menu entry instead of a button.
            listOf(PluginUiMenuEntry(context.getString(R.string.send_clipboard)) { _: Activity? ->
                userInitiatedSendClipboard()
            })
        } else {
            emptyList()
        }
    }

    private fun userInitiatedSendClipboard() {
        if (isDeviceInitialized) {
            val clipboardManager = context.getSystemService<ClipboardManager>()!!
            val item: ClipData.Item
            if (clipboardManager.hasPrimaryClip()) {
                item = clipboardManager.primaryClip!!.getItemAt(0)
                val content = item.coerceToText(this.context).toString()
                // Don't check if the content is sensitive, just send it
                this.propagateClipboard(content)
                Toast.makeText(this.context, R.string.pref_plugin_clipboard_sent, Toast.LENGTH_SHORT).show()
            }
        }
    }

    companion object {
        const val SHELL_BRIDGE_USER = "clipboard"
        private const val PREF_SHELL_BRIDGE = "clipboard_shell_bridge"

        /**
         * Packet containing just clipboard contents, sent when a device updates its clipboard.
         *
         *
         * The body should look like so:
         * {
         * "content": "password"
         * }
         */
        private const val PACKET_TYPE_CLIPBOARD = "kdeconnect.clipboard"

        /**
         * Packet containing clipboard contents and a timestamp that the contents were last updated, sent
         * on first connection
         *
         *
         * The timestamp is milliseconds since epoch. It can be 0, which indicates that the clipboard
         * update time is currently unknown.
         *
         *
         * The body should look like so:
         * {
         * "timestamp": 542904563213,
         * "content": "password"
         * }
         */
        private const val PACKET_TYPE_CLIPBOARD_CONNECT = "kdeconnect.clipboard.connect"

        fun canSyncAutomatically(context: Context): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
                return true
            }
            // Check if hack to read clipboard changes from logs is enabled
            return ContextCompat.checkSelfPermission(context, Manifest.permission.READ_LOGS) == PackageManager.PERMISSION_GRANTED
        }

    }
}
