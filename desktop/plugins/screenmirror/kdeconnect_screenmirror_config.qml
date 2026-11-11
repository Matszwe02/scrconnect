/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kdeconnect

Kirigami.ScrollablePage {
    property string device

    KdeConnectPluginConfig {
        id: config
        deviceId: device
        pluginName: "kdeconnect_screenmirror"
    }

    Kirigami.FormLayout {
        QQC2.CheckBox {
            Kirigami.FormData.label: i18nd("kdeconnect-plugins", "Phone screen:")
            text: i18nd("kdeconnect-plugins", "Turn off while the whole screen is mirrored")
            checked: config.getBool("screenOffScreen", false)
            onToggled: config.set("screenOffScreen", checked)
        }
        QQC2.CheckBox {
            text: i18nd("kdeconnect-plugins", "Turn off while app windows are open")
            checked: config.getBool("screenOffApps", false)
            onToggled: config.set("screenOffApps", checked)
        }
        QQC2.CheckBox {
            text: i18nd("kdeconnect-plugins", "Lock the phone when a window closes")
            checked: config.getBool("lockOnClose", false)
            onToggled: config.set("lockOnClose", checked)
        }
        QQC2.Label {
            Kirigami.FormData.label: i18nd("kdeconnect-plugins", "Sound:")
            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            wrapMode: Text.WordWrap
            text: i18nd("kdeconnect-plugins", "An app window takes only its own app's sound (Android 13 and newer; before, the whole phone's), the whole screen takes all of it.")
        }
        QQC2.CheckBox {
            text: i18nd("kdeconnect-plugins", "Play it on this computer instead of the phone")
            checked: config.getBool("audio", true)
            onToggled: config.set("audio", checked)
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nd("kdeconnect-plugins", "In a mirror window")
        }
        GridLayout {
            Kirigami.FormData.label: i18nd("kdeconnect-plugins", "Shortcuts:")
            columns: 2
            columnSpacing: Kirigami.Units.largeSpacing
            Repeater {
                model: [
                    i18nd("kdeconnect-plugins", "Right click"), i18nd("kdeconnect-plugins", "Back"),
                    i18nd("kdeconnect-plugins", "Middle click"), i18nd("kdeconnect-plugins", "Home"),
                    "Alt+B · Alt+H · Alt+S", i18nd("kdeconnect-plugins", "Back · Home · Recent apps"),
                    "Alt+N · Alt+Shift+N", i18nd("kdeconnect-plugins", "Open · close the notifications"),
                    "Ctrl+O · Ctrl+Shift+O", i18nd("kdeconnect-plugins", "Phone screen off · on"),
                    "Ctrl+V", i18nd("kdeconnect-plugins", "Paste this computer's clipboard"),
                    i18nd("kdeconnect-plugins", "Ctrl+right click"), i18nd("kdeconnect-plugins", "Menu: phone screen off")
                ]
                delegate: QQC2.Label {
                    text: modelData
                    font.bold: index % 2 === 0
                }
            }
        }
    }
}
