/**
 * SPDX-FileCopyrightText: 2024 David Redondo <kde@david-redondo.de>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QLine>
#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QTimer>

#include <core/kdeconnectplugin.h>

class InputCaptureBackend;
class QScreen;

class ShareInputDevicesPlugin : public KdeConnectPlugin
{
    Q_OBJECT
public:
    ShareInputDevicesPlugin(QObject *parent, const QVariantList &args);

    void receivePacket(const NetworkPacket &np) override;
    QString dbusPath() const override;

private:
    Qt::Edge configuredEdge() const;
    void sendInput(QVariantMap input);
    /** Takes the pointer and keyboard back from the phone, whatever it is doing. */
    void giveBack();
    InputCaptureBackend *m_inputCaptureSession = nullptr;
    QLine m_activatedBarrier;
    QPointF m_lastReleasePosition;
    bool m_shellInput = false; // the phone injects through the shell: it understands exact buttons and key codes
    bool m_setupNotified = false;
    double m_pointerSpeed = 1;
    // While the pointer is captured the phone must keep showing signs of life; if it stops (link lost, app hung, ADB
    // bridge stuck) nobody could ever hand the pointer back, so this computer takes it back by itself.
    QTimer m_watchdog;
    QElapsedTimer m_lastPhonePacket;
};
