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
    readonly property QtObject webcam: WebcamDbusInterfaceFactory.create(device)

    KdeConnectPluginConfig {
        id: config
        deviceId: device
        pluginName: "kdeconnect_webcam"
    }

    Kirigami.FormLayout {
        QQC2.ComboBox {
            id: backend
            Kirigami.FormData.label: i18nd("kdeconnect-plugins", "Cameras appear as:")
            textRole: "text"
            valueRole: "value"
            model: [
                { text: i18nd("kdeconnect-plugins", "Automatic"), value: "auto" },
                { text: i18nd("kdeconnect-plugins", "Webcams for every app"), value: "v4l2loopback" },
                { text: i18nd("kdeconnect-plugins", "PipeWire cameras (no setup)"), value: "pipewire" }
            ]
            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(config.getString("backend", "auto")))
            onActivated: config.set("backend", currentValue)
        }
        RowLayout {
            Kirigami.FormData.label: i18nd("kdeconnect-plugins", "Webcams for every app:")
            visible: backend.currentValue !== "pipewire"
            QQC2.Label {
                text: webcam.virtualCamerasReady ? i18nd("kdeconnect-plugins", "Ready") : i18nd("kdeconnect-plugins", "Need a one-time setup")
            }
            QQC2.Button {
                visible: !webcam.virtualCamerasReady
                icon.name: "system-run"
                text: i18nd("kdeconnect-plugins", "Set Up…")
                onClicked: webcam.setupVirtualCameras()
            }
        }
        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: i18nd("kdeconnect-plugins", "Browsers list PipeWire cameras only when told to: in Chrome, Brave and other Chromium browsers at chrome://flags/#enable-webrtc-pipewire-camera, in Firefox with media.webrtc.camera.allow-pipewire in about:config. Webcams for every app need no such setting.")
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18nd("kdeconnect-plugins", "Picture")
        }
        Repeater {
            model: [
                { key: "front", title: i18nd("kdeconnect-plugins", "Front camera:") },
                { key: "back", title: i18nd("kdeconnect-plugins", "Back camera:") }
            ]
            delegate: RowLayout {
                Kirigami.FormData.label: modelData.title
                QQC2.ComboBox {
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { text: i18nd("kdeconnect-plugins", "Not rotated"), value: 0 },
                        { text: i18nd("kdeconnect-plugins", "Rotated right"), value: 90 },
                        { text: i18nd("kdeconnect-plugins", "Upside down"), value: 180 },
                        { text: i18nd("kdeconnect-plugins", "Rotated left"), value: 270 }
                    ]
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(config.getInt(modelData.key + "Rotation", 0)))
                    onActivated: config.set(modelData.key + "Rotation", currentValue)
                }
                QQC2.CheckBox {
                    text: i18nd("kdeconnect-plugins", "Mirrored")
                    checked: config.getBool(modelData.key + "Mirror", false)
                    onToggled: config.set(modelData.key + "Mirror", checked)
                }
            }
        }

        QQC2.Label {
            Layout.maximumWidth: Kirigami.Units.gridUnit * 25
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: i18nd("kdeconnect-plugins", "A camera runs on the phone only while an app on this computer uses it. Changes apply once no app uses the cameras. Needs Android 12 or newer and the ADB setup on the phone.")
        }
    }
}
