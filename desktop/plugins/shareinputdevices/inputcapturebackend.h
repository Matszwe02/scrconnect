/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QLine>
#include <QObject>
#include <QPointF>

/**
 * Captures this computer's mouse and keyboard when the pointer is pushed against a screen edge: the InputCapture portal
 * (InputCaptureSession) or, on X11 desktops where that portal does not exist, pointer barriers (X11InputCapture).
 */
class InputCaptureBackend : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    virtual void setBarrierEdge(Qt::Edge edge) = 0;
    /** Gives the pointer and keyboard back to this computer, at this position. */
    virtual void release(const QPointF &position) = 0;
    /** The pointer and keyboard are captured right now. */
    virtual bool isActive() const = 0;

Q_SIGNALS:
    /** The pointer left through [barrier]; [delta] is where, relative to its first point. */
    void started(const QLine &barrier, const QPointF &delta);
    void mouseMove(double dx, double dy);
    /** [button]: Linux button code (BTN_LEFT...) */
    void mouseButton(int button, bool pressed);
    void scrollDelta(double dx, double dy);
    void scrollDiscrete(int dx, int dy);
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers, const QString &text);
    /** Every key, modifiers included, with its Linux key code. */
    void keyEvent(int evdev, bool pressed);
    /** The user wants the pointer back (hotkey, screen locked...). */
    void escapeRequested();
    /** This desktop cannot capture input. */
    void unavailable();
};
