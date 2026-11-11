/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QDBusInterface>
#include <QDialog>
#include <QHash>
#include <QJsonObject>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

class AppLauncherDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AppLauncherDialog(const QString &deviceId);

    QString package() const;
    QString label() const;

private Q_SLOTS:
    void onApps(const QString &json);
    void onIcons(const QString &json);

private:
    void filter();
    void populate();
    void save() const;

    QLineEdit *m_search;
    QCheckBox *m_showSystem;
    QListWidget *m_list;
    QLabel *m_status;
    QDBusInterface m_plugin;
    QString m_cachePath;
    QJsonObject m_apps;
    QHash<QString, QListWidgetItem *> m_items;
};
