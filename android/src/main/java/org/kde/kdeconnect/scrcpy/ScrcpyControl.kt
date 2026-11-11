/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.io.InputStream
import java.nio.ByteBuffer

/** Control messages of scrcpy 5 (server/.../ControlMessageReader.java), big endian. */
object ScrcpyControl {
    const val ACTION_DOWN = 0
    const val ACTION_UP = 1
    const val ACTION_MOVE = 2
    const val ACTION_HOVER_MOVE = 7
    const val BUTTON_PRIMARY = 1
    const val BUTTON_SECONDARY = 2

    private const val TYPE_KEYCODE = 0
    private const val TYPE_TEXT = 1
    private const val TYPE_TOUCH = 2
    private const val TYPE_SCROLL = 3
    private const val TYPE_BACK_OR_SCREEN_ON = 4
    private const val TYPE_SET_CLIPBOARD = 9
    private const val TYPE_SET_DISPLAY_POWER = 10
    private const val TYPE_UHID_CREATE = 12
    private const val TYPE_UHID_INPUT = 13
    private const val TYPE_UHID_DESTROY = 14
    private const val TYPE_OPEN_HARD_KEYBOARD_SETTINGS = 15
    private const val TYPE_START_APP = 16
    private const val TYPE_RESIZE_DISPLAY = 21

    private const val POINTER_ID_MOUSE = -1L
    private const val MAX_MESSAGE = 1 shl 20

    fun isInput(message: ByteArray): Boolean =
        message.isNotEmpty() && message[0].toInt() in setOf(TYPE_KEYCODE, TYPE_TEXT, TYPE_TOUCH, TYPE_SCROLL, TYPE_UHID_INPUT)

    fun isPress(message: ByteArray): Boolean = when (message.firstOrNull()?.toInt()) {
        TYPE_TEXT -> true
        TYPE_KEYCODE, TYPE_TOUCH -> message.size > 1 && message[1].toInt() == ACTION_DOWN
        else -> false
    }

    /** Null while [data] does not hold the whole message yet, -1 for an unknown type. */
    fun messageLength(data: ByteArray, at: Int): Int? {
        val available = data.size - at
        fun u8(offset: Int): Int? = if (offset < available) data[at + offset].toInt() and 0xff else null
        fun u16(offset: Int): Int? = if (offset + 2 <= available) (u8(offset)!! shl 8) or u8(offset + 1)!! else null
        fun u32(offset: Int): Int? = if (offset + 4 <= available) ByteBuffer.wrap(data, at + offset, 4).int else null
        val length = when (u8(0) ?: return null) {
            TYPE_KEYCODE -> 14
            TYPE_TEXT -> 5 + (u32(1) ?: return null)
            TYPE_TOUCH -> 32
            TYPE_SCROLL -> 21
            TYPE_BACK_OR_SCREEN_ON, 8, TYPE_SET_DISPLAY_POWER -> 2
            5, 6, 7, 11, TYPE_OPEN_HARD_KEYBOARD_SETTINGS, 17 -> 1
            TYPE_SET_CLIPBOARD -> 14 + (u32(10) ?: return null)
            TYPE_UHID_CREATE -> {
                val name = u8(7) ?: return null
                10 + name + (u16(8 + name) ?: return null)
            }
            TYPE_UHID_INPUT -> 5 + (u16(3) ?: return null)
            TYPE_UHID_DESTROY -> 3
            TYPE_START_APP -> 2 + (u8(1) ?: return null)
            TYPE_RESIZE_DISPLAY -> 5
            else -> return -1
        }
        if (length < 1 || length > MAX_MESSAGE) return -1
        return if (length <= available) length else null
    }

    private inline fun build(type: Int, block: DataOutputStream.() -> Unit = {}): ByteArray {
        val bytes = ByteArrayOutputStream()
        DataOutputStream(bytes).apply { writeByte(type) }.block()
        return bytes.toByteArray()
    }

