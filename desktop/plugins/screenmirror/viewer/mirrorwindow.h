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

#include "../scrcpyprotocol.h"
#include "../videodecoder.h"

struct MirrorOptions {
    QString deviceId;
    QString packageName; // empty: the whole screen
    QString label;
};

/** One window, one session. A single app runs on a virtual display that follows the window size. */
class MirrorWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MirrorWindow(const MirrorOptions &options);
    ~MirrorWindow() override;

    void start();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
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
    QString m_hint; // shown for a while at the bottom
    QTimer m_hintTimer;
};
