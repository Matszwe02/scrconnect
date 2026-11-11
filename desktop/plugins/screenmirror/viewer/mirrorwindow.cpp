/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "mirrorwindow.h"

#include <KLocalizedString>

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusReply>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QScreen>

using namespace Qt::StringLiterals;

namespace
{
const QString kService = u"org.kde.kdeconnect"_s;
const QString kInterface = u"org.kde.kdeconnect.device.screenmirror"_s;

// Qt key -> Android keycode, for the keys that do not produce text
const QHash<int, int> kKeys = {
    {Qt::Key_Return, 66},
    {Qt::Key_Enter, 66},
    {Qt::Key_Backspace, 67},
    {Qt::Key_Delete, 112},
    {Qt::Key_Tab, 61},
    {Qt::Key_Escape, Scrcpy::Android::KeycodeBack},
    {Qt::Key_Up, 19},
    {Qt::Key_Down, 20},
    {Qt::Key_Left, 21},
    {Qt::Key_Right, 22},
    {Qt::Key_Home, 122},
    {Qt::Key_End, 123},
    {Qt::Key_PageUp, 92},
    {Qt::Key_PageDown, 93},
};

int androidMetaState(Qt::KeyboardModifiers modifiers)
{
    return (modifiers & Qt::ShiftModifier ? 0x1 : 0) | (modifiers & Qt::AltModifier ? 0x2 : 0) | (modifiers & Qt::ControlModifier ? 0x1000 : 0)
        | (modifiers & Qt::MetaModifier ? 0x10000 : 0);
}
}

MirrorWindow::MirrorWindow(const MirrorOptions &options)
    : m_options(options)
    , m_plugin(kService, u"/modules/kdeconnect/devices/%1/screenmirror"_s.arg(options.deviceId), kInterface)
{
    setWindowTitle(options.label.isEmpty() ? i18n("Phone") : options.label);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumSize(200, 200);
    resize(appMode() ? QSize(1024, 720) : QSize(405, 900));

    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError error) {
        if (error == QLocalSocket::ServerNotFoundError || error == QLocalSocket::ConnectionRefusedError) {
            setMessage(i18n("The mirroring session has ended."));
        }
    });
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(3000);
    connect(&m_retryTimer, &QTimer::timeout, this, &MirrorWindow::start);
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(150);
    connect(&m_resizeTimer, &QTimer::timeout, this, [this] {
        appMode() ? sendDisplaySize() : fitToPicture();
    });

    connect(&m_socket, &QLocalSocket::readyRead, this, &MirrorWindow::readVideo);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        if (!m_sessionId.isEmpty()) {
            setMessage(m_image.isNull() ? i18n("The phone stopped before showing anything.") : i18n("Mirroring ended."));
        }
    });
    m_hintTimer.setSingleShot(true);
    connect(&m_hintTimer, &QTimer::timeout, this, [this] {
        m_hint.clear();
        update();
    });

    const QString path = m_plugin.path();
    QDBusConnection::sessionBus().connect(kService, path, kInterface, u"statusChanged"_s, this, SLOT(onStatus(QString, QString, QString)));
}

MirrorWindow::~MirrorWindow()
{
    stopSession();
}

void MirrorWindow::start()
{
    stopSession();
    m_parser = Scrcpy::VideoParser();
    m_decoder.close();
    m_image = QImage();
    m_frameSize = {};
    const QDBusReply<bool> screenOff = m_plugin.call(u"screenOffDefault"_s, appMode() ? u"app"_s : u"screen"_s);
    m_screenOff = screenOff.isValid() && screenOff.value();
    m_sentDisplaySize = appMode() ? displaySize() : QSize();
    const QDBusReply<QString> id = m_plugin.call(u"startSession"_s,
                                                appMode() ? u"app"_s : u"screen"_s,
                                                m_options.packageName,
                                                QVariantMap{{u"width"_s, m_sentDisplaySize.width()},
                                                            {u"height"_s, m_sentDisplaySize.height()},
                                                            {u"codec"_s, VideoDecoder::preferredCodec()}});
    if (!id.isValid() || id.value().isEmpty()) {
        setMessage(i18n("Could not start mirroring. Is the phone connected, with screen mirroring enabled?"));
        return;
    }
    m_sessionId = id.value();
    setMessage(i18n("Starting on your phone…"));
    m_socket.connectToServer(u"kdeconnect-mirror-"_s + m_sessionId);
}

void MirrorWindow::stopSession()
{
    m_socket.abort();
    if (!m_sessionId.isEmpty()) {
        m_plugin.call(QDBus::NoBlock, u"stopSession"_s, std::exchange(m_sessionId, {}));
    }
}

