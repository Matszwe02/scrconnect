/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusInterface>
#include <QDBusReply>

#include "applauncherdialog.h"
#include "mirrorwindow.h"

using namespace Qt::StringLiterals;

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(u"kdeconnect-mirror"_s);
    app.setDesktopFileName(u"org.kde.kdeconnect.mirror"_s);
    KLocalizedString::setApplicationDomain("kdeconnect-plugins");

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Show your phone's screen or a single phone app in a window"));
    parser.addHelpOption();
    parser.addOption({u"device"_s, i18n("Device id (default: the first connected phone)"), u"id"_s});
    parser.addOption({u"app"_s, i18n("Package name of the app to show, e.g. org.mozilla.firefox"), u"package"_s});
    parser.addOption({u"label"_s, i18n("App name for the window title"), u"text"_s});
    parser.addOption({u"screen"_s, i18n("Show the whole screen")});
    parser.process(app);

    MirrorOptions options{parser.value(u"device"_s), parser.value(u"app"_s), parser.value(u"label"_s)};
    if (options.deviceId.isEmpty()) {
        QDBusInterface daemon(u"org.kde.kdeconnect"_s, u"/modules/kdeconnect"_s, u"org.kde.kdeconnect.daemon"_s);
        const QDBusReply<QStringList> devices = daemon.call(u"devices"_s, true, true);
        if (!devices.isValid() || devices.value().isEmpty()) {
            qCritical("No connected phone");
            return 1;
        }
        options.deviceId = devices.value().constFirst();
    }
    if (options.packageName.isEmpty() && !parser.isSet(u"screen"_s)) {
        AppLauncherDialog picker(options.deviceId);
        if (picker.exec() != QDialog::Accepted || picker.package().isEmpty()) {
            return 0;
        }
        options.packageName = picker.package();
        options.label = picker.label();
    }

    MirrorWindow window(options);
    window.show();
    window.start();
    return app.exec();
}
