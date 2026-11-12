/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "x11inputcapture.h"

#include <QDBusConnection>

#include <private/qxkbcommon_p.h>

#include <xcb/randr.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <linux/input-event-codes.h>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace
{
constexpr double kPushThreshold = 10; // pixels the pointer must be pushed beyond the edge

double fp3232(const xcb_input_fp3232_t &v)
{
    return v.integral + v.frac / 4294967296.0;
}

template<typename T>
using Reply = std::unique_ptr<T, decltype(&free)>;

template<typename T>
Reply<T> reply(T *r)
{
    return Reply<T>(r, &free);
}

/** The ranges of a monitor's edge that do not touch another monitor, as [from, to) along the edge. */
QList<std::pair<int, int>> freeSegments(const QRect &monitor, Qt::Edge edge, const QList<QRect> &monitors)
{
    const bool vertical = edge == Qt::LeftEdge || edge == Qt::RightEdge;
    const QRect outside = edge == Qt::LeftEdge ? QRect(monitor.left() - 1, monitor.top(), 1, monitor.height())
        : edge == Qt::RightEdge                ? QRect(monitor.right() + 1, monitor.top(), 1, monitor.height())
        : edge == Qt::TopEdge                  ? QRect(monitor.left(), monitor.top() - 1, monitor.width(), 1)
                                               : QRect(monitor.left(), monitor.bottom() + 1, monitor.width(), 1);
    QList<std::pair<int, int>> blocked;
    for (const QRect &other : monitors) {
        if (const QRect touch = other.intersected(outside); !touch.isEmpty()) {
            blocked.append(vertical ? std::pair(touch.top(), touch.bottom() + 1) : std::pair(touch.left(), touch.right() + 1));
        }
    }
    std::sort(blocked.begin(), blocked.end());
    QList<std::pair<int, int>> segments;
    int pos = vertical ? monitor.top() : monitor.left();
    const int end = vertical ? monitor.bottom() + 1 : monitor.right() + 1;
    for (const auto &[from, to] : std::as_const(blocked)) {
        if (from > pos) {
            segments.append({pos, from});
        }
        pos = std::max(pos, to);
    }
    if (pos < end) {
        segments.append({pos, end});
    }
    return segments;
}

struct EventMask {
    xcb_input_event_mask_t head{XCB_INPUT_DEVICE_ALL_MASTER, 1};
    uint32_t mask;
};
}

bool X11InputCapture::isUsable()
{
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland"_L1 || qEnvironmentVariableIsSet("WAYLAND_DISPLAY") || qEnvironmentVariableIsEmpty("DISPLAY")) {
        return false;
    }
    xcb_connection_t *c = xcb_connect(nullptr, nullptr);
    const auto fixes = reply(xcb_xfixes_query_version_reply(c, xcb_xfixes_query_version(c, 5, 0), nullptr));
    const auto xi = reply(xcb_input_xi_query_version_reply(c, xcb_input_xi_query_version(c, 2, 3), nullptr));
    const bool usable = fixes && xi && fixes->major_version >= 5 && (xi->major_version > 2 || xi->minor_version >= 3);
    xcb_disconnect(c);
    return usable;
}

