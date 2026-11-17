/**
 * SPDX-FileCopyrightText: 2024 David Redondo <kde@david-redondo.de>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */
#include "shareinputdevicesplugin.h"

#include "inputcapturesession.h"
#include "plugin_shareinputdevices_debug.h"
#ifdef HAVE_X11_BACKEND
#include "x11inputcapture.h"
#endif

#include <core/daemon.h>
#include <core/device.h>

#include <KLocalizedString>
#include <KPluginFactory>

#include <QDBusConnection>
#include <QPointF>

#include <linux/input-event-codes.h>

using namespace Qt::StringLiterals;

#define PACKET_TYPE_MOUSEPAD_REQUEST u"kdeconnect.mousepad.request"_s
#define PACKET_TYPE_SHAREINPUTDEVICES u"kdeconnect.shareinputdevices"_s
#define PACKET_TYPE_SHAREINPUTDEVICES_REQUEST u"kdeconnect.shareinputdevices.request"_s
#define PACKET_TYPE_SHAREINPUTDEVICES_LEAVE u"kdeconnect.shareinputdevices.leave"_s

// The phone sends a packet of type shareinputdevices (a heartbeat, "alive") every second while it holds the pointer
constexpr int kWatchdogIntervalMs = 1000;
constexpr int kPhoneSilenceLimitMs = 5000;

QMap<int, int> specialKeysMap = {
    // 0,              // Invalid
    {Qt::Key_Backspace, 1},
    {Qt::Key_Tab, 2},
    // XK_Linefeed,    // 3
    {Qt::Key_Left, 4},
    {Qt::Key_Up, 5},
    {Qt::Key_Right, 6},
    {Qt::Key_Down, 7},
    {Qt::Key_PageUp, 8},
    {Qt::Key_PageDown, 9},
    {Qt::Key_Home, 10},
    {Qt::Key_End, 11},
    {Qt::Key_Return, 12},
    {Qt::Key_Enter, 12},
    {Qt::Key_Delete, 13},
    {Qt::Key_Escape, 14},
    {Qt::Key_SysReq, 15},
    {Qt::Key_ScrollLock, 16},
    // 0,              // 17
    // 0,              // 18
    // 0,              // 19
    // 0,              // 20
    {Qt::Key_F1, 21},
    {Qt::Key_F2, 22},
    {Qt::Key_F3, 23},
    {Qt::Key_F4, 24},
    {Qt::Key_F5, 25},
    {Qt::Key_F6, 26},
    {Qt::Key_F7, 27},
    {Qt::Key_F8, 28},
    {Qt::Key_F9, 29},
    {Qt::Key_F10, 30},
    {Qt::Key_F11, 31},
    {Qt::Key_F12, 32},
};

K_PLUGIN_CLASS_WITH_JSON(ShareInputDevicesPlugin, "kdeconnect_shareinputdevices.json")

ShareInputDevicesPlugin::ShareInputDevicesPlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
#ifdef HAVE_X11_BACKEND
    if (X11InputCapture::isUsable()) {
        m_inputCaptureSession = new X11InputCapture(this);
    }
