cmake_minimum_required(VERSION 3.16)
project(ZephyrFTP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent Svg)

# libssh2 discovery differs by platform: pkg-config is standard on
# Linux/macOS, but Windows builds (via this repo's CI) get libssh2 through
# vcpkg, which ships a CMake config package instead of a .pc file.
# UNVERIFIED: the Windows branch's target name (Libssh2::libssh2) matches
# vcpkg's libssh2 port as of this writing, but hasn't been proven against a
# real Windows build yet — that happens the first time the CI workflow runs.
if(WIN32)
    find_package(Libssh2 CONFIG REQUIRED)
    set(LIBSSH2_TARGET Libssh2::libssh2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH2 REQUIRED IMPORTED_TARGET libssh2)
    set(LIBSSH2_TARGET PkgConfig::LIBSSH2)
endif()

qt_standard_project_setup()

set(SOURCES
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/FilePaneWidget.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/SavedSite.cpp
    src/transfer/TransferManager.cpp
    resources/icons.qrc
    resources/theme.qrc
)

# .rc files are a Windows-only resource-compiler input (embeds app-icon.ico
# as the .exe's native icon, shown in File Explorer/taskbar before the app
# even runs — separate from the Qt runtime window icon set in main.cpp).
# CMake only knows what to do with this on Windows; guarded rather than
# added unconditionally so it's a clean no-op on Linux, not a build error.
if(WIN32)
    list(APPEND SOURCES resources/app-icon.rc)
endif()

set(HEADERS
    src/ui/MainWindow.h
    src/ui/FilePaneWidget.h
    src/ui/FileTreeView.h
    src/ui/ConnectionDialog.h
    src/ui/TransferQueueWidget.h
    src/ui/HostKeyVerifier.h
    src/ui/IconTheme.h
    src/ui/SiteManagerDialog.h
    src/backends/RemoteBackend.h
    src/backends/RemoteEntry.h
    src/backends/LocalBackend.h
    src/backends/SftpBackend.h
    src/backends/SftpCredentials.h
    src/backends/SavedSite.h
    src/transfer/TransferItem.h
    src/transfer/TransferManager.h
)

qt_add_executable(zephyrftp ${SOURCES} ${HEADERS})

# Without this, the linked exe defaults to the console subsystem on
# Windows — qt_add_executable() doesn't set it automatically — and every
# launch pops a blank terminal window alongside the actual Qt UI. Only
# applies to the shipped app: the test targets below stay console apps on
# purpose so their qDebug()/diagnostic output is visible when run
# (they're Linux-only in this repo's actual CI usage anyway, so it's moot
# there, but no reason to change it).
set_target_properties(zephyrftp PROPERTIES WIN32_EXECUTABLE ON)

target_include_directories(zephyrftp PRIVATE src)

target_link_libraries(zephyrftp PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Svg
    ${LIBSSH2_TARGET}
)

# --- smoke test (not part of the shipped app; verifies thread lifecycle) ---
qt_add_executable(smoke-test EXCLUDE_FROM_ALL
    src/smoke_test.cpp
    src/ui/MainWindow.cpp
    src/ui/FilePaneWidget.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/SavedSite.cpp
    src/transfer/TransferManager.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(smoke-test PRIVATE src)
target_link_libraries(smoke-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET}
)

# --- transfer queue functional test (moves a real file end-to-end) ---
qt_add_executable(transfer-queue-test EXCLUDE_FROM_ALL
    src/transfer_queue_test.cpp
    src/ui/MainWindow.cpp
    src/ui/FilePaneWidget.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/SavedSite.cpp
    src/transfer/TransferManager.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(transfer-queue-test PRIVATE src)
target_link_libraries(transfer-queue-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET}
)

# --- SiteStore persistence test (pure QCoreApplication, no GUI needed) ---
qt_add_executable(site-store-test EXCLUDE_FROM_ALL
    src/site_store_test.cpp
    src/backends/SavedSite.cpp
)
target_include_directories(site-store-test PRIVATE src)
target_link_libraries(site-store-test PRIVATE Qt6::Core)