X11InputCapture::X11InputCapture(QObject *parent)
    : InputCaptureBackend(parent)
{
    int screen = 0;
    m_connection = xcb_connect(nullptr, &screen);
    if (xcb_connection_has_error(m_connection)) {
        return;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
    for (; screen > 0; --screen) {
        xcb_screen_next(&it);
    }
    m_root = it.data->root;
    m_xiOpcode = xcb_get_extension_data(m_connection, &xcb_input_id)->major_opcode;
    m_randrEvent = xcb_get_extension_data(m_connection, &xcb_randr_id)->first_event;
    // The versions this code speaks must be announced before using the extensions
    free(xcb_input_xi_query_version_reply(m_connection, xcb_input_xi_query_version(m_connection, 2, 3), nullptr));
    free(xcb_xfixes_query_version_reply(m_connection, xcb_xfixes_query_version(m_connection, 5, 0), nullptr));
    free(xcb_randr_query_version_reply(m_connection, xcb_randr_query_version(m_connection, 1, 5), nullptr));
    xcb_randr_select_input(m_connection, m_root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

    m_grabWindow = xcb_generate_id(m_connection);
    const uint32_t overrideRedirect = 1;
    xcb_create_window(m_connection, 0, m_grabWindow, m_root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, 0, XCB_CW_OVERRIDE_REDIRECT, &overrideRedirect);
    selectEvents(false);

    if (xkb_x11_setup_xkb_extension(m_connection, 1, 0, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr, nullptr)) {
        m_keyboardId = xkb_x11_get_core_keyboard_device_id(m_connection);
        xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        m_keymap = xkb_x11_keymap_new_from_device(context, m_connection, m_keyboardId, XKB_KEYMAP_COMPILE_NO_FLAGS);
        xkb_context_unref(context);
    }

    m_notifier = std::make_unique<QSocketNotifier>(xcb_get_file_descriptor(m_connection), QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &X11InputCapture::readEvents);
    m_lastRelease.start();

    QDBusConnection::systemBus().connect({}, {}, u"org.freedesktop.login1.Session"_s, u"Lock"_s, this, SLOT(onScreenLocked()));
    for (const QString &service : {u"org.freedesktop.ScreenSaver"_s, u"org.cinnamon.ScreenSaver"_s, u"org.gnome.ScreenSaver"_s, u"org.mate.ScreenSaver"_s}) {
        QDBusConnection::sessionBus().connect(service, {}, service, u"ActiveChanged"_s, this, SLOT(onScreenLocked(bool)));
    }
}

X11InputCapture::~X11InputCapture()
{
    deactivate();
    xkb_state_unref(m_xkbState);
    xkb_keymap_unref(m_keymap);
    xcb_disconnect(m_connection);
}

void X11InputCapture::onScreenLocked(bool locked)
{
    if (locked && m_active) {
        Q_EMIT escapeRequested();
    }
}

void X11InputCapture::setBarrierEdge(Qt::Edge edge)
{
    m_edge = edge;
    rebuildBarriers();
}

void X11InputCapture::rebuildBarriers()
{
    if (!m_notifier || m_active) {
        return;
    }
    for (const Barrier &barrier : std::as_const(m_barriers)) {
        xcb_xfixes_delete_pointer_barrier(m_connection, barrier.id);
    }
    m_barriers.clear();
    QList<QRect> monitors;
    if (const auto r = reply(xcb_randr_get_monitors_reply(m_connection, xcb_randr_get_monitors(m_connection, m_root, true), nullptr))) {
        for (auto it = xcb_randr_get_monitors_monitors_iterator(r.get()); it.rem; xcb_randr_monitor_info_next(&it)) {
            monitors.append(QRect(it.data->x, it.data->y, it.data->width, it.data->height));
        }
    }
    // The directions in which the barrier lets the pointer through: back into the screen
    const uint32_t through = m_edge == Qt::LeftEdge ? XCB_XFIXES_BARRIER_DIRECTIONS_POSITIVE_X
        : m_edge == Qt::RightEdge                   ? XCB_XFIXES_BARRIER_DIRECTIONS_NEGATIVE_X
        : m_edge == Qt::TopEdge                     ? XCB_XFIXES_BARRIER_DIRECTIONS_POSITIVE_Y
                                                    : XCB_XFIXES_BARRIER_DIRECTIONS_NEGATIVE_Y;
    for (const QRect &monitor : std::as_const(monitors)) {
        const bool vertical = m_edge == Qt::LeftEdge || m_edge == Qt::RightEdge;
        const int fixed = m_edge == Qt::LeftEdge ? monitor.left()
            : m_edge == Qt::RightEdge            ? monitor.right() + 1
            : m_edge == Qt::TopEdge              ? monitor.top()
                                                 : monitor.bottom() + 1;
        for (const auto &[from, to] : freeSegments(monitor, m_edge, monitors)) {
            const QLine line = vertical ? QLine(fixed, from, fixed, to) : QLine(from, fixed, to, fixed);
            const uint32_t id = xcb_generate_id(m_connection);
            xcb_xfixes_create_pointer_barrier(m_connection, id, m_root, line.x1(), line.y1(), line.x2(), line.y2(), through, 0, nullptr);
            m_barriers.append({id, line, monitor});
        }
    }
    xcb_flush(m_connection);
}

void X11InputCapture::selectEvents(bool raw)
{
    EventMask m;
    m.mask = XCB_INPUT_XI_EVENT_MASK_BARRIER_HIT | XCB_INPUT_XI_EVENT_MASK_BARRIER_LEAVE;
    if (raw) {
        m.mask |= XCB_INPUT_XI_EVENT_MASK_RAW_MOTION | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE
            | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE;
    }
    xcb_input_xi_select_events(m_connection, m_root, 1, &m.head);
    xcb_flush(m_connection);
}

void X11InputCapture::readEvents()
{
    while (xcb_generic_event_t *event = xcb_poll_for_event(m_connection)) {
        const auto ge = reinterpret_cast<xcb_ge_generic_event_t *>(event);
        if (m_randrEvent && (event->response_type & 0x7f) == m_randrEvent + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
            rebuildBarriers();
        } else if ((event->response_type & 0x7f) != XCB_GE_GENERIC || ge->extension != m_xiOpcode) {
        } else if (ge->event_type == XCB_INPUT_BARRIER_HIT) {
            barrierHit(event);
        } else if (ge->event_type == XCB_INPUT_BARRIER_LEAVE) {
            m_push = 0;
        } else if (!m_active) {
        } else if (ge->event_type == XCB_INPUT_RAW_MOTION) {
            // Valuators: a bit mask, then one value per set bit. Axis 0 is x, axis 1 is y.
            const auto motion = reinterpret_cast<xcb_input_raw_motion_event_t *>(event);
            const uint32_t mask = motion->valuators_len ? xcb_input_raw_button_press_valuator_mask(motion)[0] : 0;
            const xcb_input_fp3232_t *values = xcb_input_raw_button_press_axisvalues(motion);
            double delta[2] = {0, 0};
            for (int axis = 0, index = 0; axis < 2; ++axis) {
                if (mask & (1u << axis)) {
                    delta[axis] = fp3232(values[index++]);
                }
            }
            // Pointer warps by other programs show up as absurd jumps
            const double limit = 2.0 * std::max(m_crossed.monitor.width(), m_crossed.monitor.height());
            if ((delta[0] || delta[1]) && std::abs(delta[0]) < limit && std::abs(delta[1]) < limit) {
                Q_EMIT mouseMove(delta[0], delta[1]);
            }
        } else if (ge->event_type == XCB_INPUT_RAW_BUTTON_PRESS || ge->event_type == XCB_INPUT_RAW_BUTTON_RELEASE) {
            rawButton(reinterpret_cast<xcb_input_raw_button_press_event_t *>(event)->detail, ge->event_type == XCB_INPUT_RAW_BUTTON_PRESS);
        } else if (ge->event_type == XCB_INPUT_RAW_KEY_PRESS || ge->event_type == XCB_INPUT_RAW_KEY_RELEASE) {
            rawKey(reinterpret_cast<xcb_input_raw_key_press_event_t *>(event)->detail, ge->event_type == XCB_INPUT_RAW_KEY_PRESS);
        }
        free(event);
    }
    if (xcb_connection_has_error(m_connection)) {
        m_notifier.reset();
        if (m_active) {
            m_active = false;
            Q_EMIT escapeRequested();
        }
    }
}

void X11InputCapture::barrierHit(xcb_generic_event_t *event)
{
    const auto hit = reinterpret_cast<xcb_input_barrier_hit_event_t *>(event);
    const auto barrier = std::find_if(m_barriers.cbegin(), m_barriers.cend(), [hit](const Barrier &b) {
        return b.id == hit->barrier;
    });
    if (m_active || barrier == m_barriers.cend() || m_lastRelease.elapsed() < 500) {
        return;
    }
    if (!m_lastHit.isValid() || m_lastHit.elapsed() > 200) {
        m_push = 0;
    }
    m_lastHit.start();
    const double dx = fp3232(hit->dx);
    const double dy = fp3232(hit->dy);
    const double outward = m_edge == Qt::LeftEdge ? -dx : m_edge == Qt::RightEdge ? dx : m_edge == Qt::TopEdge ? -dy : dy;
    m_push += std::max(0.0, outward);
    if (m_push >= kPushThreshold) {
        m_push = 0;
        m_crossed = *barrier;
        activate(QPointF(hit->root_x / 65536.0, hit->root_y / 65536.0));
    }
}

void X11InputCapture::activate(const QPointF &position)
{
    xcb_map_window(m_connection, m_grabWindow);
    const auto pointer = reply(xcb_grab_pointer_reply(
        m_connection,
        xcb_grab_pointer(m_connection, false, m_grabWindow, 0, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
        nullptr));
    const auto keyboard = reply(
        xcb_grab_keyboard_reply(m_connection, xcb_grab_keyboard(m_connection, false, m_grabWindow, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), nullptr));
    m_active = true;
    if (!pointer || pointer->status != XCB_GRAB_STATUS_SUCCESS || !keyboard || keyboard->status != XCB_GRAB_STATUS_SUCCESS) {
        deactivate();
        return;
    }
    xcb_xfixes_hide_cursor(m_connection, m_root);
    if (m_keymap) {
        xkb_state_unref(m_xkbState);
        m_xkbState = xkb_x11_state_new_from_device(m_keymap, m_connection, m_keyboardId);
    }
    selectEvents(true);
    Q_EMIT started(m_crossed.line, position - m_crossed.line.p1());
}

void X11InputCapture::deactivate()
{
    if (!m_active || !m_notifier) {
        return;
    }
    m_active = false;
    selectEvents(false);
    xcb_ungrab_keyboard(m_connection, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(m_connection, XCB_CURRENT_TIME);
    xcb_xfixes_show_cursor(m_connection, m_root);
    xcb_unmap_window(m_connection, m_grabWindow);
    xcb_flush(m_connection);
    m_lastRelease.start();
}

void X11InputCapture::release(const QPointF &position)
{
    if (!m_active) {
        return;
    }
    deactivate();
    // Just inside the monitor, so that the pointer does not hit the barrier again at once
    const QRect m = m_crossed.monitor;
    QPointF p(std::clamp<double>(position.x(), m.left(), m.right()), std::clamp<double>(position.y(), m.top(), m.bottom()));
    if (m_edge == Qt::LeftEdge || m_edge == Qt::RightEdge) {
        p.setX(m_edge == Qt::LeftEdge ? m.left() + 2 : m.right() - 2);
    } else {
        p.setY(m_edge == Qt::TopEdge ? m.top() + 2 : m.bottom() - 2);
    }
    xcb_warp_pointer(m_connection, XCB_NONE, m_root, 0, 0, 0, 0, int16_t(p.x()), int16_t(p.y()));
    xcb_flush(m_connection);
}

void X11InputCapture::rawButton(uint32_t button, bool pressed)
{
    switch (button) {
    case 1:
        Q_EMIT mouseButton(BTN_LEFT, pressed);
        break;
    case 2:
        Q_EMIT mouseButton(BTN_MIDDLE, pressed);
        break;
    case 3:
        Q_EMIT mouseButton(BTN_RIGHT, pressed);
        break;
    case 4: // wheel up, down, left, right: 120 per notch like libei, positive = down / right
    case 5:
    case 6:
    case 7:
        if (pressed) {
            Q_EMIT scrollDiscrete(button == 6 ? -120 : button == 7 ? 120 : 0, button == 4 ? -120 : button == 5 ? 120 : 0);
        }
        break;
    case 8:
        Q_EMIT mouseButton(BTN_SIDE, pressed);
        break;
    case 9:
        Q_EMIT mouseButton(BTN_EXTRA, pressed);
        break;
    }
}

void X11InputCapture::rawKey(uint32_t keycode, bool pressed)
{
    const int evdev = int(keycode) - 8;
    if (!m_xkbState) {
        Q_EMIT keyEvent(evdev, pressed);
        return;
    }
    xkb_state_update_key(m_xkbState, keycode, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_xkbState, keycode);
    const Qt::KeyboardModifiers modifiers = QXkbCommon::modifiers(m_xkbState, sym);
    const Qt::KeyboardModifiers relevant = modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier);
    if (pressed
        && ((evdev == KEY_X && relevant == Qt::AltModifier) || (evdev == KEY_ESC && relevant == (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)))) {
        Q_EMIT escapeRequested();
        return;
    }
    Q_EMIT keyEvent(evdev, pressed);
    const bool modifier = (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || sym == XKB_KEY_ISO_Level3_Shift || sym == XKB_KEY_Num_Lock;
    if (pressed && !modifier) {
        Q_EMIT key(static_cast<Qt::Key>(QXkbCommon::keysymToQtKey(sym, modifiers)), modifiers, QXkbCommon::lookupStringNoKeysymTransformations(sym));
    }
}

#include "moc_x11inputcapture.cpp"
