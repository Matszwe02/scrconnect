/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.scrcpy

import org.junit.Assert
import org.junit.Test

class ScrcpyControlTest {
    private fun mouse(action: Int) = ScrcpyControl.mouse(action, 10, 10, 100, 100, ScrcpyControl.BUTTON_PRIMARY, ScrcpyControl.BUTTON_PRIMARY)

    private val scroll = ScrcpyControl.scroll(10, 10, 100, 100, 0f, 1f, 0)

    private fun text(text: String): ByteArray {
        val utf8 = text.toByteArray()
        return byteArrayOf(1) + java.nio.ByteBuffer.allocate(4).putInt(utf8.size).array() + utf8
    }

    @Test
    fun pressesWakeADarkWindow() {
        Assert.assertTrue(ScrcpyControl.isPress(mouse(ScrcpyControl.ACTION_DOWN)))
        Assert.assertTrue(ScrcpyControl.isPress(ScrcpyControl.keycode(ScrcpyControl.ACTION_DOWN, 29)))
        Assert.assertTrue(ScrcpyControl.isPress(text("a")))
    }

    @Test
    fun movesAndReleasesDoNot() {
        Assert.assertFalse(ScrcpyControl.isPress(mouse(ScrcpyControl.ACTION_UP)))
        Assert.assertFalse(ScrcpyControl.isPress(mouse(ScrcpyControl.ACTION_MOVE)))
        Assert.assertFalse(ScrcpyControl.isPress(mouse(ScrcpyControl.ACTION_HOVER_MOVE)))
        Assert.assertFalse(ScrcpyControl.isPress(ScrcpyControl.keycode(ScrcpyControl.ACTION_UP, 29)))
        Assert.assertFalse(ScrcpyControl.isPress(scroll))
    }

    @Test
    fun onlyInputIsHeldBack() {
        Assert.assertTrue(ScrcpyControl.isInput(mouse(ScrcpyControl.ACTION_HOVER_MOVE)))
        Assert.assertTrue(ScrcpyControl.isInput(scroll))
        Assert.assertFalse(ScrcpyControl.isInput(ScrcpyControl.startApp("org.example")))
        Assert.assertFalse(ScrcpyControl.isInput(ScrcpyControl.backOrScreenOn(ScrcpyControl.ACTION_DOWN)))
        Assert.assertFalse(ScrcpyControl.isInput(ScrcpyControl.displayPower(true)))
        Assert.assertFalse(ScrcpyControl.isInput(ByteArray(0)))
    }

    @Test
    fun knowsTheLengthOfEveryMessageItBuilds() {
        val messages = listOf(
            ScrcpyControl.keycode(ScrcpyControl.ACTION_DOWN, 29),
            text("héllo"),
            mouse(ScrcpyControl.ACTION_MOVE),
            scroll,
            ScrcpyControl.displayPower(false),
            ScrcpyControl.backOrScreenOn(ScrcpyControl.ACTION_DOWN),
            ScrcpyControl.uhidCreate(1, "keyboard", ByteArray(63)),
            ScrcpyControl.uhidInput(1, ByteArray(8)),
            ScrcpyControl.uhidDestroy(1),
            ScrcpyControl.openHardKeyboardSettings(),
            ScrcpyControl.startApp("org.example.app"),
        )
        for (m in messages) {
            Assert.assertEquals("type ${m[0]}", m.size, ScrcpyControl.messageLength(m, 0))
            Assert.assertNull("type ${m[0]}, incomplete", ScrcpyControl.messageLength(m.copyOf(m.size - 1), 0))
        }
    }

    @Test
    fun splitsAClickBehindHoverMovesInOneChunk() {
        val hover = mouse(ScrcpyControl.ACTION_HOVER_MOVE)
        val down = mouse(ScrcpyControl.ACTION_DOWN)
        val up = mouse(ScrcpyControl.ACTION_UP)
        val messages = ControlSplitter().push(hover + hover + down + up)
        Assert.assertEquals(listOf(hover, hover, down, up).map { it.toList() }, messages.map { it.toList() })
        Assert.assertTrue(messages.any(ScrcpyControl::isPress))
    }

    @Test
    fun joinsAMessageCutAcrossChunks() {
        val splitter = ControlSplitter()
        val down = mouse(ScrcpyControl.ACTION_DOWN)
        val typed = text("abc")
        val stream = down + typed
        Assert.assertTrue(splitter.push(stream.copyOfRange(0, 5)).isEmpty())
        Assert.assertEquals(listOf(down.toList()), splitter.push(stream.copyOfRange(5, 34)).map { it.toList() })
        Assert.assertEquals(listOf(typed.toList()), splitter.push(stream.copyOfRange(34, stream.size)).map { it.toList() })
        Assert.assertTrue(splitter.splitting)
    }

    @Test
    fun passesEverythingOnAfterAnUnknownType() {
        val splitter = ControlSplitter()
        val down = mouse(ScrcpyControl.ACTION_DOWN)
        val unknown = byteArrayOf(99, 1, 2, 3)
        val messages = splitter.push(down + unknown)
        Assert.assertFalse(splitter.splitting)
        Assert.assertEquals(listOf(down.toList(), unknown.toList()), messages.map { it.toList() })
        val later = byteArrayOf(2, 0, 0)
        Assert.assertEquals(listOf(later.toList()), splitter.push(later).map { it.toList() })
    }
}
