cmake_minimum_required(VERSION 3.16)
project(ZephyrFTP VERSION 0.2.15 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent Svg)

# libssh2 discovery differs by toolchain, not just by OS: MSVC+vcpkg
# (this repo's current CI) ships a CMake config package instead of a .pc
# file, but MinGW's libssh2 — both native mingw-w64 and Fedora's
# mingw64-cross toolchain used for local cross-compilation — ships a
# standard .pc file same as Linux/macOS, discoverable via pkg-config found
# in the mingw sysroot. CMake sets MINGW true for any GCC-targeting-Windows
# toolchain regardless of host, which is exactly the split needed here.
if(WIN32 AND NOT MINGW)
    find_package(Libssh2 CONFIG REQUIRED)
    set(LIBSSH2_TARGET Libssh2::libssh2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH2 REQUIRED IMPORTED_TARGET libssh2)
    set(LIBSSH2_TARGET PkgConfig::LIBSSH2)
endif()

# CredentialStore's OS-keychain backend: the Win32 Credential Manager
# API (wincred.h) on Windows — part of advapi32, always present in the
# Windows SDK / mingw sysroot, no separate discovery needed — or
# libsecret (pkg-config) everywhere else. WIN32 covers both native MSVC
# and MinGW-cross builds here (unlike the libssh2 split above, both
# genuinely want the real Win32 API, not libsecret, so no MINGW
# special-casing is needed).
if(WIN32)
    set(CREDENTIAL_STORE_TARGET advapi32)
else()
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    set(CREDENTIAL_STORE_TARGET PkgConfig::LIBSECRET)
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
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/FtpBackend.cpp
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/FolderEnumerator.cpp
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
    src/ui/CertificateVerifier.h
    src/ui/IconTheme.h
    src/ui/SiteManagerDialog.h
    src/backends/RemoteBackend.h
    src/backends/RemoteEntry.h
    src/backends/LocalBackend.h
    src/backends/SftpBackend.h
    src/backends/SftpCredentials.h
    src/backends/SavedSite.h
    src/backends/CredentialStore.h
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

