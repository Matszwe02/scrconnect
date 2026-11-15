/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
#include "audioplayer.h"

#include "streamrelay.h"

#include <QTimer>
#include <QtEndian>

#include <utility>

using namespace Qt::StringLiterals;

static constexpr qint64 kMaxPending = 1 << 18; // about 1.4 s of audio waiting for the helper

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
    if (m_helper) {
        m_helper->disconnect(this);
        m_helper->closeWriteChannel(); // it quits at the end of its input
        if (!m_helper->waitForFinished(1000)) {
            m_helper->kill();
            m_helper->waitForFinished(1000);
        }
    }
}

void AudioPlayer::startHelper()
{
    const QString program = StreamRelay::clientProgram(u"kdeconnect-audio"_s);
    if (program.isEmpty()) {
        return;
    }
    delete std::exchange(m_helper, nullptr);
    m_helper = new QProcess(this);
    m_helper->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(m_helper, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if ((status == QProcess::CrashExit || code != 0) && m_raw && ++m_crashes <= 5) {
            QTimer::singleShot(1000, this, &AudioPlayer::startHelper);
        }
        m_helper->deleteLater();
        m_helper = nullptr;
    });
    m_helper->start(program, {});
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
            m_raw = true;
            startHelper();
        }
        m_buffer.remove(0, 4);
    }
    while (m_buffer.size() >= 12) {
        const qsizetype size = qFromBigEndian<quint32>(m_buffer.constData() + 8);
        if (size > (1 << 20)) {
            m_raw = false; // not a stream this knows
        }
        if (!m_raw) {
            m_buffer.clear();
            return;
        }
        if (m_buffer.size() < 12 + size) {
            return;
        }
        // Without a running helper (it crashed and is restarting) the audio is dropped, which keeps frames whole
        if (m_helper && m_helper->state() == QProcess::Running && m_helper->bytesToWrite() < kMaxPending) {
            m_helper->write(m_buffer.constData() + 12, size);
        }
        m_buffer.remove(0, 12 + size);
    }
}

#include "moc_audioplayer.cpp"