void MirrorWindow::onStatus(const QString &sessionId, const QString &state, const QString &message)
{
    if (sessionId != m_sessionId) {
        return;
    }
    if (state == "needs_setup"_L1) {
        setMessage(i18n("The phone needs the one-time ADB setup (SCR Connect on the phone, this computer, Screen mirroring):\n\n%1\n\n"
                        "This window continues by itself.",
                        message));
        m_sessionId.clear();
        m_retryTimer.start();
    } else if (state == "screen_off"_L1) {
        m_hintTimer.stop();
        m_hint = i18n("The phone's screen is off. Right-click here to turn it on.");
        update();
    } else if (state == "screen_on"_L1) {
        m_hint.clear();
        update();
    } else if (state == "error"_L1) {
        m_sessionId.clear();
        setMessage(i18n("The phone reported an error:\n%1", message));
    }
}

void MirrorWindow::readVideo()
{
    for (const Scrcpy::VideoEvent &event : m_parser.push(m_socket.readAll())) {
        switch (event.type) {
        case Scrcpy::VideoEvent::DeviceName:
            setWindowTitle(m_options.label.isEmpty() ? event.name : i18nc("window title: app - phone", "%1 – %2", m_options.label, event.name));
            break;
        case Scrcpy::VideoEvent::CodecId:
            if (!m_decoder.open(event.codec)) {
                setMessage(i18n("This computer cannot decode the phone's video."));
                m_socket.abort();
            }
            break;
        case Scrcpy::VideoEvent::Size:
            m_frameSize = event.size;
            m_decoder.flush();
            m_resizeTimer.start();
            break;
        case Scrcpy::VideoEvent::Frame:
            if (m_decoder.decode(event, m_image)) {
                m_message.clear();
                update();
            }
            break;
        }
    }
    if (!m_parser.error().isEmpty()) {
        setMessage(m_parser.error());
        m_socket.abort();
    }
}

void MirrorWindow::setMessage(const QString &text)
{
    m_message = text;
    update();
}

void MirrorWindow::send(const QByteArray &message)
{
    if (m_socket.state() == QLocalSocket::ConnectedState) {
        m_socket.write(message);
    }
}

void MirrorWindow::setScreenOff(bool off)
{
    m_screenOff = off;
    send(Scrcpy::setScreenPower(!off));
}

QRectF MirrorWindow::pictureRect() const
{
    if (m_frameSize.isEmpty()) {
        return {};
    }
    const QSizeF size = QSizeF(m_frameSize).scaled(this->size(), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
}

QPoint MirrorWindow::framePosition(QPointF position) const
{
    const QRectF picture = pictureRect();
    const QPointF p = (position - picture.topLeft()) / picture.width() * m_frameSize.width();
    return QPoint(qBound(0, int(p.x()), m_frameSize.width() - 1), qBound(0, int(p.y()), m_frameSize.height() - 1));
}

void MirrorWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (!m_image.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(pictureRect(), m_image);
    }
    if (!m_hint.isEmpty()) {
        QRect box = p.fontMetrics().boundingRect(rect().adjusted(16, 0, -16, 0), Qt::AlignCenter | Qt::TextWordWrap, m_hint).adjusted(-10, -6, 10, 6);
        box.moveBottom(height() - 16);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 180));
        p.drawRoundedRect(box, 6, 6);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, m_hint);
    }
    if (!m_message.isEmpty()) {
        p.setPen(Qt::white);
        p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, m_message);
    }
}

void MirrorWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_resizeTimer.start();
}

void MirrorWindow::fitToPicture()
{
    if (m_frameSize.isEmpty() || isMaximized() || isFullScreen()) {
        return;
    }
    QSize size = m_frameSize.scaled(width(), QWIDGETSIZE_MAX, Qt::KeepAspectRatio);
    if (screen() && size.height() > screen()->availableGeometry().height() * 0.92) {
        size = m_frameSize.scaled(QWIDGETSIZE_MAX, int(screen()->availableGeometry().height() * 0.92), Qt::KeepAspectRatio);
    }
    if (qAbs(size.width() - width()) > 1 || qAbs(size.height() - height()) > 1) {
        resize(size);
    }
}

QSize MirrorWindow::displaySize() const
{
    auto side = [this](int logical) {
        return qBound(240, qRound(logical * devicePixelRatioF()), 4096) & ~1;
    };
    return QSize(side(width()), side(height()));
}

void MirrorWindow::sendDisplaySize()
{
    // The phone drops control messages until the stream is up, and may round the size: compare with what was asked last
    if (m_frameSize.isEmpty() || displaySize() == m_sentDisplaySize) {
        return;
    }
    m_sentDisplaySize = displaySize();
    send(Scrcpy::resizeDisplay(m_sentDisplaySize));
}

void MirrorWindow::sendKey(int keycode, bool down, int meta)
{
    send(Scrcpy::injectKeycode(down ? Scrcpy::Android::ActionDown : Scrcpy::Android::ActionUp, keycode, meta));
}

