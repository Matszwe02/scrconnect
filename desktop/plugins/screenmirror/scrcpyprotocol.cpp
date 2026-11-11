/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "scrcpyprotocol.h"

#include <QDataStream>
#include <QIODevice>
#include <QtEndian>

#include <algorithm>

namespace Scrcpy
{
namespace
{
constexpr qsizetype kDeviceNameLength = 64;
constexpr quint32 kMaxFrameSize = 32 * 1024 * 1024;

quint32 u32(const QByteArray &b, qsizetype at)
{
    return qFromBigEndian<quint32>(b.constData() + at);
}

template<typename F>
QByteArray message(quint8 type, F write)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << type;
    write(out);
    return bytes;
}

QByteArray message(quint8 type)
{
    return QByteArray(1, char(type));
}

void writePosition(QDataStream &out, QPoint position, QSize frameSize)
{
    out << qint32(position.x()) << qint32(position.y()) << quint16(frameSize.width()) << quint16(frameSize.height());
}

void writeString(QDataStream &out, const QByteArray &utf8)
{
    out << quint32(utf8.size());
    out.writeRawData(utf8.constData(), int(utf8.size()));
}
}

QList<VideoEvent> VideoParser::push(const QByteArray &data)
{
    QList<VideoEvent> events;
    if (m_state == Failed) {
        return events;
    }
    m_buffer += data;
    qsizetype pos = 0;
    auto available = [&] {
        return m_buffer.size() - pos;
    };
    auto fail = [&](const QString &why) {
        m_error = why;
        m_state = Failed;
        m_buffer.clear();
        pos = 0;
    };
    while (m_state != Failed) {
        if (m_state == Name) {
            if (available() < kDeviceNameLength) {
                break;
            }
            const QByteArray name = m_buffer.mid(pos, kDeviceNameLength);
            VideoEvent event;
            event.type = VideoEvent::DeviceName;
            event.name = QString::fromUtf8(name.left(name.indexOf('\0')));
            events.append(event);
            pos += kDeviceNameLength;
            m_state = CodecHeader;
        } else if (m_state == CodecHeader) {
            if (available() < 4) {
                break;
            }
            const quint32 id = u32(m_buffer, pos);
            pos += 4;
            const Codec codec = id == 0x68323634 ? Codec::H264 : id == 0x68323635 ? Codec::H265 : id == 0x00617631 ? Codec::AV1 : Codec::Unknown;
            if (codec == Codec::Unknown) {
                fail(id <= 1 ? QStringLiteral("The phone could not capture video") : QStringLiteral("Unknown video codec"));
                break;
            }
            VideoEvent event;
            event.type = VideoEvent::CodecId;
            event.codec = codec;
            events.append(event);
            m_state = FrameHeader;
        } else if (m_state == FrameHeader) {
            if (available() < 12) {
                break;
            }
            const quint64 head = (quint64(u32(m_buffer, pos)) << 32) | u32(m_buffer, pos + 4);
            const quint32 size = u32(m_buffer, pos + 8);
            pos += 12;
            if (head & (1ULL << 63)) { // session packet: the new frame size follows
                VideoEvent event;
                event.type = VideoEvent::Size;
                event.size = QSize(int(head & 0xffffffff), int(size));
                events.append(event);
            } else if (size > kMaxFrameSize) {
                fail(QStringLiteral("Video frame too large"));
            } else {
                m_frame = VideoEvent();
                m_frame.config = head & (1ULL << 62);
                m_frame.pts = qint64(head & ((1ULL << 61) - 1));
                m_frameSize = size;
                m_state = FrameBody;
            }
        } else {
            if (available() < m_frameSize) {
                break;
            }
            m_frame.data = m_buffer.mid(pos, m_frameSize);
            pos += m_frameSize;
            events.append(std::move(m_frame));
            m_state = FrameHeader;
        }
    }
    m_buffer.remove(0, pos);
    return events;
}

QByteArray injectKeycode(quint8 action, int keycode, int metaState)
{
    return message(0, [&](QDataStream &out) {
        out << action << qint32(keycode) << qint32(0) << qint32(metaState);
    });
}

QByteArray injectText(const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    qsizetype length = std::min<qsizetype>(utf8.size(), 300);
    while (length > 0 && length < utf8.size() && (quint8(utf8[length]) & 0xC0) == 0x80) {
        --length;
    }
    return message(1, [&](QDataStream &out) {
        writeString(out, utf8.left(length));
    });
}

QByteArray injectTouch(quint8 action, QPoint position, QSize frameSize)
{
    const bool pressed = action != Android::ActionUp;
    return message(2, [&](QDataStream &out) {
        out << action << quint64(-2); // pointer id: generic finger
        writePosition(out, position, frameSize);
        out << quint16(pressed ? 0xffff : 0) << qint32(0) << qint32(pressed ? 1 : 0);
    });
}

QByteArray injectScroll(QPoint position, QSize frameSize, float h, float v)
{
    auto fixed = [](float notches) {
        return qint16(std::clamp(notches / 16.0f, -1.0f, 1.0f) * 0x7fff);
    };
    return message(3, [&](QDataStream &out) {
        writePosition(out, position, frameSize);
        out << fixed(h) << fixed(v) << qint32(0);
    });
}

QByteArray backOrScreenOn(quint8 action)
{
    return message(4, [&](QDataStream &out) {
        out << action;
    });
}

QByteArray expandNotificationPanel()
{
    return message(5);
}

QByteArray collapsePanels()
{
    return message(7);
}

QByteArray pasteText(const QString &text)
{
    return message(9, [&](QDataStream &out) {
        out << quint64(0) << quint8(true); // sequence 0: no acknowledgement
        writeString(out, text.toUtf8());
    });
}

QByteArray setScreenPower(bool on)
{
    return message(10, [&](QDataStream &out) {
        out << quint8(on);
    });
}

QByteArray resetVideo()
{
    return message(17);
}

QByteArray resizeDisplay(QSize size)
{
    return message(21, [&](QDataStream &out) {
        out << quint16(size.width()) << quint16(size.height());
    });
}
}