    fun keycode(action: Int, keycode: Int, metaState: Int = 0) = build(TYPE_KEYCODE) {
        writeByte(action)
        writeInt(keycode)
        writeInt(0)
        writeInt(metaState)
    }

    private fun DataOutputStream.position(x: Int, y: Int, width: Int, height: Int) {
        writeInt(x)
        writeInt(y)
        writeShort(width)
        writeShort(height)
    }

    fun mouse(action: Int, x: Int, y: Int, width: Int, height: Int, actionButton: Int, buttons: Int) = build(TYPE_TOUCH) {
        writeByte(action)
        writeLong(POINTER_ID_MOUSE)
        position(x, y, width, height)
        writeShort(if (action == ACTION_UP || action == ACTION_HOVER_MOVE) 0 else 0xffff)
        writeInt(actionButton)
        writeInt(buttons)
    }

    fun scroll(x: Int, y: Int, width: Int, height: Int, h: Float, v: Float, buttons: Int) = build(TYPE_SCROLL) {
        position(x, y, width, height)
        writeShort((h / 16f).coerceIn(-1f, 1f).times(0x7fff).toInt())
        writeShort((v / 16f).coerceIn(-1f, 1f).times(0x7fff).toInt())
        writeInt(buttons)
    }

    fun backOrScreenOn(action: Int) = build(TYPE_BACK_OR_SCREEN_ON) { writeByte(action) }

    fun displayPower(on: Boolean) = build(TYPE_SET_DISPLAY_POWER) { writeBoolean(on) }

    fun uhidCreate(id: Int, name: String, reportDescriptor: ByteArray) = build(TYPE_UHID_CREATE) {
        writeShort(id)
        writeShort(0x1d6b)
        writeShort(0x0104)
        writeByte(name.length)
        writeBytes(name)
        writeShort(reportDescriptor.size)
        write(reportDescriptor)
    }

    fun uhidInput(id: Int, report: ByteArray) = build(TYPE_UHID_INPUT) {
        writeShort(id)
        writeShort(report.size)
        write(report)
    }

    fun uhidDestroy(id: Int) = build(TYPE_UHID_DESTROY) { writeShort(id) }

    fun openHardKeyboardSettings() = build(TYPE_OPEN_HARD_KEYBOARD_SETTINGS)

    fun startApp(packageName: String) = build(TYPE_START_APP) {
        writeByte(packageName.length)
        writeBytes(packageName)
    }
}

/**
 * Cuts the computer's control stream, which comes in chunks of any size, into whole messages. After an unknown type it
 * hands everything on as it comes ([splitting] false).
 */
class ControlSplitter {
    private var pending = ByteArray(0)

    @Volatile
    var splitting = true
        private set

    @Synchronized
    fun push(data: ByteArray): List<ByteArray> {
        if (!splitting) return listOf(data)
        val buffer = pending + data
        val messages = mutableListOf<ByteArray>()
        var at = 0
        while (at < buffer.size) {
            val length = ScrcpyControl.messageLength(buffer, at) ?: break
            if (length < 0) {
                splitting = false
                messages += buffer.copyOfRange(at, buffer.size)
                pending = ByteArray(0)
                return messages
            }
            messages += buffer.copyOfRange(at, at + length)
            at += length
        }
        pending = buffer.copyOfRange(at, buffer.size)
        return messages
    }
}

fun readClipboardMessages(input: InputStream, onClipboard: (String) -> Unit) {
    val data = DataInputStream(input)
    while (true) {
        when (val type = data.read()) {
            -1 -> return
            0 -> {
                val length = data.readInt()
                if (length !in 0..(1 shl 20)) throw IOException("Corrupt device message")
                onClipboard(String(ByteArray(length).also { data.readFully(it) }))
            }
            1 -> data.readLong()
            2 -> {
                data.readUnsignedShort()
                data.readFully(ByteArray(data.readUnsignedShort()))
            }
            else -> throw IOException("Unknown device message $type")
        }
    }
}
