kdeconnect_add_plugin(kdeconnect_shareinputdevices SOURCES shareinputdevicesplugin.cpp inputcapturesession.cpp inputcapturebackend.h)

pkg_check_modules(PKG_libei REQUIRED IMPORTED_TARGET libei-1.0)

target_link_libraries(kdeconnect_shareinputdevices
    kdeconnectcore
    kdeconnectdbusinterfaces
    KF6::I18n
    Qt::GuiPrivate
    PkgConfig::PKG_libei
)

# X11 desktops have no InputCapture portal: pointer barriers instead
pkg_check_modules(X11_CAPTURE IMPORTED_TARGET xcb xcb-xfixes xcb-xinput xcb-randr xkbcommon-x11)
if (X11_CAPTURE_FOUND)
    target_sources(kdeconnect_shareinputdevices PRIVATE x11inputcapture.cpp)
    target_compile_definitions(kdeconnect_shareinputdevices PRIVATE HAVE_X11_BACKEND)
    target_link_libraries(kdeconnect_shareinputdevices PkgConfig::X11_CAPTURE)
endif()

install(FILES "kdeconnect_shareinputdevices_config.qml" DESTINATION ${KDE_INSTALL_DATADIR}/kdeconnect)
