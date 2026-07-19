cmake_minimum_required(VERSION 3.20)
project(mailo VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Sql WebEngineQuick)
find_package(KPim6IMAP REQUIRED)
find_package(KPim6Mime REQUIRED)
find_package(KPim6SMTP REQUIRED)
find_package(Qt6Keychain REQUIRED)

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

target_link_libraries(mailo PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Sql Qt6::WebEngineQuick
    KPim6::IMAP
    KPim6::Mime
    KPim6::SMTP
    Qt6Keychain::Qt6Keychain
)

install(TARGETS mailo RUNTIME DESTINATION bin)

qt_add_executable(viewertest tests/viewertest.cpp src/viewersecurity.cpp src/viewersecurity.h)
target_link_libraries(viewertest PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::WebEngineQuick)
