/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>

#include <memory>

class Sink
{
public:
    virtual ~Sink() = default;
    virtual void write(const QByteArray &frame) = 0;
    virtual bool inUse() = 0;
};

namespace PipeWireSink
{
std::unique_ptr<Sink> open(const QString &name, const QString &description, QSize size);
}
