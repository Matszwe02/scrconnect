/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QDBusInterface>
#include <QImage>
#include <QLocalSocket>
#include <QTimer>
#include <QWidget>

#include <memory>

#include "../scrcpyprotocol.h"
#include "kwinscript.h"
#include "../videodecoder.h"

struct MirrorOptions {
    QString deviceId;
    QString packageName; // empty: the whole screen
    QString label;
    QString attachSession; // a session the daemon started (Open on PC, hand-off)
};

/** One window, one session. A single app runs on a virtual display that follows the window size. */
class MirrorWindow : public QWidget
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kdeconnect.mirror.window")
public:
    explicit MirrorWindow(const MirrorOptions &options);
    ~MirrorWindow() override;

    void start();
    void followPointer()
    {
        m_followPointer = true;
    }

public Q_SLOTS:
    Q_SCRIPTABLE void returnToPhone();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private Q_SLOTS:
    void onStatus(const QString &sessionId, const QString &state, const QString &message);

private:
    void lockSize(bool lock);
    bool appMode() const
    {
        return !m_options.packageName.isEmpty();
    }
    void stopSession();
    void readVideo();
    void setMessage(const QString &text);
    void send(const QByteArray &message);
    void sendKey(int keycode, bool down, int meta = 0);
    void sendTouch(quint8 action, QPointF position);
    void setScreenOff(bool off);
    void fitToPicture();
    void sendDisplaySize();
    QSize displaySize() const;
    QRectF pictureRect() const;
    QPoint framePosition(QPointF position) const;

    MirrorOptions m_options;
    QDBusInterface m_plugin;
    QString m_sessionId;
    QLocalSocket m_socket;
    QTimer m_retryTimer;
    QTimer m_resizeTimer;
    Scrcpy::VideoParser m_parser;
    VideoDecoder m_decoder;
    QImage m_image;
    QSize m_frameSize;
    QSize m_sentDisplaySize;
    QString m_message;
    bool m_touching = false;
    bool m_screenOff = false;
    bool m_followPointer = false;
    std::unique_ptr<KWinScript> m_kwin;
    QString m_hint; // shown for a while at the bottom
    QTimer m_hintTimer;
    int m_phoneEdge = 0; // Qt::Edge
    bool m_dragArmed = false;
    QTimer m_dragTimer;
    bool m_sizeLocked = false;
    QSize m_unlockedMinimum;
    QSize m_unlockedMaximum;
};