# Makes the version set in this file's project() declaration above
# actually reach the compiled binary — CMake's PROJECT_VERSION has no
# effect on the C++ source without being wired through explicitly like
# this. Used by MainWindow for the window title and About dialog.
target_compile_definitions(zephyrftp PRIVATE APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(zephyrftp PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Svg
    ${LIBSSH2_TARGET}
    ${CREDENTIAL_STORE_TARGET}
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
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/FtpBackend.cpp   # MainWindow.cpp constructs one — see startConnection()
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(smoke-test PRIVATE src)
target_compile_definitions(smoke-test PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(smoke-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${CREDENTIAL_STORE_TARGET}
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
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/FtpBackend.cpp   # MainWindow.cpp constructs one — see startConnection()
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(transfer-queue-test PRIVATE src)
target_compile_definitions(transfer-queue-test PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(transfer-queue-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- SiteStore persistence test (pure QCoreApplication, no GUI needed) ---
qt_add_executable(site-store-test EXCLUDE_FROM_ALL
    src/site_store_test.cpp
    src/backends/SavedSite.cpp
)
target_include_directories(site-store-test PRIVATE src)
target_link_libraries(site-store-test PRIVATE Qt6::Core)

# --- Navigation history test (back/forward/up, real LocalBackend + real temp dirs) ---
qt_add_executable(navigation-test EXCLUDE_FROM_ALL
    src/navigation_test.cpp
    src/ui/FilePaneWidget.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(navigation-test PRIVATE src)
target_link_libraries(navigation-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg)

# --- Transfer pause/resume orchestration test (fake backend, no real server needed) ---
qt_add_executable(transfer-pause-test EXCLUDE_FROM_ALL
    src/transfer_pause_test.cpp
    src/ui/FilePaneWidget.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(transfer-pause-test PRIVATE src)
target_link_libraries(transfer-pause-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg)

# --- File management operations test (delete/rename/create, real LocalBackend + real temp dirs) ---
qt_add_executable(file-operations-test EXCLUDE_FROM_ALL
    src/file_operations_test.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(file-operations-test PRIVATE src)
target_link_libraries(file-operations-test PRIVATE Qt6::Core)

# --- Whole-folder transfer test (recursive enumeration + mirrored dirs, real LocalBackend) ---
qt_add_executable(folder-transfer-test EXCLUDE_FROM_ALL
    src/folder_transfer_test.cpp
    src/ui/FilePaneWidget.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(folder-transfer-test PRIVATE src)
target_link_libraries(folder-transfer-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg)

# --- Destination conflict resolution test (Overwrite/Skip, Write Into/Skip, real dialog interaction) ---
qt_add_executable(conflict-resolution-test EXCLUDE_FROM_ALL
    src/conflict_resolution_test.cpp
    src/ui/FilePaneWidget.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(conflict-resolution-test PRIVATE src)
target_link_libraries(conflict-resolution-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg)

# --- FTP directory-listing parser test (MLSD/LIST, realistic sample data — no real server available) ---
# CertificateVerifier.cpp/Widgets are pulled in because FtpBackend.cpp
# includes CertificateVerifier.h (a Q_OBJECT header) for its
# m_certificateVerifier member — same "always pair the two" rule this
# codebase already follows for SftpBackend.cpp + HostKeyVerifier.cpp
# (see CONTRIBUTING.md's AUTOMOC build gotcha). This test never actually
# constructs one (it only calls FtpBackend's static parser functions).
qt_add_executable(ftp-parsing-test EXCLUDE_FROM_ALL
    src/ftp_parsing_test.cpp
    src/backends/FtpBackend.cpp
    src/backends/RemoteBackend.h
    src/ui/CertificateVerifier.cpp
)
target_include_directories(ftp-parsing-test PRIVATE src)
target_link_libraries(ftp-parsing-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# Protocol-selection wiring: ConnectionDialog's protocol choice, and
# SavedSite's persistence of it. Needs Widgets (it constructs a real
# ConnectionDialog) and Svg (IconTheme, pulled in transitively). Note
# RemoteBackend.h listed explicitly — see CONTRIBUTING.md's build gotcha
# on AUTOMOC and Q_OBJECT headers in test targets.
qt_add_executable(protocol-selection-test EXCLUDE_FROM_ALL
    src/protocol_selection_test.cpp
    src/ui/ConnectionDialog.cpp
    src/backends/SavedSite.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(protocol-selection-test PRIVATE src)
target_link_libraries(protocol-selection-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# --- Live-server verification harnesses (NOT part of the ten-target
# self-contained test suite above — these need an external server
# already running, started via tools/local-test-servers/*.sh; see each
# .cpp's own header comment and tools/local-test-servers/README.md) ---
qt_add_executable(verify-sftp-pubkey EXCLUDE_FROM_ALL
    src/verify_sftp_pubkey.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/FolderEnumerator.cpp
)
target_include_directories(verify-sftp-pubkey PRIVATE src)
target_link_libraries(verify-sftp-pubkey PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET}
)

qt_add_executable(verify-sftp-pause-cancel EXCLUDE_FROM_ALL
    src/verify_sftp_pause_cancel.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-sftp-pause-cancel PRIVATE src)
target_link_libraries(verify-sftp-pause-cancel PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET}
)

# --- SSH-daemon diversity: real Dropbear (password auth), not just
# OpenSSH — see src/verify_sftp_vendors.cpp's own header comment for the
# honest accounting of what this does and doesn't prove. Needs
# tools/local-test-servers/start-dropbear.sh's podman container already
# running. ---
qt_add_executable(verify-sftp-vendors EXCLUDE_FROM_ALL
    src/verify_sftp_vendors.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-sftp-vendors PRIVATE src)
target_link_libraries(verify-sftp-vendors PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET}
)

qt_add_executable(verify-ftp-live EXCLUDE_FROM_ALL
    src/verify_ftp_live.cpp
    src/backends/FtpBackend.cpp
    src/backends/RemoteBackend.h
    src/ui/CertificateVerifier.cpp
)
target_include_directories(verify-ftp-live PRIVATE src)
target_link_libraries(verify-ftp-live PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# --- FTPS certificate trust-on-first-use, against the real self-signed
# test server (start-ftps.sh) — see src/verify_ftps_trust.cpp's own
# header comment for the three phases this drives. ---
qt_add_executable(verify-ftps-trust EXCLUDE_FROM_ALL
    src/verify_ftps_trust.cpp
    src/ui/CertificateVerifier.cpp
    src/backends/FtpBackend.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-ftps-trust PRIVATE src)
target_link_libraries(verify-ftps-trust PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent)

# --- Real vendor-diversity verification: actual vsftpd/proftpd, not
# pyftpdlib — see src/verify_ftp_vendors.cpp's own header comment. Needs
# tools/local-test-servers/start-vsftpd.sh/start-proftpd.sh's podman
# containers already running, a different (heavier) precondition than
# verify-ftp-live's, hence its own target. ---
qt_add_executable(verify-ftp-vendors EXCLUDE_FROM_ALL
    src/verify_ftp_vendors.cpp
    src/backends/FtpBackend.cpp
    src/backends/RemoteBackend.h
    src/ui/CertificateVerifier.cpp
)
target_include_directories(verify-ftp-vendors PRIVATE src)
target_link_libraries(verify-ftp-vendors PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
