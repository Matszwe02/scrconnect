/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
package org.kde.kdeconnect.plugins.webcam

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class WebcamProbeTest {
    private fun stream(codec: Int, vararg packets: Pair<Long, Int>): ByteArray {
        val bytes = ByteArrayOutputStream()
        DataOutputStream(bytes).apply {
            write(ByteArray(64))
            writeInt(codec)
            for ((flags, size) in packets) {
                writeLong(flags)
                writeInt(size)
                if (flags >= 0) write(ByteArray(size))
            }
        }
        return bytes.toByteArray()
    }

    private val session = Long.MIN_VALUE or 1280L
    private val config = 1L shl 62

    private fun clock(): () -> Long {
        var now = 0L
        return { now.also { now += 100 } }
    }

    @Test
    fun readsUntilPicturesFlowedLongEnough() {
        val pictures = List(20) { (it * 33_000L) to 10 }.toTypedArray()
        val data = stream(0x68323634, session to 720, config to 20, *pictures)
        val read = WebcamPlugin.readUntilPictures(ByteArrayInputStream(data), forMs = 500, clock = clock())
        val expected = 64 + 4 + 12 + (12 + 20) + 5 * (12 + 10)
        assertEquals(expected, read!!.size)
        assertArrayEquals(data.copyOf(expected), read)
    }

    @Test
    fun aStreamThatEndsBeforePicturesFailed() {
        assertNull(WebcamPlugin.readUntilPictures(ByteArrayInputStream(stream(0x68323634, session to 720, config to 20)), clock = clock()))
        assertNull(WebcamPlugin.readUntilPictures(ByteArrayInputStream(ByteArray(64)), clock = clock()))
    }

    @Test
    fun aDisabledStreamFailed() {
        assertNull(WebcamPlugin.readUntilPictures(ByteArrayInputStream(stream(1, 0L to 10)), clock = clock()))
        assertNull(WebcamPlugin.readUntilPictures(ByteArrayInputStream(stream(0, 0L to 10)), clock = clock()))
    }
}