void MirrorWindow::sendTouch(quint8 action, QPointF position)
{
    if (!m_frameSize.isEmpty()) {
        send(Scrcpy::injectTouch(action, framePosition(position), m_frameSize));
    }
}

void MirrorWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && pictureRect().contains(event->position())) {
        m_touching = true;
        sendTouch(Scrcpy::Android::ActionDown, event->position());
    } else if (event->button() == Qt::RightButton) {
        send(Scrcpy::backOrScreenOn(Scrcpy::Android::ActionDown));
    } else if (event->button() == Qt::MiddleButton) {
        sendKey(Scrcpy::Android::KeycodeHome, true);
    }
}

void MirrorWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_touching) {
        sendTouch(Scrcpy::Android::ActionMove, event->position());
    }
}

void MirrorWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_touching) {
        m_touching = false;
        sendTouch(Scrcpy::Android::ActionUp, event->position());
    } else if (event->button() == Qt::RightButton) {
        send(Scrcpy::backOrScreenOn(Scrcpy::Android::ActionUp));
    } else if (event->button() == Qt::MiddleButton) {
        sendKey(Scrcpy::Android::KeycodeHome, false);
    }
}

void MirrorWindow::wheelEvent(QWheelEvent *event)
{
    if (!m_frameSize.isEmpty()) {
        const QPointF notches = QPointF(event->angleDelta()) / 120.0;
        send(Scrcpy::injectScroll(framePosition(event->position()), m_frameSize, notches.x(), notches.y()));
    }
}

bool MirrorWindow::focusNextPrevChild(bool)
{
    return false;
}

void MirrorWindow::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = event->key();
    if (mods == Qt::AltModifier && (key == Qt::Key_H || key == Qt::Key_S)) {
        const int keycode = key == Qt::Key_H ? Scrcpy::Android::KeycodeHome : Scrcpy::Android::KeycodeAppSwitch;
        sendKey(keycode, true);
        sendKey(keycode, false);
    } else if (mods == Qt::AltModifier && key == Qt::Key_B) {
        send(Scrcpy::backOrScreenOn(Scrcpy::Android::ActionDown));
        send(Scrcpy::backOrScreenOn(Scrcpy::Android::ActionUp));
    } else if (mods == Qt::AltModifier && key == Qt::Key_N) {
        send(Scrcpy::expandNotificationPanel());
    } else if (mods == (Qt::AltModifier | Qt::ShiftModifier) && key == Qt::Key_N) {
        send(Scrcpy::collapsePanels());
    } else if (mods == Qt::AltModifier && key == Qt::Key_R) {
        send(Scrcpy::resetVideo());
    } else if (key == Qt::Key_O && (mods == Qt::ControlModifier || mods == (Qt::ControlModifier | Qt::ShiftModifier))) {
        setScreenOff(mods == Qt::ControlModifier);
    } else if (mods == Qt::ControlModifier && key == Qt::Key_V) {
        if (const QString text = QApplication::clipboard()->text(); !text.isEmpty()) {
            send(Scrcpy::pasteText(text));
        }
    } else if (kKeys.contains(key)) {
        sendKey(kKeys.value(key), true, androidMetaState(mods));
    } else if (!(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && !event->text().isEmpty() && event->text().at(0).isPrint()) {
        send(Scrcpy::injectText(event->text()));
    } else {
        QWidget::keyPressEvent(event);
    }
}

void MirrorWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (kKeys.contains(event->key()) && !event->isAutoRepeat()) {
        sendKey(kKeys.value(event->key()), false, androidMetaState(event->modifiers()));
    }
}

void MirrorWindow::contextMenuEvent(QContextMenuEvent *event)
{
    if (event->reason() == QContextMenuEvent::Mouse && !(event->modifiers() & Qt::ControlModifier)) {
        return; // a plain right click is "back" on the phone
    }
    QMenu menu(this);
    QAction *off = menu.addAction(i18n("Phone screen off"), QKeySequence(Qt::CTRL | Qt::Key_O));
    off->setCheckable(true);
    off->setChecked(m_screenOff);
    connect(off, &QAction::toggled, this, &MirrorWindow::setScreenOff);
    menu.exec(event->globalPos());
}

void MirrorWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MirrorWindow::dropEvent(QDropEvent *event)
{
    QStringList urls;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            urls << url.toString();
        }
    }
    QDBusInterface share(kService, u"/modules/kdeconnect/devices/%1/share"_s.arg(m_options.deviceId), u"org.kde.kdeconnect.device.share"_s);
    if (!urls.isEmpty() && share.isValid()) {
        share.call(QDBus::NoBlock, u"shareUrls"_s, urls);
        event->acceptProposedAction();
    }
}

void MirrorWindow::closeEvent(QCloseEvent *event)
{
    m_retryTimer.stop();
    stopSession();
    QWidget::closeEvent(event);
}

#include "moc_mirrorwindow.cpp"
