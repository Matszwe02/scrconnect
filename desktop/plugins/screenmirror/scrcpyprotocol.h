/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>

// The scrcpy 5 wire protocol (Apache-2.0, Genymobile), big endian
namespace Scrcpy
{
enum class Codec {
    Unknown,
    H264,
    H265,
    AV1,
};

struct VideoEvent {
    enum Type {
        DeviceName,
        CodecId,
        Size,
        Frame,
    };
    Type type = Frame;
    QString name;
    Codec codec = Codec::Unknown;
    QSize size;
    QByteArray data;
    bool config = false;
    qint64 pts = 0;
};

class VideoParser
{
public:
    QList<VideoEvent> push(const QByteArray &data);
    QString error() const
    {
        return m_error;
    }

private:
    enum State {
        Name,
        CodecHeader,
        FrameHeader,
        FrameBody,
        Failed,
    };
    State m_state = Name;
    QByteArray m_buffer;
    VideoEvent m_frame;
    qsizetype m_frameSize = 0;
    QString m_error;
};

namespace Android
{
constexpr quint8 ActionDown = 0;
constexpr quint8 ActionUp = 1;
constexpr quint8 ActionMove = 2;
constexpr int KeycodeHome = 3;
constexpr int KeycodeBack = 4;
constexpr int KeycodeAppSwitch = 187;
}

QByteArray injectKeycode(quint8 action, int keycode, int metaState = 0);
QByteArray injectText(const QString &text);
QByteArray injectTouch(quint8 action, QPoint position, QSize frameSize);
QByteArray injectScroll(QPoint position, QSize frameSize, float h, float v);
QByteArray backOrScreenOn(quint8 action);
QByteArray expandNotificationPanel();
QByteArray collapsePanels();
QByteArray pasteText(const QString &text);
QByteArray setScreenPower(bool on);
QByteArray resetVideo();
QByteArray resizeDisplay(QSize size);
}
