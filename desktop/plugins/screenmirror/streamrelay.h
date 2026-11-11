/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QIODevice>
#include <QLocalServer>
#include <QPointer>
#include <QSharedPointer>

class QLocalSocket;

class StreamRelay : public QObject
{
    Q_OBJECT
public:
    StreamRelay(const QString &name, QObject *parent);
    ~StreamRelay() override;

    static QString clientProgram(const QString &name);

    bool isListening() const
    {
        return m_server.isListening();
    }
    void setSource(const QSharedPointer<QIODevice> &source);

Q_SIGNALS:
    void clientConnected(QLocalSocket *client);
    void closed();

private:
    void relay();

    QLocalServer m_server;
    QPointer<QLocalSocket> m_client;
    QSharedPointer<QIODevice> m_source;
    QByteArray m_pending;
};