#endif
    if (!m_inputCaptureSession) {
        m_inputCaptureSession = new InputCaptureSession(this);
    }
    connect(m_inputCaptureSession, &InputCaptureBackend::started, this, [this](const QLine &barrier, const QPointF &delta) {
        m_activatedBarrier = barrier;
        m_shellInput = false;
        m_lastPhonePacket.restart();
        m_watchdog.start();
        // "modded" and "along" (position along the edge, 0..1) are for this fork's phone app; others ignore them
        const double length = barrier.dx() ? barrier.dx() : barrier.dy();
        const double along = length ? (barrier.dx() ? delta.x() : delta.y()) / length : 0.5;
        NetworkPacket packet(PACKET_TYPE_SHAREINPUTDEVICES_REQUEST,
                             {{u"exitEdge"_s, configuredEdge()}, {u"deltax"_s, delta.x()}, {u"deltay"_s, delta.y()}, {u"along"_s, along}, {u"modded"_s, true}});
        sendPacket(packet);
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::mouseMove, this, [this](double x, double y) {
        sendInput({{u"dx"_s, x * m_pointerSpeed}, {u"dy"_s, y * m_pointerSpeed}});
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::mouseButton, this, [this](int button, bool pressed) {
        QVariantMap input;
        if (m_shellInput) {
            input = {{u"button"_s, button}, {u"pressed"_s, pressed}};
        } else if (button == BTN_LEFT) {
            // mousepad only supports separate press and release for left click currently, so send event on release even though not entirely correct
            input.insert(pressed ? u"singlehold"_s : u"singlerelease"_s, true);
        } else if (button == BTN_RIGHT && pressed) {
            input.insert(u"rightclick"_s, true);
        } else if (button == BTN_MIDDLE) {
            input.insert(u"middleclick"_s, true);
        }
        sendInput(input);
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::scrollDelta, this, [this](double x, double y) {
        qDebug() << "scrollDelta" << x << y;
        // scrollDirection in kdeconnect is inverted compared to what we get here
        sendInput({{u"scroll"_s, true}, {u"dx"_s, x}, {u"dy"_s, y}});
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::scrollDiscrete, this, [this](int x, int y) {
        qDebug() << "scolldiscrete" << x << y;
        constexpr auto anglePer120Step = 15 / 120.0;
        QVariantMap input{{u"scroll"_s, true}, {u"dx"_s, x * anglePer120Step}, {u"dy"_s, -y * anglePer120Step}};
        if (m_shellInput) { // wheel notches, positive = right / up
            input.insert(u"wheelX"_s, x / 120.0);
            input.insert(u"wheelY"_s, -y / 120.0);
        }
        sendInput(input);
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::keyEvent, this, [this](int evdev, bool pressed) {
        if (m_shellInput) {
            sendInput({{u"evdev"_s, evdev}, {u"pressed"_s, pressed}});
        }
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::unavailable, this, [this] {
        Daemon::instance()->sendSimpleNotification(u"error"_s,
                                                   i18n("Phone Leap"),
                                                   i18n("This desktop does not let applications capture the pointer at the screen edge (it needs "
                                                        "the InputCapture portal: Plasma 6.1 or GNOME 45 and newer, or an X11 session)."),
                                                   u"dialog-input-devices"_s);
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::escapeRequested, this, &ShareInputDevicesPlugin::giveBack);
    m_watchdog.setInterval(kWatchdogIntervalMs);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (!m_inputCaptureSession->isActive()) {
            m_watchdog.stop();
        } else if (m_lastPhonePacket.hasExpired(kPhoneSilenceLimitMs)) {
            qCWarning(KDECONNECT_PLUGIN_SHAREINPUTDEVICES) << "The phone has been silent for" << kPhoneSilenceLimitMs << "ms, taking the pointer back";
            giveBack();
            Daemon::instance()->sendSimpleNotification(u"notification"_s,
                                                       i18n("Phone Leap"),
                                                       i18n("The phone stopped responding, so the pointer was given back to this computer."),
                                                       u"dialog-input-devices"_s);
        }
    });
    connect(m_inputCaptureSession, &InputCaptureBackend::key, this, [this](Qt::Key key, Qt::KeyboardModifiers modifiers, const QString &text) {
        if (key == Qt::Key_X && modifiers == Qt::AltModifier && qobject_cast<InputCaptureSession *>(m_inputCaptureSession)) {
            Q_EMIT m_inputCaptureSession->escapeRequested(); // the X11 backend handles this itself
            return;
        }
        if (m_shellInput) {
            return;
        }
        qDebug() << "sending key" << text << modifiers << specialKeysMap.value(key);
        sendInput({
            {u"key"_s, text},
            {u"specialKey"_s, specialKeysMap.value(key)},
            {u"shift"_s, static_cast<bool>(modifiers & Qt::ShiftModifier)},
            {u"ctrl"_s, static_cast<bool>(modifiers & Qt::ControlModifier)},
            {u"alt"_s, static_cast<bool>(modifiers & Qt::AltModifier)},
            {u"super"_s, static_cast<bool>(modifiers & Qt::MetaModifier)},
        });
    });

    // Phones have several times the pixel density of a monitor: the pointer may move faster there (percent)
    auto applyConfig = [this] {
        m_inputCaptureSession->setBarrierEdge(configuredEdge());
        m_pointerSpeed = qBound(25, config()->getInt(u"pointerSpeed"_s, 100), 400) / 100.0;
    };
    applyConfig();
    connect(config(), &KdeConnectPluginConfig::configChanged, this, applyConfig);
}

void ShareInputDevicesPlugin::giveBack()
{
    m_watchdog.stop();
    m_inputCaptureSession->release(QPointF(m_activatedBarrier.p1() + m_activatedBarrier.p2()) / 2);
    m_shellInput = false;
    // Always: the phone may be holding the pointer in a way this computer does not know of
    NetworkPacket packet(PACKET_TYPE_SHAREINPUTDEVICES_LEAVE);
    sendPacket(packet);
}

// "leap" tells this fork's phone app Phone Leap's input from the remote control's and keyboard's; others ignore it
void ShareInputDevicesPlugin::sendInput(QVariantMap input)
{
    input.insert(u"leap"_s, true);
    NetworkPacket packet(PACKET_TYPE_MOUSEPAD_REQUEST, input);
    sendPacket(packet);
}

Qt::Edge ShareInputDevicesPlugin::configuredEdge() const
{
    return static_cast<Qt::Edge>(config()->getInt(u"edge"_s, Qt::LeftEdge));
}

void ShareInputDevicesPlugin::receivePacket(const NetworkPacket &np)
{
    if (np.type() == PACKET_TYPE_SHAREINPUTDEVICES) {
        m_lastPhonePacket.restart();
        if (np.get<bool>(u"shellInput"_s)) {
            m_shellInput = true;
        }
        if (np.get<QString>(u"reason"_s) == "needs_setup"_L1 && !std::exchange(m_setupNotified, true)) {
            Daemon::instance()->sendSimpleNotification(u"notification"_s,
                                                       device()->name(),
                                                       i18n("The phone cannot take the pointer yet: finish the ADB setup on the phone (SCR Connect, this computer, Screen mirroring), "
                                                            "or enable the Mouse receiver (accessibility) of SCR Connect there."),
                                                       u"input-mouse"_s);
        }
        // Only while the pointer is captured: the phone repeats its release when input keeps coming after one, in case
        // this computer missed it, and some of that input was merely on its way
        if (np.has(u"releaseDeltax"_s) && m_inputCaptureSession->isActive()) {
            m_shellInput = false;
            QPointF releaseDelta{np.get<double>(u"releaseDeltax"_s), np.get<double>(u"releaseDeltay"_s)};
            if (np.has(u"releaseAlong"_s)) { // this fork's phone app: relative, independent of the screen sizes
                releaseDelta = QPointF(m_activatedBarrier.p2() - m_activatedBarrier.p1()) * np.get<double>(u"releaseAlong"_s);
            }
            m_lastReleasePosition = m_activatedBarrier.p1() + releaseDelta;
            m_inputCaptureSession->release(m_lastReleasePosition);
        }
        // Dragged out of the phone: its app in front comes along as a window (screen mirroring). The phone sends this
        // after the release, once it found the app.
        if (KdeConnectPlugin *mirror = device()->plugin(u"kdeconnect_screenmirror"_s); mirror && np.has(u"handoffPackage"_s)) {
            QMetaObject::invokeMethod(mirror,
                                      "openHandoffWindow",
                                      Q_ARG(QString, np.get<QString>(u"handoffPackage"_s)),
                                      Q_ARG(QString, np.get<QString>(u"handoffLabel"_s)),
                                      Q_ARG(int, np.get<int>(u"handoffTaskId"_s)),
                                      Q_ARG(int, int(m_lastReleasePosition.x())),
                                      Q_ARG(int, int(m_lastReleasePosition.y())));
        }
    }
}

QString ShareInputDevicesPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/shareinputdevices").arg(device()->id());
}

#include "shareinputdevicesplugin.moc"
