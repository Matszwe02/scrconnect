/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QString>

#include <core/kdeconnectplugin.h>

class QProcess;
class StreamRelay;

/**
 * The phone's cameras as webcams. The daemon only relays: the helper process kdeconnect-webcam owns the virtual cameras,
 * decodes the video and asks for a stream when an app opens a camera.
 */
class WebcamPlugin : public KdeConnectPlugin
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.device.webcam")
    Q_PROPERTY(bool virtualCamerasReady READ virtualCamerasReady NOTIFY virtualCamerasChanged)

public:
    WebcamPlugin(QObject *parent, const QVariantList &args);
    ~WebcamPlugin() override;

    void receivePacket(const NetworkPacket &np) override;
    QString dbusPath() const override;

    /** The helper reads the stream from the local socket "kdeconnect-webcam-<id>". */
    Q_SCRIPTABLE QString startStream(const QString &cameraId, int width, int height, const QString &codec);
    Q_SCRIPTABLE void stopStream(const QString &streamId);
    bool virtualCamerasReady() const;
    /** One-time setup of the v4l2loopback cameras, as root. */
    Q_SCRIPTABLE void setupVirtualCameras();

Q_SIGNALS:
    Q_SCRIPTABLE void virtualCamerasChanged();
    Q_SCRIPTABLE void statusChanged(const QString &streamId, const QString &state, const QString &message);

private:
    void startHelper();
    void stopHelper();
    void destroyStream(const QString &id, bool notifyPhone);

    QHash<QString, StreamRelay *> m_streams;
    QJsonArray m_cameras;
    QProcess *m_helper = nullptr;
    bool m_restartPending = false;
    int m_crashes = 0;
    QString m_lastError;
};
