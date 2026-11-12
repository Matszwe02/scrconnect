/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QHash>
#include <QVariantMap>

#include <core/kdeconnectplugin.h>

class AudioPlayer;
class QLocalSocket;
class StreamRelay;

/**
 * Relays each session's video from the phone to a kdeconnect-mirror viewer over a local socket, and the viewer's control
 * messages back to the phone. The phone's sound is played here.
 */
class ScreenMirrorPlugin : public KdeConnectPlugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.screenmirror")

public:
    using KdeConnectPlugin::KdeConnectPlugin;
    ~ScreenMirrorPlugin() override;

    void receivePacket(const NetworkPacket &np) override;
    QString dbusPath() const override;

    /**
     * mode: "screen" or "app" (packageName required). options: width, height, codec, notificationId, handoff, handoffTaskId.
     * Returns the session id, empty on failure; the viewer then connects to the local socket "kdeconnect-mirror-<id>".
     */
    Q_SCRIPTABLE QString startSession(const QString &mode, const QString &packageName, const QVariantMap &options);
    Q_SCRIPTABLE void stopSession(const QString &sessionId);
    Q_SCRIPTABLE bool screenOffDefault(const QString &mode) const;
    Q_SCRIPTABLE void requestApps(const QStringList &iconsFor);
    Q_SCRIPTABLE void openViewer(bool wholeScreen);

    Q_SCRIPTABLE void openNotification(const QString &notificationId, const QString &packageName);
    /** An app dragged out of the phone with Phone Leap: opens it in a window that follows the pointer. */
    Q_INVOKABLE void openHandoffWindow(const QString &packageName, const QString &label, int taskId, int x, int y);
    Q_SCRIPTABLE void returnToPhone(const QString &sessionId);
    /** The screen edge (Qt::Edge) behind which Phone Leap puts the phone, 0 if that plugin is off. */
    Q_SCRIPTABLE int phoneEdge() const;

Q_SIGNALS:
    Q_SCRIPTABLE void statusChanged(const QString &sessionId, const QString &state, const QString &message);
    Q_SCRIPTABLE void appsReceived(const QString &json);
    Q_SCRIPTABLE void iconsReceived(const QString &json);

private:
    struct Session {
        StreamRelay *video = nullptr;
        AudioPlayer *audio = nullptr;
    };

    void viewerConnected(const QString &id, QLocalSocket *viewer);
    void destroySession(const QString &id, bool notifyPhone);
    bool launchViewer(const QStringList &arguments);

    QHash<QString, Session> m_sessions;
    QHash<QString, QString> m_notificationWindows;
};
