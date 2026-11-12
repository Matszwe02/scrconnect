/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "webcamplugin.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSize>
#include <QTimer>
#include <QUuid>

#include <KLocalizedString>
#include <KPluginFactory>

#include <core/daemon.h>
#include <core/device.h>

#include "../screenmirror/streamrelay.h"

using namespace Qt::StringLiterals;

#define PACKET_TYPE_WEBCAM_CAMERAS_REQUEST u"kdeconnect.webcam.cameras.request"_s
#define PACKET_TYPE_WEBCAM_START u"kdeconnect.webcam.start"_s
#define PACKET_TYPE_WEBCAM_STOP u"kdeconnect.webcam.stop"_s
#define PACKET_TYPE_WEBCAM_CAMERAS u"kdeconnect.webcam.cameras"_s
#define PACKET_TYPE_WEBCAM_VIDEO u"kdeconnect.webcam.video"_s
#define PACKET_TYPE_WEBCAM_STATUS u"kdeconnect.webcam.status"_s

K_PLUGIN_CLASS_WITH_JSON(WebcamPlugin, "kdeconnect_webcam.json")

namespace
{
QString flatpakSetupCommand()
{
    return u"flatpak run --command=/app/run.sh %1 kdeconnect-webcam --setup-script | sudo sh"_s.arg(qEnvironmentVariable("FLATPAK_ID"));
}
}

WebcamPlugin::WebcamPlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
    connect(config(), &KdeConnectPluginConfig::configChanged, this, [this] {
        m_streams.isEmpty() ? startHelper() : void(m_restartPending = true);
    });
    NetworkPacket np(PACKET_TYPE_WEBCAM_CAMERAS_REQUEST);
    sendPacket(np);
}

WebcamPlugin::~WebcamPlugin()
{
    const QStringList ids = m_streams.keys();
    for (const QString &id : ids) {
        destroyStream(id, true);
    }
    stopHelper();
}

QString WebcamPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/webcam").arg(device()->id());
}

bool WebcamPlugin::virtualCamerasReady() const
{
    const QDir sys(u"/sys/class/video4linux"_s);
    for (const QString &entry : sys.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile name(sys.filePath(entry + u"/name"_s));
        if (name.open(QIODevice::ReadOnly) && name.readAll().trimmed() == "Phone front camera") {
            return true;
        }
    }
    return false;
}

void WebcamPlugin::setupVirtualCameras()
{
    auto *setup = new QProcess(this);
    connect(setup, &QProcess::finished, this, [this, setup](int code, QProcess::ExitStatus status) {
        setup->deleteLater();
        const bool ok = status == QProcess::NormalExit && code == 0;
        const QString failure = i18n(
            "The virtual cameras could not be set up. They need the v4l2loopback kernel module (package v4l2loopback-dkms, v4l2loopback or "
            "linux-modules-extra; with Secure Boot it must be signed) and administrator rights.");
        Daemon::instance()->sendSimpleNotification(u"notification"_s,
                                                   i18n("Camera sharing"),
                                                   ok ? i18n("The virtual cameras are ready.")
                                                      : QFile::exists(u"/.flatpak-info"_s)
                                                      ? failure + u' ' + i18n("Or run in a terminal: %1", flatpakSetupCommand())
                                                      : failure + u' ' + i18n("Without a password dialog, run \"sudo kdeconnect-webcam --setup\"."),
                                                   u"camera-web"_s);
        Q_EMIT virtualCamerasChanged();
        startHelper();
    });
    stopHelper();
    setup->start(StreamRelay::clientProgram(u"kdeconnect-webcam"_s), {u"--setup"_s});
}

void WebcamPlugin::stopHelper()
{
    if (m_helper) {
        m_helper->disconnect(this);
        m_helper->terminate();
        if (!m_helper->waitForFinished(2000)) {
            m_helper->kill();
        }
        delete std::exchange(m_helper, nullptr);
    }
}

