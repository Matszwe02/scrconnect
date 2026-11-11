/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "applauncherdialog.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace
{
constexpr int RolePackage = Qt::UserRole;
constexpr int RoleSystem = Qt::UserRole + 1;

QPixmap decodeIcon(const QJsonValue &base64)
{
    QPixmap icon;
    icon.loadFromData(QByteArray::fromBase64(base64.toString().toLatin1()), "PNG");
    return icon;
}
}

AppLauncherDialog::AppLauncherDialog(const QString &deviceId)
    : m_search(new QLineEdit(this))
    , m_showSystem(new QCheckBox(i18n("Show system apps"), this))
    , m_list(new QListWidget(this))
    , m_status(new QLabel(i18n("Loading the apps from your phone…"), this))
    , m_plugin(u"org.kde.kdeconnect"_s, u"/modules/kdeconnect/devices/%1/screenmirror"_s.arg(deviceId), u"org.kde.kdeconnect.device.screenmirror"_s)
    , m_cachePath(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/kdeconnect-mirror/apps-%1.json"_s.arg(deviceId))
{
    setWindowTitle(i18n("Mirror a phone app"));
    resize(620, 620);
    m_search->setPlaceholderText(i18n("Search"));
    m_search->setClearButtonEnabled(true);
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(48, 48));
    m_list->setGridSize(QSize(104, 92));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setWordWrap(true);
    m_list->setSortingEnabled(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_search);
    layout->addWidget(m_showSystem);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    connect(m_search, &QLineEdit::textChanged, this, &AppLauncherDialog::filter);
    connect(m_showSystem, &QCheckBox::toggled, this, &AppLauncherDialog::filter);
    connect(m_list, &QListWidget::itemActivated, this, &QDialog::accept);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        for (int i = 0; i < m_list->count(); ++i) {
            if (!m_list->item(i)->isHidden()) {
                m_list->setCurrentRow(i);
                accept();
                return;
            }
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QDBusConnection::sessionBus().connect(m_plugin.service(), m_plugin.path(), m_plugin.interface(), u"appsReceived"_s, this, SLOT(onApps(QString)));
    QDBusConnection::sessionBus().connect(m_plugin.service(), m_plugin.path(), m_plugin.interface(), u"iconsReceived"_s, this, SLOT(onIcons(QString)));
    if (QFile cache(m_cachePath); cache.open(QIODevice::ReadOnly)) {
        m_apps = QJsonDocument::fromJson(cache.readAll()).object();
        populate();
    }
    m_plugin.call(QDBus::NoBlock, u"requestApps"_s, QStringList());
}

QString AppLauncherDialog::package() const
{
    return m_list->currentItem() ? m_list->currentItem()->data(RolePackage).toString() : QString();
}

QString AppLauncherDialog::label() const
{
    return m_list->currentItem() ? m_list->currentItem()->text() : QString();
}

void AppLauncherDialog::onApps(const QString &json)
{
    QJsonObject apps;
    QStringList missingIcons;
    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QString package = value["package"_L1].toString();
        QJsonObject app = m_apps.value(package).toObject();
        app.insert(u"label"_s, value["label"_L1]);
        app.insert(u"system"_s, value["system"_L1]);
        if (!app.contains("icon"_L1)) {
            missingIcons << package;
        }
        apps.insert(package, app);
    }
    m_apps = apps;
    populate();
    save();
    if (!missingIcons.isEmpty()) {
        m_plugin.call(QDBus::NoBlock, u"requestApps"_s, missingIcons);
    }
}

void AppLauncherDialog::onIcons(const QString &json)
{
    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QString package = value["package"_L1].toString();
        if (QJsonObject app = m_apps.value(package).toObject(); !app.isEmpty()) {
            app.insert(u"icon"_s, value["icon"_L1]);
            m_apps.insert(package, app);
            if (const QPixmap icon = decodeIcon(value["icon"_L1]); m_items.contains(package) && !icon.isNull()) {
                m_items[package]->setIcon(icon);
            }
        }
    }
    save();
}

void AppLauncherDialog::populate()
{
    const QString current = package();
    m_list->clear();
    m_items.clear();
    for (auto it = m_apps.constBegin(); it != m_apps.constEnd(); ++it) {
        const QJsonObject app = it.value().toObject();
        const QPixmap icon = decodeIcon(app.value("icon"_L1));
        auto *item = new QListWidgetItem(icon.isNull() ? QIcon::fromTheme(u"application-x-executable"_s) : QIcon(icon), app.value("label"_L1).toString(it.key()), m_list);
        item->setData(RolePackage, it.key());
        item->setData(RoleSystem, app.value("system"_L1).toBool());
        item->setToolTip(it.key());
        m_items.insert(it.key(), item);
    }
    if (m_items.contains(current)) {
        m_list->setCurrentItem(m_items[current]);
    }
    filter();
    m_status->setText(i18np("%1 app", "%1 apps", m_list->count()));
}

void AppLauncherDialog::save() const
{
    QDir().mkpath(QFileInfo(m_cachePath).absolutePath());
    if (QSaveFile cache(m_cachePath); cache.open(QIODevice::WriteOnly)) {
        cache.write(QJsonDocument(m_apps).toJson(QJsonDocument::Compact));
        cache.commit();
    }
}

void AppLauncherDialog::filter()
{
    const QString needle = m_search->text().trimmed();
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const bool matches = item->text().contains(needle, Qt::CaseInsensitive) || item->toolTip().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!matches || (item->data(RoleSystem).toBool() && !m_showSystem->isChecked()));
    }
}

#include "moc_applauncherdialog.cpp"
