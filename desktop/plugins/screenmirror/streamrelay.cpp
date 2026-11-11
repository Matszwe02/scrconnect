/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "streamrelay.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLocalSocket>
#include <QStandardPaths>

namespace
{
constexpr qsizetype kMaxPending = 64 * 1024 * 1024;
}

StreamRelay::StreamRelay(const QString &name, QObject *parent)
    : QObject(parent)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.listen(name);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        m_client = m_server.nextPendingConnection();
        m_server.close();
        connect(m_client, &QLocalSocket::disconnected, this, &StreamRelay::closed, Qt::QueuedConnection);
        m_client->write(std::exchange(m_pending, {}));
        Q_EMIT clientConnected(m_client);
    });
}

QString StreamRelay::clientProgram(const QString &name)
{
    const QString next = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
    return QFileInfo::exists(next) ? next : QStandardPaths::findExecutable(name);
}

StreamRelay::~StreamRelay()
{
    if (m_source) {
        m_source->disconnect(this);
        m_source->close();
    }
    if (m_client) {
        m_client->disconnect(this);
        m_client->disconnectFromServer();
    }
}

void StreamRelay::setSource(const QSharedPointer<QIODevice> &source)
{
    m_source = source;
    connect(source.data(), &QIODevice::readyRead, this, &StreamRelay::relay);
    connect(source.data(), &QIODevice::readChannelFinished, this, [this] {
        relay();
        if (m_client) {
            m_client->disconnectFromServer();
        }
    });
    relay();
}

void StreamRelay::relay()
{
    const QByteArray data = m_source->readAll();
    if (m_client) {
        m_client->write(data);
    } else if ((m_pending += data).size() > kMaxPending) {
        m_pending.clear();
        m_source->disconnect(this);
        QMetaObject::invokeMethod(this, &StreamRelay::closed, Qt::QueuedConnection);
    }
}

#include "moc_streamrelay.cpp"
