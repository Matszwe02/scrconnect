/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QRect>
#include <QSocketNotifier>

#include <memory>

#include <xcb/xcb.h>

#include "inputcapturebackend.h"

struct xkb_keymap;
struct xkb_state;

/**
 * Input capture for X11 sessions, which have no InputCapture portal. An XFixes pointer barrier lies on the chosen edge of
 * every monitor that has no neighbour there; pushing the pointer into it far enough grabs pointer and keyboard, hides the
 * cursor and reports XInput 2 raw events (relative motion, buttons, keys) until release(). The grabs end with the process,
 * on screen lock, and with Alt+X or Ctrl+Alt+Shift+Esc.
 */
class X11InputCapture : public InputCaptureBackend
{
    Q_OBJECT
public:
    explicit X11InputCapture(QObject *parent);
    ~X11InputCapture() override;

    /** A plain X11 session (not XWayland: barriers only affect X clients there) with XFixes 5 and XInput 2.3. */
    static bool isUsable();

    void setBarrierEdge(Qt::Edge edge) override;
    void release(const QPointF &position) override;
    bool isActive() const override
    {
        return m_active;
    }

private Q_SLOTS:
    void onScreenLocked(bool locked = true);

private:
    void rebuildBarriers();
    void readEvents();
    void barrierHit(xcb_generic_event_t *event);
    void activate(const QPointF &position);
    void deactivate();
    void selectEvents(bool raw);
    void rawKey(uint32_t keycode, bool pressed);
    void rawButton(uint32_t button, bool pressed);

    xcb_connection_t *m_connection = nullptr;
    xcb_window_t m_root = 0;
    xcb_window_t m_grabWindow = 0;
    uint8_t m_xiOpcode = 0;
    uint8_t m_randrEvent = 0;
    std::unique_ptr<QSocketNotifier> m_notifier;
    Qt::Edge m_edge = Qt::LeftEdge;
    struct Barrier {
        uint32_t id = 0;
        QLine line;
        QRect monitor;
    };
    QList<Barrier> m_barriers;
    Barrier m_crossed{};
    double m_push = 0;
    QElapsedTimer m_lastHit;
    QElapsedTimer m_lastRelease;
    bool m_active = false;
    xkb_keymap *m_keymap = nullptr;
    xkb_state *m_xkbState = nullptr;
    int32_t m_keyboardId = -1;
};
