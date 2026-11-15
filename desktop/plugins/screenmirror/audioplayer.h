/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QObject>
#include <QProcess>
#include <QSharedPointer>

/**
 * Unpacks the phone's audio stream and hands the PCM to the helper process kdeconnect-audio, which plays it. The helper is
 * separate because the audio backend can crash when an output device goes away; the player then just starts it again.
 * Late packets are dropped rather than queued, to stay in sync with the picture.
 */
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    AudioPlayer(const QSharedPointer<QIODevice> &stream, QObject *parent);
    ~AudioPlayer() override;

private:
    void read();
    void startHelper();

    QSharedPointer<QIODevice> m_stream;
    QByteArray m_buffer;
    bool m_started = false;
    bool m_raw = false;
    int m_crashes = 0;
    QProcess *m_helper = nullptr;
};
