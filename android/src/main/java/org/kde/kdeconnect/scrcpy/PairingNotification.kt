/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import android.annotation.SuppressLint
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.RemoteInput
import kotlin.concurrent.thread
import org.kde.kdeconnect.helpers.NotificationHelper
import org.kde.kdeconnect_tp.R

// Android closes the pairing dialog when another app comes to the front, so the code is typed into a notification reply
object PairingNotification {
    private const val ID = 0x4d49
    private const val KEY_CODE = "code"

    @SuppressLint("MissingPermission")
    fun show(context: Context, error: String? = null) {
        if (error == null && isShown(context)) return
        val intent = PendingIntent.getBroadcast(
            context, 0, Intent(context, Receiver::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
        )
        val action = NotificationCompat.Action.Builder(R.drawable.ic_phonelink_36dp, context.getString(R.string.mirror_pairing_code_hint), intent)
            .addRemoteInput(RemoteInput.Builder(KEY_CODE).setLabel(context.getString(R.string.mirror_pairing_code_hint)).build())
            .build()
        val notification = NotificationCompat.Builder(context, NotificationHelper.Channels.HIGHPRIORITY)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(context.getString(R.string.mirror_setup_title))
            .setContentText(error ?: context.getString(R.string.mirror_notification_enter_code))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(action)
            .build()
        NotificationManagerCompat.from(context).notify(ID, notification)
    }

    fun cancel(context: Context) = NotificationManagerCompat.from(context).cancel(ID)

    private fun isShown(context: Context): Boolean =
        context.getSystemService(NotificationManager::class.java).activeNotifications.any { it.id == ID }

    class Receiver : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val code = RemoteInput.getResultsFromIntent(intent)?.getCharSequence(KEY_CODE)?.toString()?.trim().orEmpty()
            val app = context.applicationContext
            val pending = goAsync()
            thread {
                try {
                    val error = SelfAdb.pair(app, code)
                    if (error == null) cancel(app) else show(app, error)
                } finally {
                    pending.finish()
                }
            }
        }
    }
}
