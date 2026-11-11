/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
#include "audioplayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QtEndian>

AudioPlayer::AudioPlayer(const QSharedPointer<QIODevice> &stream, QObject *parent)
    : QObject(parent)
    , m_stream(stream)
{
    connect(stream.data(), &QIODevice::readyRead, this, &AudioPlayer::read);
    read();
}

AudioPlayer::~AudioPlayer()
{
    m_stream->disconnect(this);
    m_stream->close();
}

void AudioPlayer::read()
{
    m_buffer += m_stream->readAll();
    if (!m_started) {
        if (m_buffer.size() < 4) {
            return;
        }
        m_started = true;
        if (m_buffer.left(4) == QByteArray("\0raw", 4)) {
            QAudioFormat format;
            format.setSampleRate(48000);
            format.setChannelCount(2);
            format.setSampleFormat(QAudioFormat::Int16);
            m_sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
            m_sink->setBufferSize(48000 * 4 / 8);
            m_output = m_sink->start();
        }
        m_buffer.remove(0, 4);
    }
    while (m_buffer.size() >= 12) {
        const qsizetype size = qFromBigEndian<quint32>(m_buffer.constData() + 8);
        if (size > (1 << 20)) {
            m_output = nullptr;
        }
        if (!m_output) {
            m_buffer.clear();
            return;
        }
        if (m_buffer.size() < 12 + size) {
            return;
        }
        if (size <= m_sink->bytesFree()) {
            m_output->write(m_buffer.constData() + 12, size);
        }
        m_buffer.remove(0, 12 + size);
    }
}

#include "moc_audioplayer.cpp"
