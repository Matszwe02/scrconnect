/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "kwinscript.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QGuiApplication>

using namespace Qt::StringLiterals;

namespace
{
const QString kKWin = u"org.kde.KWin"_s;

// KWin 6 scripting API: https://develop.kde.org/docs/plasma/kwin/api/
const char kScript[] = R"(
const pid = %1, edge = %2, viewer = "%4", path = "%5", iface = "%6", method = "%7";
let follow = %3;

function screenAt(p) {
    return workspace.screens.find(s => p.x >= s.geometry.x && p.x < s.geometry.x + s.geometry.width
                                       && p.y >= s.geometry.y && p.y < s.geometry.y + s.geometry.height);
}

function distanceToPhoneEdge(p) {
    const screen = screenAt(p);
    if (!screen) {
        return -1;
    }
    const r = screen.geometry;
    let distance, outside;
    if (edge === 2) {
        distance = p.x - r.x; outside = {x: r.x - 1, y: p.y};
    } else if (edge === 4) {
        distance = r.x + r.width - 1 - p.x; outside = {x: r.x + r.width, y: p.y};
    } else if (edge === 1) {
        distance = p.y - r.y; outside = {x: p.x, y: r.y - 1};
    } else {
        distance = r.y + r.height - 1 - p.y; outside = {x: p.x, y: r.y + r.height};
    }
    return screenAt(outside) ? -1 : distance;
}

function watch(window) {
    if (window.pid !== pid || !window.normalWindow) {
        return;
    }
    if (follow) {
        follow = false;
        const place = () => {
            const cursor = workspace.cursorPos;
            const g = window.frameGeometry;
            window.frameGeometry = {x: Math.round(cursor.x - g.width / 2), y: cursor.y - 12, width: g.width, height: g.height};
            workspace.activeWindow = window;
            workspace.slotWindowMove();
        };
        if (window.frameGeometry.width > 1) {
            place();
        } else {
            const once = () => {
                if (window.frameGeometry.width > 1) {
                    window.frameGeometryChanged.disconnect(once);
                    place();
                }
            };
            window.frameGeometryChanged.connect(once);
        }
    }
    if (edge) {
        let armed = false;
        window.interactiveMoveResizeStarted.connect(() => armed = false);
        window.interactiveMoveResizeStepped.connect(() => {
            const distance = distanceToPhoneEdge(workspace.cursorPos);
            if (distance > 100) {
                armed = true;
            } else if (armed && distance >= 0 && distance <= 1) {
                armed = false;
                callDBus(viewer, path, iface, method);
            }
        });
    }
}

workspace.windowList().forEach(watch);
workspace.windowAdded.connect(watch);
)";
}

bool KWinScript::isAvailable()
{
    return QGuiApplication::platformName().startsWith("wayland"_L1) && QDBusConnection::sessionBus().interface()->isServiceRegistered(kKWin);
}

bool KWinScript::load(bool followPointer, int phoneEdge, const QString &callbackPath, const QString &callbackInterface, const QString &callbackMethod)
{
    m_file.setFileTemplate(QDir::tempPath() + u"/kdeconnect-mirror-XXXXXX.js"_s);
    if (!m_file.open()) {
        return false;
    }
    const QString script = QString::fromLatin1(kScript)
                               .arg(QCoreApplication::applicationPid())
                               .arg(phoneEdge)
                               .arg(followPointer ? u"true"_s : u"false"_s)
                               .arg(QDBusConnection::sessionBus().baseService(), callbackPath, callbackInterface, callbackMethod);
    m_file.write(script.toUtf8());
    m_file.flush();

    m_name = u"kdeconnect-mirror-%1"_s.arg(QCoreApplication::applicationPid());
    QDBusInterface scripting(kKWin, u"/Scripting"_s, u"org.kde.kwin.Scripting"_s);
    scripting.call(u"unloadScript"_s, m_name);
    const QDBusReply<int> id = scripting.call(u"loadScript"_s, m_file.fileName(), m_name);
    if (!id.isValid() || id.value() < 0) {
        m_name.clear();
        return false;
    }
    QDBusInterface(kKWin, u"/Scripting/Script%1"_s.arg(id.value()), u"org.kde.kwin.Script"_s).call(u"run"_s);
    return true;
}

KWinScript::~KWinScript()
{
    if (!m_name.isEmpty()) {
        QDBusInterface(kKWin, u"/Scripting"_s, u"org.kde.kwin.Scripting"_s).call(QDBus::NoBlock, u"unloadScript"_s, m_name);
    }
}
