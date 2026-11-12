/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.plugins.screenmirror

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import org.kde.kdeconnect.scrcpy.ScrcpySession
import org.kde.kdeconnect.scrcpy.SelfAdb

/**
 * Moves apps between the phone's screen and mirror windows with shell commands: `cmd activity display move-stack` up to
 * Android 11; Android 12 removed it, there the activity is started on the other display, which brings its task over.
 */
object Handoff {
    data class Task(val packageName: String, val id: Int, val component: String?)

    private val STACK = Regex("(?m)^(?:Stack|RootTask) id=")
    private val TASK = Regex("taskId=(\\d+): ([A-Za-z][\\w.]*)/")
    private val TOP_ACTIVITY = Regex("topActivity=ComponentInfo\\{([^}]+)\\}")
    private val RESUMED = Regex("ResumedActivity.*ActivityRecord\\{\\S+ u\\d+ (([A-Za-z][\\w.]*)/[\\w.$]+) t(\\d+)")
    private val COMPONENT = Regex("[A-Za-z][\\w.]*/[\\w.$]+")

    private fun shell(context: Context, command: String): String = runCatching { SelfAdb.shell(context, "$command 2>&1") }.getOrDefault("")

    private fun skipped(context: Context) = setOfNotNull(
        context.packageName,
        "com.android.systemui",
        context.packageManager.resolveActivity(Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME), PackageManager.MATCH_DEFAULT_ONLY)
            ?.activityInfo?.packageName,
    )

    fun top(context: Context, displayId: Int): Task? {
        val skip = skipped(context)
        for (stack in shell(context, "cmd activity stack list").split(STACK)) {
            if (!stack.lineSequence().first().contains("displayId=$displayId ") || "visible=true" !in stack) continue
            if ("mActivityType=home" in stack || "mActivityType=recents" in stack) continue
            val task = TASK.find(stack) ?: continue
            if (task.groupValues[2] in skip) continue
            return Task(task.groupValues[2], task.groupValues[1].toInt(), TOP_ACTIVITY.find(stack)?.groupValues?.get(1))
        }
        return null
    }

    fun foreground(context: Context): Task? = top(context, 0)
        ?: RESUMED.find(shell(context, "dumpsys activity activities | grep ResumedActivity | head -1"))?.let {
            Task(it.groupValues[2], it.groupValues[3].toInt(), it.groupValues[1]).takeIf { t -> t.packageName !in skipped(context) }
        }

    fun find(context: Context, packageName: String): Task? =
        Regex("taskId=(\\d+): ${Regex.escape(packageName)}/").find(shell(context, "cmd activity stack list"))?.let {
            Task(packageName, it.groupValues[1].toInt(), null)
        }

    // Only task list entries ("* Task{" from Android 11, "* TaskRecord{" before): other lines keep naming a task after it is gone
    fun exists(context: Context, taskId: Int): Boolean =
        Regex("(?m)^\\s*\\* Task\\w*\\{\\S+ #$taskId\\b").containsMatchIn(shell(context, "dumpsys activity activities | grep -F '* Task'"))

    fun move(context: Context, task: Task, displayId: Int): Boolean {
        fun ok(out: String) = listOf("Error", "Exception", "Unknown").none { out.contains(it, ignoreCase = true) }
        if (ok(shell(context, "cmd activity display move-stack ${task.id} $displayId"))) return true
        val component = task.component?.takeIf { COMPONENT.matches(it) } ?: return false
        return ok(shell(context, "am start --display $displayId -n '$component' -f 0x20000")) // FLAG_ACTIVITY_REORDER_TO_FRONT
    }

    fun bringToFront(context: Context, packageName: String, component: String?) {
        if (!ScrcpySession.isValidPackageName(packageName)) return
        if (component != null && COMPONENT.matches(component)) {
            val out = shell(context, "am start -n '$component' -f 0x20000")
            if (!out.contains("Error", ignoreCase = true)) return
        }
        shell(context, "monkey -p $packageName -c android.intent.category.LAUNCHER 1")
    }
}