void WebcamPlugin::startHelper()
{
    stopHelper();
    m_restartPending = false;
    // One virtual camera for the first front and the first back camera, in the size closest to 720p
    QJsonArray cameras;
    for (const QString &facing : {u"front"_s, u"back"_s}) {
        for (const QJsonValue &value : std::as_const(m_cameras)) {
            const QJsonObject camera = value.toObject();
            if (camera.value("facing"_L1).toString() != facing) {
                continue;
            }
            QSize best(1280, 720);
            int distance = INT_MAX;
            for (const QJsonValue &size : camera.value("sizes"_L1).toArray()) {
                const QSize s(size[0].toInt(), size[1].toInt());
                if (const int d = std::abs(s.width() - 1280) + std::abs(s.height() - 720); d < distance) {
                    distance = d;
                    best = s;
                }
            }
            cameras.append(QJsonObject{
                {u"id"_s, camera.value("id"_L1)},
                {u"device"_s, u"Phone %1 camera"_s.arg(facing)},
                {u"label"_s, i18n("%1 %2 camera", device()->name(), facing)},
                {u"width"_s, best.width()},
                {u"height"_s, best.height()},
                {u"rotation"_s, config()->getInt(facing + u"Rotation"_s, 0)},
                {u"mirror"_s, config()->getBool(facing + u"Mirror"_s, false)},
            });
            break;
        }
    }
    const QString program = StreamRelay::clientProgram(u"kdeconnect-webcam"_s);
    if (cameras.isEmpty() || program.isEmpty()) {
        return;
    }
    m_helper = new QProcess(this);
    m_helper->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(m_helper, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if ((status == QProcess::CrashExit || code != 0) && ++m_crashes <= 5) {
            QTimer::singleShot(3000, this, &WebcamPlugin::startHelper);
        }
    });
    m_helper->start(program,
                    {u"--device"_s,
                     device()->id(),
                     u"--backend"_s,
                     config()->getString(u"backend"_s, u"auto"_s),
                     u"--cameras"_s,
                     QString::fromUtf8(QJsonDocument(cameras).toJson(QJsonDocument::Compact))});
}

QString WebcamPlugin::startStream(const QString &cameraId, int width, int height, const QString &codec)
{
    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    auto *relay = new StreamRelay(u"kdeconnect-webcam-"_s + id, this);
    if (!relay->isListening()) {
        delete relay;
        return {};
    }
    connect(relay, &StreamRelay::closed, this, [this, id] {
        destroyStream(id, true);
    });
    m_streams.insert(id, relay);
    NetworkPacket np(PACKET_TYPE_WEBCAM_START,
                     {{u"streamId"_s, id}, {u"cameraId"_s, cameraId}, {u"width"_s, width}, {u"height"_s, height}, {u"codec"_s, codec}});
    if (!sendPacket(np)) {
        destroyStream(id, false);
        return {};
    }
    return id;
}

void WebcamPlugin::stopStream(const QString &streamId)
{
    destroyStream(streamId, true);
}

void WebcamPlugin::destroyStream(const QString &id, bool notifyPhone)
{
    StreamRelay *relay = m_streams.take(id);
    if (!relay) {
        return;
    }
    if (notifyPhone) {
        NetworkPacket np(PACKET_TYPE_WEBCAM_STOP, {{u"streamId"_s, id}});
        sendPacket(np);
    }
    relay->disconnect(this);
    relay->deleteLater(); // this may run inside one of its signals
    if (m_streams.isEmpty() && m_restartPending) {
        QTimer::singleShot(0, this, &WebcamPlugin::startHelper);
    }
}

void WebcamPlugin::receivePacket(const NetworkPacket &np)
{
    const QString id = np.get<QString>(u"streamId"_s);
    if (np.type() == PACKET_TYPE_WEBCAM_CAMERAS) {
        m_cameras = QJsonArray::fromVariantList(np.get<QVariantList>(u"cameras"_s));
        m_crashes = 0;
        startHelper();
    } else if (np.type() == PACKET_TYPE_WEBCAM_STATUS) {
        const QString state = np.get<QString>(u"state"_s);
        const QString message = np.get<QString>(u"message"_s);
        Q_EMIT statusChanged(id, state, message);
        destroyStream(id, false);
        // Video apps cannot show why the camera does not start: say it once
        if (state == "error"_L1 && message != std::exchange(m_lastError, message)) {
            Daemon::instance()->sendSimpleNotification(u"notification"_s,
                                                       i18n("Camera sharing"),
                                                       i18n("%1 could not start its camera: %2", device()->name(), message),
                                                       u"camera-web"_s);
        }
    } else if (StreamRelay *relay = m_streams.value(id); relay && np.type() == PACKET_TYPE_WEBCAM_VIDEO && np.payload()) {
        relay->setSource(np.payload());
        m_lastError.clear();
    }
}

#include "moc_webcamplugin.cpp"
#include "webcamplugin.moc"
