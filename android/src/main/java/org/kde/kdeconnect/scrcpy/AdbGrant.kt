/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.content.ComponentName
import android.content.Context
import android.provider.Settings
import org.kde.kdeconnect.KdeConnectAccessibilityService
import org.kde.kdeconnect.plugins.notifications.NotificationReceiver
import org.kde.kdeconnect.plugins.remotekeyboardime.RemoteKeyboardService

/** Shell commands that give this app what the user otherwise turns on in Android's settings; see Plugin.adbGrantCommands. */
object AdbGrant {
    private fun component(context: Context, service: Class<*>) = ComponentName(context, service).flattenToString()

    fun notificationListener(context: Context) = listOf("cmd notification allow_listener ${component(context, NotificationReceiver::class.java)}")

    fun inputMethod(context: Context) = listOf("ime enable ${component(context, RemoteKeyboardService::class.java)}")

    fun allFilesAccess(context: Context) = listOf("appops set ${context.packageName} MANAGE_EXTERNAL_STORAGE allow")

    fun accessibility(context: Context): List<String> {
        val key = Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES
        val service = ComponentName(context, KdeConnectAccessibilityService::class.java)
        val enabled = Settings.Secure.getString(context.contentResolver, key).orEmpty().split(':').filter { it.isNotEmpty() }
        val others = enabled.filter { ComponentName.unflattenFromString(it) != service }
        return listOfNotNull(
            // Android 13+ keeps apps that did not come from a store out of the accessibility settings until this is allowed
            "appops set ${context.packageName} ACCESS_RESTRICTED_SETTINGS allow",
            // Listed but not running (after an update or a crash): Android starts it again only once it was taken out
            (if (others.isEmpty()) "settings delete secure $key" else "settings put secure $key '${others.joinToString(":")}'")
                .takeIf { others.size != enabled.size },
            "settings put secure $key '${(others + service.flattenToString()).joinToString(":")}'",
            "settings put secure accessibility_enabled 1",
        )
    }
}
