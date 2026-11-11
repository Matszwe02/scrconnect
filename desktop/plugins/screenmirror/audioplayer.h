/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
#pragma once

#include <QIODevice>
#include <QObject>
#include <QSharedPointer>

class QAudioSink;

// Late packets are dropped rather than queued, to stay in sync with the picture
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    AudioPlayer(const QSharedPointer<QIODevice> &stream, QObject *parent);
    ~AudioPlayer() override;

private:
    void read();

    QSharedPointer<QIODevice> m_stream;
    QByteArray m_buffer;
    bool m_started = false;
    QAudioSink *m_sink = nullptr;
    QIODevice *m_output = nullptr;
};
