/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "screenmirrorplugin.h"

#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QUuid>

#include <KPluginFactory>

#include <core/device.h>

#include "audioplayer.h"
#include "plugin_screenmirror_debug.h"
#include "streamrelay.h"

using namespace Qt::StringLiterals;

#define PACKET_TYPE_MIRROR_APPS_REQUEST u"kdeconnect.mirror.apps.request"_s
#define PACKET_TYPE_MIRROR_REQUEST u"kdeconnect.mirror.request"_s
#define PACKET_TYPE_MIRROR_CONTROL u"kdeconnect.mirror.control"_s
#define PACKET_TYPE_MIRROR_STOP u"kdeconnect.mirror.stop"_s
#define PACKET_TYPE_MIRROR_OPEN u"kdeconnect.mirror.open"_s
#define PACKET_TYPE_MIRROR_HANDOFF_RETURN u"kdeconnect.mirror.handoff.return"_s
#define PACKET_TYPE_MIRROR_STATUS u"kdeconnect.mirror.status"_s
#define PACKET_TYPE_MIRROR_APPS u"kdeconnect.mirror.apps"_s
#define PACKET_TYPE_MIRROR_ICONS u"kdeconnect.mirror.icons"_s
#define PACKET_TYPE_MIRROR_VIDEO u"kdeconnect.mirror.video"_s
#define PACKET_TYPE_MIRROR_AUDIO u"kdeconnect.mirror.audio"_s

K_PLUGIN_CLASS_WITH_JSON(ScreenMirrorPlugin, "kdeconnect_screenmirror.json")

namespace
{
constexpr qsizetype kMaxControlChunk = 32 * 1024;
}

ScreenMirrorPlugin::~ScreenMirrorPlugin()
{
    const QStringList ids = m_sessions.keys();
    for (const QString &id : ids) {
        destroySession(id, true);
    }
}

QString ScreenMirrorPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/screenmirror").arg(device()->id());
}

bool ScreenMirrorPlugin::launchViewer(const QStringList &arguments)
{
    const QString program = StreamRelay::clientProgram(u"kdeconnect-mirror"_s);
    if (program.isEmpty() || !QProcess::startDetached(program, QStringList{u"--device"_s, device()->id()} + arguments)) {
        qCWarning(KDECONNECT_PLUGIN_SCREENMIRROR) << "Cannot start the kdeconnect-mirror viewer";
        return false;
    }
    return true;
}

void ScreenMirrorPlugin::openViewer(bool wholeScreen)
{
    launchViewer(wholeScreen ? QStringList{u"--screen"_s} : QStringList());
}

void ScreenMirrorPlugin::requestApps(const QStringList &iconsFor)
{
    NetworkPacket np(PACKET_TYPE_MIRROR_APPS_REQUEST);
    if (!iconsFor.isEmpty()) {
        np.set(u"icons"_s, iconsFor);
    }
    sendPacket(np);
}

bool ScreenMirrorPlugin::screenOffDefault(const QString &mode) const
{
    return config()->getBool(mode == "app"_L1 ? u"screenOffApps"_s : u"screenOffScreen"_s, false);
}

QString ScreenMirrorPlugin::startSession(const QString &mode, const QString &packageName, const QVariantMap &options)
{
    // Unguessable: the socket name gives access to a live view of the phone
    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    auto *relay = new StreamRelay(u"kdeconnect-mirror-"_s + id, this);
    if (!relay->isListening()) {
        delete relay;
        return {};
    }
    connect(relay, &StreamRelay::clientConnected, this, [this, id](QLocalSocket *viewer) {
        viewerConnected(id, viewer);
    });
    connect(relay, &StreamRelay::closed, this, [this, id] {
        destroySession(id, true);
    });
    m_sessions.insert(id, {relay});

    NetworkPacket np(PACKET_TYPE_MIRROR_REQUEST, {{u"sessionId"_s, id}, {u"mode"_s, mode}, {u"package"_s, packageName}});
    for (const QString &key : {u"width"_s, u"height"_s}) {
        np.set(key, qMax(0, options.value(key).toInt()));
    }
    np.set(u"screenOff"_s, screenOffDefault(mode));
    np.set(u"lockOnClose"_s, config()->getBool(u"lockOnClose"_s, false));
    np.set(u"audio"_s, config()->getBool(u"audio"_s, true));
    if (const QString codec = options.value(u"codec"_s).toString(); !codec.isEmpty()) {
        config()->set(u"codec"_s, codec);
    }
    np.set(u"codec"_s, config()->getString(u"codec"_s, u"h264"_s));
    for (const QString &key : {u"notificationId"_s, u"handoff"_s, u"handoffTaskId"_s}) {
        if (options.contains(key)) {
            np.set(key, options.value(key));
        }
    }
    if (!sendPacket(np)) {
        destroySession(id, false);
        return {};
    }
    return id;
}

