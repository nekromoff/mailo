cmake_minimum_required(VERSION 3.20)
project(mailo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Debian policy wants 0755 on packaged directories; without this CMake creates
# them with the build user's umask (0775 here) and lintian complains.
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Sql WebEngineQuick)
find_package(KPim6IMAP REQUIRED)
find_package(KPim6Mime REQUIRED)
find_package(KPim6SMTP REQUIRED)
find_package(Qt6Keychain REQUIRED)
# Attachment payloads are stored compressed on disk (see attachmentstore.cpp).
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(mailo
    src/main.cpp
    src/mailclient.cpp
    src/mailclient.h
    src/foldermodel.cpp
    src/foldermodel.h
    src/messagelistmodel.cpp
    src/messagelistmodel.h
    src/mailstore.cpp
    src/mailstore.h
    src/attachmentstore.cpp
    src/attachmentstore.h
    src/documenthandler.cpp
    src/documenthandler.h
    src/oauthhelper.cpp
    src/oauthhelper.h
    src/viewersecurity.cpp
    src/viewersecurity.h
)

set_source_files_properties(src/qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
set_source_files_properties(src/qml/AccountSheet.qml PROPERTIES QT_RESOURCE_ALIAS AccountSheet.qml)
set_source_files_properties(src/qml/MessageViewer.qml PROPERTIES QT_RESOURCE_ALIAS MessageViewer.qml)
set_source_files_properties(src/qml/ComposeSheet.qml PROPERTIES QT_RESOURCE_ALIAS ComposeSheet.qml)

qt_add_qml_module(mailo
    URI Mailo
    VERSION 1.0
    QML_FILES
        src/qml/Main.qml
        src/qml/AccountSheet.qml
        src/qml/MessageViewer.qml
        src/qml/ComposeSheet.qml
)

# Compile ABOUT.md into the binary (shown in Settings → About). Editing
# ABOUT.md and rebuilding always bakes the new text in — it is never read
# from disk at runtime.
qt_add_resources(mailo "about"
    PREFIX "/"
    FILES ABOUT.md
)

target_link_libraries(mailo PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Sql Qt6::WebEngineQuick
    KPim6::IMAP
    KPim6::Mime
    KPim6::SMTP
    Qt6Keychain::Qt6Keychain
    PkgConfig::ZSTD
)

target_compile_definitions(mailo PRIVATE MAILO_VERSION="${PROJECT_VERSION}")

install(TARGETS mailo RUNTIME DESTINATION bin)

# Desktop integration files — installed into the standard XDG locations so
# linuxdeploy (and normal system installs) pick them up for packaging.
include(GNUInstallDirs)
install(FILES packaging/org.mailo.Mailo.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/org.mailo.Mailo.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/org.mailo.Mailo.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Debian has no "License" control field — the licence and copyright holder
# belong in the DEP-5 copyright file under share/doc/<package>, alongside a
# gzipped changelog (mandatory for a native package).
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/changelog.gz
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/changelog
            ${CMAKE_CURRENT_BINARY_DIR}/changelog
    COMMAND gzip -9nf ${CMAKE_CURRENT_BINARY_DIR}/changelog
    DEPENDS packaging/changelog
    COMMENT "Compressing Debian changelog"
    VERBATIM)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mailo.1.gz
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/mailo.1
            ${CMAKE_CURRENT_BINARY_DIR}/mailo.1
    COMMAND gzip -9nf ${CMAKE_CURRENT_BINARY_DIR}/mailo.1
    DEPENDS packaging/mailo.1
    COMMENT "Compressing man page"
    VERBATIM)
add_custom_target(mailo-docs ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/changelog.gz
            ${CMAKE_CURRENT_BINARY_DIR}/mailo.1.gz)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mailo.1.gz
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

install(FILES packaging/copyright
              ${CMAKE_CURRENT_BINARY_DIR}/changelog.gz
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/mailo)

# Debian packaging: `cmake --build build && (cd build && cpack -G DEB)`.
# Shared-library Depends are computed by dpkg-shlibdeps; the extra Depends
# below are runtime-only QML/data packages the linker can't see (QML modules
# are loaded by name, the org.kde.desktop style is forced in main.cpp, and
# icons come from the Breeze theme).
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME mailo)
set(CPACK_PACKAGE_VENDOR "Daniel Duris")
set(CPACK_PACKAGE_CONTACT "Daniel Duris <dusoft@staznosti.sk>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight IMAP mail client for KDE")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/nekromoff/mailo")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Mailo is an IMAP mail client focused on speed and safe message viewing.
It caches headers, bodies and the folder tree in SQLite for instant,
offline-capable browsing and full-text search, and renders HTML mail in a
sandboxed QtWebEngine view that blocks all remote content until you opt in
per message.

Licensed under the LGPL-3.0-or-later; see /usr/share/doc/mailo/copyright.")
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_PACKAGE_SECTION mail)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "\
qml6-module-org-kde-kirigami, \
qml6-module-org-kde-qqc2desktopstyle, \
breeze-icon-theme, \
qml6-module-qtcore, \
qml6-module-qtquick-controls, \
qml6-module-qtquick-layouts, \
qml6-module-qtquick-dialogs, \
qml6-module-qtwebengine")
include(CPack)

qt_add_executable(attachmenttest tests/attachmenttest.cpp
    src/attachmentstore.cpp src/attachmentstore.h)
target_link_libraries(attachmenttest PRIVATE Qt6::Core PkgConfig::ZSTD)

qt_add_executable(viewertest tests/viewertest.cpp src/viewersecurity.cpp src/viewersecurity.h)
target_link_libraries(viewertest PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::WebEngineQuick)
