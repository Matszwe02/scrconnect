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

struct Position {
    QPoint point;
    QSize frame;
};

QDataStream &operator<<(QDataStream &out, const Position &p)
{
    return out << qint32(p.point.x()) << qint32(p.point.y()) << quint16(p.frame.width()) << quint16(p.frame.height());
}

struct Utf8 { // a length, then the bytes
    QByteArray bytes;
};

QDataStream &operator<<(QDataStream &out, const Utf8 &s)
{
    out << quint32(s.bytes.size());
    out.writeRawData(s.bytes.constData(), int(s.bytes.size()));
    return out;
}

template<typename... Fields>
QByteArray message(quint8 type, const Fields &...fields)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    ((out << type) << ... << fields);
    return bytes;
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
    auto add = [&events](VideoEvent::Type type) -> VideoEvent & {
        events.emplaceBack().type = type;
        return events.last();
    };
    while (m_state != Failed) {
        if (m_state == Name) {
            if (available() < kDeviceNameLength) {
                break;
            }
            const QByteArray name = m_buffer.mid(pos, kDeviceNameLength);
            add(VideoEvent::DeviceName).name = QString::fromUtf8(name.left(name.indexOf('\0')));
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
            add(VideoEvent::CodecId).codec = codec;
            m_state = FrameHeader;
        } else if (m_state == FrameHeader) {
            if (available() < 12) {
                break;
            }
            const quint64 head = (quint64(u32(m_buffer, pos)) << 32) | u32(m_buffer, pos + 4);
            const quint32 size = u32(m_buffer, pos + 8);
            pos += 12;
            if (head & (1ULL << 63)) { // session packet: the new frame size follows
                add(VideoEvent::Size).size = QSize(int(head & 0xffffffff), int(size));
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
    return message(0, action, qint32(keycode), qint32(0), qint32(metaState));
}

QByteArray injectText(const QString &text)
{
    QByteArray utf8 = text.toUtf8();
    qsizetype length = std::min<qsizetype>(utf8.size(), 300);
    while (length > 0 && length < utf8.size() && (quint8(utf8[length]) & 0xC0) == 0x80) {
        --length;
    }
    return message(1, Utf8{utf8.left(length)});
}

QByteArray injectTouch(quint8 action, QPoint position, QSize frameSize)
{
    const bool pressed = action != Android::ActionUp;
    // pointer id: generic finger
    return message(2, action, quint64(-2), Position{position, frameSize}, quint16(pressed ? 0xffff : 0), qint32(0), qint32(pressed ? 1 : 0));
}

QByteArray injectScroll(QPoint position, QSize frameSize, float h, float v)
{
    auto fixed = [](float notches) {
        return qint16(std::clamp(notches / 16.0f, -1.0f, 1.0f) * 0x7fff);
    };
    return message(3, Position{position, frameSize}, fixed(h), fixed(v), qint32(0));
}

QByteArray backOrScreenOn(quint8 action)
{
    return message(4, action);
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
    return message(9, quint64(0), quint8(true), Utf8{text.toUtf8()}); // sequence 0: no acknowledgement
}

QByteArray setScreenPower(bool on)
{
    return message(10, quint8(on));
}

QByteArray resetVideo()
{
    return message(17);
}

QByteArray resizeDisplay(QSize size)
{
    return message(21, quint16(size.width()), quint16(size.height()));
}
}
