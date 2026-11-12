/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.plugins.inputdevicesreceiver

import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.util.DisplayMetrics
import android.view.Display
import org.kde.kdeconnect.KdeConnectAccessibilityService
import org.kde.kdeconnect.scrcpy.ScrcpyControl
import org.kde.kdeconnect.scrcpy.ShellBridge

/**
 * Exact input from the computer, injected as the shell user through the [ShellBridge]: the mouse as scrcpy mouse events,
 * keys through a virtual USB keyboard. This class owns the pointer position; the accessibility service only draws it.
 * [exitEdge] is the computer's edge the pointer left through (Qt::Edge), [along] the position along it, 0..1.
 */
class ShellInput(context: Context, private val exitEdge: Int, along: Float, private val user: String) {
    private val app = context.applicationContext
    private val displays = app.getSystemService(DisplayManager::class.java)
    private val metrics = DisplayMetrics()
    private var x = 0f
    private var y = 0f
    private var overshoot = 0f
    private var buttons = 0
    private val modifiers = BooleanArray(8)
    private val pressed = ArrayList<Int>()
    private var keyboardGeneration = -1

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
        override fun onDisplayChanged(displayId: Int) {
            if (displayId != Display.DEFAULT_DISPLAY) return
            val (w, h) = width to height
            updateSize()
            x = x / w * width
            y = y / h * height
        }
    }

    val dragging get() = buttons and ScrcpyControl.BUTTON_PRIMARY != 0

    private val width get() = metrics.widthPixels.toFloat()
    private val height get() = metrics.heightPixels.toFloat()

    init {
        ShellBridge.acquire(app, user)
        updateSize()
        displays.registerDisplayListener(displayListener, Handler(Looper.getMainLooper()))
        val a = along.coerceIn(0f, 1f)
        when (exitEdge) {
            TOP -> { x = a * (width - 1); y = height - 1 }
            BOTTOM -> { x = a * (width - 1); y = 0f }
            LEFT -> { x = width - 1; y = a * (height - 1) }
            else -> { x = 0f; y = a * (height - 1) }
        }
        if (!app.getSystemService(PowerManager::class.java).isInteractive) {
            ShellBridge.send(ScrcpyControl.backOrScreenOn(ScrcpyControl.ACTION_DOWN))
            ShellBridge.send(ScrcpyControl.backOrScreenOn(ScrcpyControl.ACTION_UP))
        }
        pointer(ScrcpyControl.ACTION_HOVER_MOVE, 0)
    }

    private fun updateSize() {
        @Suppress("DEPRECATION")
        displays.getDisplay(Display.DEFAULT_DISPLAY).getRealMetrics(metrics)
    }

    fun end() {
        if (buttons and ScrcpyControl.BUTTON_PRIMARY != 0) button(BTN_LEFT, false)
        if (buttons and ScrcpyControl.BUTTON_SECONDARY != 0) button(BTN_RIGHT, false)
        if (pressed.isNotEmpty() || modifiers.any { it }) {
            pressed.clear()
            modifiers.fill(false)
            keyboardReport()
        }
        if (keyboardGeneration == ShellBridge.generation) ShellBridge.send(ScrcpyControl.uhidDestroy(KEYBOARD_ID))
        KdeConnectAccessibilityService.instance?.hideCursorNow()
        displays.unregisterDisplayListener(displayListener)
        ShellBridge.release(user)
    }

    private fun pointer(action: Int, actionButton: Int) {
        KdeConnectAccessibilityService.instance?.showCursorAt(x.toInt(), y.toInt())
        ShellBridge.send(ScrcpyControl.mouse(action, x.toInt(), y.toInt(), metrics.widthPixels, metrics.heightPixels, actionButton, buttons))
    }

    fun move(dx: Float, dy: Float): Float? {
        val nx = x + dx
        val ny = y + dy
        val beyond = when (exitEdge) {
            TOP -> ny - (height - 1)
            BOTTOM -> -ny
            LEFT -> nx - (width - 1)
            else -> -nx
        }
        x = nx.coerceIn(0f, width - 1)
        y = ny.coerceIn(0f, height - 1)
        overshoot = if (beyond > 0) overshoot + beyond else 0f
        if (overshoot > 2) return if (exitEdge == LEFT || exitEdge == RIGHT) y / (height - 1) else x / (width - 1)
        pointer(if (buttons != 0) ScrcpyControl.ACTION_MOVE else ScrcpyControl.ACTION_HOVER_MOVE, 0)
        return null
    }

    fun button(code: Int, down: Boolean) {
        val bit = when (code) {
            BTN_LEFT -> ScrcpyControl.BUTTON_PRIMARY
            BTN_RIGHT -> ScrcpyControl.BUTTON_SECONDARY
            else -> {
                when (code) {
                    BTN_MIDDLE -> key(KEYCODE_HOME, down)
                    BTN_SIDE -> key(KEYCODE_BACK, down)
                    BTN_EXTRA -> key(KEYCODE_APP_SWITCH, down)
                }
                return
            }
        }
        buttons = if (down) buttons or bit else buttons and bit.inv()
        pointer(if (down) ScrcpyControl.ACTION_DOWN else ScrcpyControl.ACTION_UP, bit)
    }

    fun scroll(h: Float, v: Float) {
        ShellBridge.send(ScrcpyControl.scroll(x.toInt(), y.toInt(), metrics.widthPixels, metrics.heightPixels, h, v, buttons))
    }

    private fun key(keycode: Int, down: Boolean) =
        ShellBridge.send(ScrcpyControl.keycode(if (down) ScrcpyControl.ACTION_DOWN else ScrcpyControl.ACTION_UP, keycode))

    fun evdevKey(evdev: Int, down: Boolean) {
        val usage = EVDEV_TO_HID[evdev] ?: return
        if (usage >= 0xE0) {
            if (modifiers[usage - 0xE0] == down) return
            modifiers[usage - 0xE0] = down
        } else if (down) {
            if (usage in pressed) return
            pressed += usage
        } else if (!pressed.remove(usage)) {
            return
        }
        if (keyboardGeneration != ShellBridge.generation) {
            // A new bridge server has no keyboard yet
            if (!ShellBridge.send(ScrcpyControl.uhidCreate(KEYBOARD_ID, "SCR Connect keyboard", REPORT_DESCRIPTOR))) return
            keyboardGeneration = ShellBridge.generation
            val prefs = androidx.preference.PreferenceManager.getDefaultSharedPreferences(app)
            if (!prefs.getBoolean(PREF_LAYOUT_HINT, false)) {
                prefs.edit().putBoolean(PREF_LAYOUT_HINT, true).apply()
                ShellBridge.send(ScrcpyControl.openHardKeyboardSettings())
            }
        }
        keyboardReport()
    }

    /** Boot keyboard report: modifier bits, a reserved byte, up to six keys (0x01 = too many keys). */
    private fun keyboardReport() {
        val report = ByteArray(8)
        modifiers.forEachIndexed { i, down -> if (down) report[0] = (report[0].toInt() or (1 shl i)).toByte() }
        pressed.forEachIndexed { i, usage -> if (i < 6) report[2 + i] = (if (pressed.size > 6) 1 else usage).toByte() }
        ShellBridge.send(ScrcpyControl.uhidInput(KEYBOARD_ID, report))
    }

    companion object {
        // Qt::Edge of the computer
        const val TOP = 0x1
        const val LEFT = 0x2
        const val RIGHT = 0x4
        const val BOTTOM = 0x8

        const val BTN_LEFT = 0x110
        const val BTN_RIGHT = 0x111
        const val BTN_MIDDLE = 0x112
        const val BTN_SIDE = 0x113
        const val BTN_EXTRA = 0x114

        private const val KEYCODE_HOME = 3
        private const val KEYCODE_BACK = 4
        private const val KEYCODE_APP_SWITCH = 187
        private const val KEYBOARD_ID = 0x4b43
        private const val PREF_LAYOUT_HINT = "shellinput_keyboard_hint_shown"

        // The standard boot keyboard, as scrcpy's app/src/hid/hid_keyboard.c (Apache-2.0)
        private val REPORT_DESCRIPTOR = intArrayOf(
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
            0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
            0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
            0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
            0xC0,
        ).map { it.toByte() }.toByteArray()

        // Linux key code (linux/input-event-codes.h) -> USB HID keyboard usage
        private val EVDEV_TO_HID: Map<Int, Int> = buildMap {
            intArrayOf(30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44)
                .forEachIndexed { i, code -> put(code, 4 + i) } // a..z
            for (i in 0..9) put(2 + i, 30 + i) // 1..9, 0
            // enter esc backspace tab space - = [ ] \ ; ' ` , . / capslock
            intArrayOf(28, 1, 14, 15, 57, 12, 13, 26, 27, 43).forEachIndexed { i, code -> put(code, 40 + i) }
            intArrayOf(39, 40, 41, 51, 52, 53, 58).forEachIndexed { i, code -> put(code, 51 + i) }
            for (i in 0..9) put(59 + i, 58 + i) // F1..F10
            put(87, 68); put(88, 69) // F11, F12
            // print scrolllock pause insert home pageup delete end pagedown right left down up numlock kp/ kp* kp- kp+ kpenter
            intArrayOf(99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103, 69, 98, 55, 74, 78, 96)
                .forEachIndexed { i, code -> put(code, 70 + i) }
            intArrayOf(79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, 86, 127).forEachIndexed { i, code -> put(code, 89 + i) } // kp1..kp. 102nd menu
            // ctrl shift alt meta, left then right
            intArrayOf(29, 42, 56, 125, 97, 54, 100, 126).forEachIndexed { i, code -> put(code, 0xE0 + i) }
        }
    }
}