void ScreenMirrorPlugin::openNotification(const QString &notificationId, const QString &packageName)
{
    if (const QString open = m_notificationWindows.value(packageName); m_sessions.contains(open)) {
        NetworkPacket np(PACKET_TYPE_MIRROR_OPEN, {{u"sessionId"_s, open}, {u"notificationId"_s, notificationId}, {u"package"_s, packageName}});
        sendPacket(np);
        return;
    }
    const QString id = startSession(u"app"_s, packageName, {{u"notificationId"_s, notificationId}});
    if (!id.isEmpty() && launchViewer({u"--attach"_s, id, u"--app"_s, packageName})) {
        m_notificationWindows.insert(packageName, id);
    } else {
        destroySession(id, true);
    }
}

void ScreenMirrorPlugin::openHandoffWindow(const QString &packageName, const QString &label, int taskId, int x, int y)
{
    const QString id = startSession(u"app"_s, packageName, {{u"handoff"_s, true}, {u"handoffTaskId"_s, taskId}});
    if (!id.isEmpty() && !launchViewer({u"--attach"_s, id, u"--app"_s, packageName, u"--label"_s, label, u"--handoff"_s, u"%1,%2"_s.arg(x).arg(y)})) {
        destroySession(id, true);
    }
}

void ScreenMirrorPlugin::returnToPhone(const QString &sessionId)
{
    if (m_sessions.contains(sessionId)) {
        NetworkPacket np(PACKET_TYPE_MIRROR_HANDOFF_RETURN, {{u"sessionId"_s, sessionId}});
        sendPacket(np);
        destroySession(sessionId, false);
    }
}

int ScreenMirrorPlugin::phoneEdge() const
{
    const KdeConnectPlugin *share = device()->plugin(u"kdeconnect_shareinputdevices"_s);
    return share ? share->config()->getInt(u"edge"_s, Qt::LeftEdge) : 0;
}

void ScreenMirrorPlugin::stopSession(const QString &sessionId)
{
    destroySession(sessionId, true);
}

void ScreenMirrorPlugin::destroySession(const QString &id, bool notifyPhone)
{
    const Session s = m_sessions.take(id);
    if (!s.video) {
        return;
    }
    m_notificationWindows.removeIf([&id](const auto &it) {
        return it.value() == id;
    });
    if (notifyPhone) {
        NetworkPacket np(PACKET_TYPE_MIRROR_STOP, {{u"sessionId"_s, id}});
        sendPacket(np);
    }
    delete s.audio;
    s.video->disconnect(this);
    s.video->deleteLater(); // this may run inside one of its signals
}

void ScreenMirrorPlugin::receivePacket(const NetworkPacket &np)
{
    const QString id = np.get<QString>(u"sessionId"_s);
    const auto s = m_sessions.find(id);
    if (np.type() == PACKET_TYPE_MIRROR_STATUS) {
        const QString state = np.get<QString>(u"state"_s);
        Q_EMIT statusChanged(id, state, np.get<QString>(u"message"_s));
        if (s != m_sessions.end() && !state.startsWith("screen_"_L1)) {
            destroySession(id, false);
        }
    } else if (np.type() == PACKET_TYPE_MIRROR_APPS) {
        const QVariantList apps = np.get<QVariantList>(u"apps"_s);
        Q_EMIT appsReceived(QString::fromUtf8(QJsonDocument::fromVariant(apps).toJson(QJsonDocument::Compact)));
    } else if (np.type() == PACKET_TYPE_MIRROR_ICONS) {
        const QVariantList icons = np.get<QVariantList>(u"icons"_s);
        Q_EMIT iconsReceived(QString::fromUtf8(QJsonDocument::fromVariant(icons).toJson(QJsonDocument::Compact)));
    } else if (s == m_sessions.end() || !np.payload()) {
        return;
    } else if (np.type() == PACKET_TYPE_MIRROR_VIDEO) {
        s->video->setSource(np.payload());
    } else if (np.type() == PACKET_TYPE_MIRROR_AUDIO) {
        if (!s->audio) {
            s->audio = new AudioPlayer(np.payload(), this);
        }
    }
}

void ScreenMirrorPlugin::viewerConnected(const QString &id, QLocalSocket *socket)
{
    auto relayControl = [this, id, socket] {
        // Packets, not a payload: a payload ends as soon as its source has nothing to read
        const QByteArray bytes = socket->readAll();
        for (qsizetype offset = 0; offset < bytes.size(); offset += kMaxControlChunk) {
            NetworkPacket np(PACKET_TYPE_MIRROR_CONTROL, {{u"sessionId"_s, id}, {u"data"_s, QString::fromLatin1(bytes.mid(offset, kMaxControlChunk).toBase64())}});
            sendPacket(np);
        }
    };
    connect(socket, &QLocalSocket::readyRead, this, relayControl);
    relayControl(); // sent before the connection above existed
}

#include "moc_screenmirrorplugin.cpp"
#include "screenmirrorplugin.moc"
