/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QString>
#include <QTemporaryFile>

/**
 * Plasma on Wayland: places the window under the pointer, lets it follow, and calls back when the pointer dragging it
 * reaches the phone's screen edge, which a Wayland window cannot do itself.
 */
class KWinScript
{
public:
    static bool isAvailable();

    bool load(bool followPointer, int phoneEdge, const QString &callbackPath, const QString &callbackInterface, const QString &callbackMethod);
    ~KWinScript();

private:
    QString m_name;
    QTemporaryFile m_file;
};
