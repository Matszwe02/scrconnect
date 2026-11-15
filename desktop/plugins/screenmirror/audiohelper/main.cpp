/*
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// Plays raw 48 kHz stereo 16-bit PCM read from stdin. It is a process of its own because the audio backend can crash when
// an output device disappears (bluetooth headphones disconnecting), and that must not take the daemon down with it.

#include <QAudioFormat>
#include <QAudioSink>
#include <QCoreApplication>
#include <QMediaDevices>
#include <QSocketNotifier>
#include <QTimer>

#include <unistd.h>

class Player : public QObject
{
public:
    Player()
        : m_notifier(STDIN_FILENO, QSocketNotifier::Read)
    {
        connect(&m_notifier, &QSocketNotifier::activated, this, &Player::readInput);
        connect(&m_devices, &QMediaDevices::audioOutputsChanged, this, &Player::openSink);
        m_retry.setSingleShot(true);
        m_retry.setInterval(1000);
        connect(&m_retry, &QTimer::timeout, this, &Player::openSink);
        openSink();
    }

private:
    void closeSink()
    {
        m_output = nullptr;
        if (m_sink) {
            m_sink->disconnect(this);
            m_sink->stop();
            delete std::exchange(m_sink, nullptr);
        }
    }

    void openSink()
    {
        closeSink();
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) {
            return;
        }
        QAudioFormat format;
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        m_sink = new QAudioSink(device, format, this);
        m_sink->setBufferSize(48000 * 4 / 8);
        connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
            if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
                m_output = nullptr;
                m_retry.start();
            }
        });
        m_output = m_sink->start();
    }

    void readInput()
    {
        char buffer[1 << 16];
        const ssize_t read = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (read <= 0) {
            QCoreApplication::quit(); // the daemon closed the pipe or is gone
            return;
        }
        m_pending.append(buffer, read);
        const qsizetype size = m_pending.size() & ~qsizetype(3); // whole stereo samples only, or the channels swap
        // Late audio is dropped rather than queued, to stay in sync with the picture
        if (m_output && size <= m_sink->bytesFree()) {
            m_output->write(m_pending.constData(), size);
        }
        m_pending.remove(0, size);
    }

    QSocketNotifier m_notifier;
    QMediaDevices m_devices;
    QTimer m_retry;
    QByteArray m_pending;
    QAudioSink *m_sink = nullptr;
    QIODevice *m_output = nullptr;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    Player player;
    return app.exec();
}
