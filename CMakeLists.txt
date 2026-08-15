cmake_minimum_required(VERSION 3.16)
project(ZephyrFTP VERSION 0.7.29 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent Svg Test)

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

# OpenSSL discovery mirrors the libssh2 split immediately above, same
# reasoning: MSVC+vcpkg ships a CMake config package, while MinGW
# (native and Fedora's mingw64-cross toolchain) ships a standard .pc
# file discoverable via pkg-config, same as Linux. Needed directly (not
# just transitively through Qt) by FtpTlsSocket.cpp — FTPS's raw-OpenSSL
# TLS layer, which exists because QSslSocket's public API can't drive
# the classic TLS-1.2 session reuse a genuinely strict FTPS server can
# require; see FtpTlsSocket.h's own doc comment.
if(WIN32 AND NOT MINGW)
    find_package(OpenSSL REQUIRED)
    set(OPENSSL_TARGET OpenSSL::SSL)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
    set(OPENSSL_TARGET PkgConfig::OPENSSL)
endif()

# CredentialStore's OS-keychain backend: the Win32 Credential Manager
# API (wincred.h) on Windows — part of advapi32, always present in the
# Windows SDK / mingw sysroot, no separate discovery needed — Keychain
# Services (Security.framework) on macOS, or libsecret (pkg-config)
# everywhere else. WIN32 covers both native MSVC and MinGW-cross builds
# here (unlike the libssh2 split above, both genuinely want the real
# Win32 API, not libsecret, so no MINGW special-casing is needed).
if(WIN32)
    set(CREDENTIAL_STORE_TARGET advapi32)
elseif(APPLE)
    set(CREDENTIAL_STORE_TARGET "-framework Security" "-framework CoreFoundation")
else()
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    set(CREDENTIAL_STORE_TARGET PkgConfig::LIBSECRET)
endif()

qt_standard_project_setup()

set(SOURCES
    src/main.cpp
    src/AppSettings.cpp
    src/ui/MainWindow.cpp
    src/backends/QuickConnectParser.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/TransferQueueTable.cpp
    src/ui/CommandsPaneWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/EditSessionManager.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/ChecksumVerifier.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    src/ui/CompareDeleteConfirmDialog.cpp
    src/ui/CompareDialog.cpp
    src/cli/ScriptParser.cpp
    src/cli/ScriptRunner.cpp
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

# .icns is the macOS analog of the .rc/.ico handling above — the app
# bundle's own icon, shown in Finder/Dock. Unlike every other icon
# asset in this repo, it's never committed: iconutil (the only tool
# that produces .icns) only exists on macOS, so this file is generated
# fresh by the macOS CI job itself, right before this configure step
# runs (see build.yml's build-macos job). Guarded on APPLE so a local
# non-macOS configure doesn't fail looking for a file that was never
# generated.
if(APPLE)
    list(APPEND SOURCES resources/icons/app-icon.icns)
    set_source_files_properties(resources/icons/app-icon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
endif()

set(HEADERS
    src/AppSettings.h
    src/ui/MainWindow.h
    src/ui/FilePaneWidget.h
    src/ui/FileTreeView.h
    src/ui/ConnectionDialog.h
    src/ui/TransferQueueWidget.h
    src/ui/CommandsPaneWidget.h
    src/ui/HostKeyVerifier.h
    src/ui/CertificateVerifier.h
    src/ui/IconTheme.h
    src/ui/SiteManagerDialog.h
    src/ui/PreferencesDialog.h
    src/ui/EditSessionManager.h
    src/ui/PermissionsDialog.h
    src/backends/RemoteBackend.h
    src/backends/RemoteEntry.h
    src/backends/LocalBackend.h
    src/backends/SftpBackend.h
    src/backends/SftpCredentials.h
    src/backends/SavedSite.h
    src/backends/CredentialStore.h
    src/transfer/TransferItem.h
    src/transfer/TransferManager.h
    src/transfer/TransferQueueStore.h
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
    ${OPENSSL_TARGET}
    ${CREDENTIAL_STORE_TARGET}
)

# --- Linux distro packaging (.deb/.rpm via CPack) ---
#
# Windows has no equivalent block here: that platform is distributed as
# a plain zip (see build.yml's Windows job) with its own separate
# collect-win-runtime.sh DLL-bundling step, nothing CPack is involved in.
# macOS has its own separate block below — UNIX AND NOT APPLE, not the
# old bare NOT WIN32, since macOS is also NOT WIN32 and this block's
# .desktop file / DEB;RPM generator make no sense there. That gap
# existed unnoticed until this project actually had a macOS target to
# expose it — nothing here is new caution, just a real, previously-
# unreachable bug being fixed as part of that build's own CMake setup.
#
# install() rules below are what CPack actually packages — a real
# system install layout (bin/, share/applications/, share/icons/), not
# just the bare binary the existing plain tarball ships (see
# build.yml's "Assemble deployable folder" step) . Both distribution
# forms coexist on purpose: the tarball needs no install and no root,
# the .deb/.rpm integrate properly with a desktop environment's app
# launcher/icon theme in exchange for actually installing.
if(UNIX AND NOT APPLE)
    install(TARGETS zephyrftp RUNTIME DESTINATION bin)
    install(FILES resources/zephyrftp.desktop DESTINATION share/applications)

    # hicolor icon theme convention: one file per resolution, all named
    # identically, under share/icons/hicolor/<size>x<size>/apps/ — this
    # is what desktop environments actually look up by the .desktop
    # file's bare "Icon=zephyrftp" key (no path, no extension). Reuses
    # the exact PNGs tools/generate_app_icon.cpp already produces for
    # the Windows .ico and the in-app window icon — one source of icon
    # truth, not a separate packaging-only copy.
    foreach(size 16 32 48 64 128 256)
        install(FILES resources/icons/app-icon-${size}.png
                DESTINATION share/icons/hicolor/${size}x${size}/apps
                RENAME zephyrftp.png)
    endforeach()

    # Scalable companion to the fixed-size PNGs above — Flathub's own
    # guidelines prefer an SVG icon where available, and appstreamcli
    # compose (part of the Flatpak build) picks it up automatically.
    # wind.svg is the same source tools/generate_app_icon.cpp already
    # renders the PNGs from, not a separate icon.
    install(FILES resources/icons/wind.svg
            DESTINATION share/icons/hicolor/scalable/apps
            RENAME zephyrftp.svg)

    # AppStream metainfo — read by software centers (GNOME Software, KDE
    # Discover) for both this native package and the Flatpak build later;
    # share/metainfo/ is the standard location either way. Deliberately
    # the same packaging-agnostic file the Flatpak manifest also
    # consumes via rename-appdata-file — see that file's own header
    # comment for why it isn't named after a reverse-DNS app-id here.
    install(FILES resources/zephyrftp.metainfo.xml DESTINATION share/metainfo)

    # /usr, not CMAKE_INSTALL_PREFIX's default /usr/local — a real
    # system package (unlike a from-source `make install`) belongs at
    # the FHS path a distro's own packages use, so the .desktop
    # file/icon actually get found by the desktop environment without
    # each user needing /usr/local in their icon theme search path.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    set(CPACK_PACKAGE_NAME "zephyrftp")
    set(CPACK_PACKAGE_VENDOR "Bad Cluster")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dual-pane SFTP/FTP client")
    # CPACK_PACKAGE_DESCRIPTION_SUMMARY alone is enough for the .deb's
    # one-line Description field, but the RPM generator's long
    # description falls back to a generic CPack-authored placeholder
    # ("This is an installer created using CPack...") without this set
    # explicitly — confirmed directly: a first real .rpm built without
    # it actually shipped that placeholder text as the package
    # description, not caught by only inspecting the .deb.
    set(CPACK_PACKAGE_DESCRIPTION
        "Browse your local files and a remote server side by side, then \
drag, drop, or double-click to move things between them. SFTP with \
password or private-key login, host-key verification, a real transfer \
queue with pause/resume, and whole-folder transfers.")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/arelas/zephyrftp")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    # No dedicated project email exists (a deliberate choice, not an
    # oversight) — points at the GitHub repo (Issues) instead of
    # inventing or exposing a personal address.
    set(CPACK_PACKAGE_CONTACT "ZephyrFTP Maintainers <https://github.com/arelas/zephyrftp>")

    set(CPACK_GENERATOR "DEB;RPM")

    # SHLIBDEPS runs dpkg-shlibdeps against the actual linked binary to
    # compute real runtime dependencies (exact Qt6/libssh2/libsecret
    # package names+versions this distro's own linker resolved to) —
    # deliberately not a hand-maintained list, which would drift from
    # what the binary actually needs and silently go stale.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

    # RPM's equivalent of SHLIBDEPS — rpmbuild's own automatic
    # dependency scanner, on by default, set explicitly here so the
    # intent (real scanned deps, not a hand list) reads the same way
    # the Debian block above does.
    set(CPACK_RPM_PACKAGE_AUTOREQPROV YES)
    set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
    # The generic CPACK_PACKAGE_DESCRIPTION above turned out NOT to be
    # enough for RPM specifically — confirmed directly by rebuilding and
    # re-inspecting: the placeholder text still shipped with only that
    # set. The RPM generator apparently needs its own
    # CPACK_RPM_PACKAGE_DESCRIPTION, unlike CPACK_RPM_PACKAGE_LICENSE/
    # _GROUP/etc., which do fall back to their generic CPACK_PACKAGE_*
    # equivalents.
    set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")

    include(CPack)
endif()

# --- macOS app bundle (.dmg packaging happens in the CI workflow
# itself, NOT via CPack — see the note below) ---
#
# A real .app bundle, not the bare-binary tarball approach Linux's
# plain tarball uses — Finder/Dock/Launchpad all expect one, and an
# unbundled Qt executable dropped directly into /Applications wouldn't
# find its own Info.plist-declared icon or behave like a normal Mac
# app. MACOSX_BUNDLE_ICON_FILE points at the .icns added to SOURCES
# above (generated fresh by the macOS CI job itself before this
# configure step runs, never committed to the repo).
if(APPLE)
    set_target_properties(zephyrftp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "ZephyrFTP"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.arelas.zephyrftp"
        MACOSX_BUNDLE_ICON_FILE "app-icon.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )

    install(TARGETS zephyrftp BUNDLE DESTINATION .)

    # Deliberately NOT using CPack's DragNDrop generator here, despite
    # every other platform in this file using CPack for its own
    # packaging — found the hard way (a real crash report from a real
    # user, v0.7.3): `cpack -G DragNDrop` re-stages the bundle from
    # this file's own `install(TARGETS ... BUNDLE DESTINATION .)` rule,
    # which reconstructs the bundle from CMake's OWN build-time record
    # of what belongs in it (the executable, Info.plist, the
    # MACOSX_PACKAGE_LOCATION-tagged .icns) — not a raw copy of
    # whatever currently sits in the build tree's .app. Since the CI
    # workflow's "Deploy Qt/non-Qt libraries" steps add
    # Contents/Frameworks AFTER this bundle is originally built, CMake
    # has no record of those files ever existing, so CPack's re-staged
    # copy silently drops them — the built app's load commands were
    # already correctly rewritten to point at Contents/Frameworks by
    # that point, so the packaged .dmg looked fine but crashed on
    # launch on every machine but the CI runner. The workflow now
    # builds the .dmg directly from the exact `build/zephyrftp.app`
    # tree the deploy steps modified in place, via `hdiutil`, with no
    # CMake/CPack re-staging step in between to silently drop anything.
endif()

# --- smoke test (not part of the shipped app; verifies thread lifecycle) ---
qt_add_executable(smoke-test EXCLUDE_FROM_ALL
    src/smoke_test.cpp
    src/AppSettings.cpp
    src/ui/MainWindow.cpp
    src/backends/QuickConnectParser.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/TransferQueueTable.cpp
    src/ui/CommandsPaneWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/EditSessionManager.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp   # MainWindow.cpp constructs one — see startConnection()
    src/backends/FtpTlsSocket.cpp
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/ChecksumVerifier.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    src/ui/CompareDialog.cpp
    src/ui/CompareDeleteConfirmDialog.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(smoke-test PRIVATE src)
target_compile_definitions(smoke-test PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(smoke-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${OPENSSL_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- Checksum verification test: ChecksumVerifier's match/mismatch/
# busy-backend-queuing/cancellation/hidden-from-UI behavior (see
# src/checksum_verification_test.cpp's own header comment). Full
# dependency list cloned from smoke-test above rather than hand-trimmed —
# same "duplicate the whole list, don't chase a minimal subset" precedent
# sync-browsing-test/transfer-queue-test already established for any
# target that needs TransferQueueTable.cpp (which pulls in SiteStore::
# load(), and from there the same wide dependency graph MainWindow itself
# needs).
qt_add_executable(checksum-verification-test EXCLUDE_FROM_ALL
    src/checksum_verification_test.cpp
    src/AppSettings.cpp
    src/ui/MainWindow.cpp
    src/backends/QuickConnectParser.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/TransferQueueTable.cpp
    src/ui/CommandsPaneWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/EditSessionManager.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp   # MainWindow.cpp constructs one — see startConnection()
    src/backends/FtpTlsSocket.cpp
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/ChecksumVerifier.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    src/ui/CompareDialog.cpp
    src/ui/CompareDeleteConfirmDialog.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(checksum-verification-test PRIVATE src)
target_compile_definitions(checksum-verification-test PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(checksum-verification-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${OPENSSL_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- Synchronized browsing test: real MainWindow (same headless
# construction technique as smoke-test above), two LocalBackend panes,
# no server needed — see src/sync_browsing_test.cpp's own header comment.
qt_add_executable(sync-browsing-test EXCLUDE_FROM_ALL
    src/sync_browsing_test.cpp
    src/AppSettings.cpp
    src/ui/MainWindow.cpp
    src/backends/QuickConnectParser.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/TransferQueueTable.cpp
    src/ui/CommandsPaneWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/EditSessionManager.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/ChecksumVerifier.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    src/ui/CompareDialog.cpp
    src/ui/CompareDeleteConfirmDialog.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(sync-browsing-test PRIVATE src)
target_compile_definitions(sync-browsing-test PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(sync-browsing-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${OPENSSL_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- transfer queue functional test (moves a real file end-to-end) ---
qt_add_executable(transfer-queue-test EXCLUDE_FROM_ALL
    src/transfer_queue_test.cpp
    src/AppSettings.cpp
    src/ui/MainWindow.cpp
    src/backends/QuickConnectParser.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/ConnectionDialog.cpp
    src/ui/TransferQueueWidget.cpp
    src/ui/TransferQueueTable.cpp
    src/ui/CommandsPaneWidget.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
    src/ui/IconTheme.cpp
    src/ui/SiteManagerDialog.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/EditSessionManager.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp   # MainWindow.cpp constructs one — see startConnection()
    src/backends/FtpTlsSocket.cpp
    src/backends/SavedSite.cpp
    src/backends/CredentialStore.cpp
    src/transfer/TransferManager.cpp
    src/transfer/ChecksumVerifier.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    src/ui/CompareDialog.cpp
    src/ui/CompareDeleteConfirmDialog.cpp
    resources/icons.qrc
    resources/theme.qrc
    ${HEADERS}
)
target_include_directories(transfer-queue-test PRIVATE src)
target_compile_definitions(transfer-queue-test PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(transfer-queue-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${OPENSSL_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- SiteStore persistence test (pure QCoreApplication, no GUI needed) ---
qt_add_executable(site-store-test EXCLUDE_FROM_ALL
    src/site_store_test.cpp
    src/backends/SavedSite.cpp
)
target_include_directories(site-store-test PRIVATE src)
target_link_libraries(site-store-test PRIVATE Qt6::Core)

# --- Connection history store test (pure QCoreApplication, no GUI
# needed) — same shape as site-store-test above, see
# src/connection_history_test.cpp's own header comment.
qt_add_executable(connection-history-test EXCLUDE_FROM_ALL
    src/connection_history_test.cpp
    src/backends/ConnectionHistory.cpp
)
target_include_directories(connection-history-test PRIVATE src)
target_link_libraries(connection-history-test PRIVATE Qt6::Core)

# --- AppSettings persistence test (pure QCoreApplication, no GUI needed) ---
qt_add_executable(app-settings-test EXCLUDE_FROM_ALL
    src/app_settings_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
)
target_include_directories(app-settings-test PRIVATE src)
# CREDENTIAL_STORE_TARGET (not just Qt6::Core, as before) — AppSettings.cpp
# now calls CredentialStore::save()/load()/remove() for the proxy
# password (see AppSettings::setProxyPassword()). Whether the OS
# credential store is actually available/unlocked in this environment
# doesn't matter for what this required-suite test checks (see its own
# comment) — only that settings.json itself never gets a plaintext
# password, which doesn't depend on CredentialStore succeeding.
target_link_libraries(app-settings-test PRIVATE Qt6::Core ${CREDENTIAL_STORE_TARGET})

# --- Bandwidth throttle pacing test: BandwidthThrottle's own math in
# isolation (no server, no backend) — see src/bandwidth_throttle_test.cpp's
# own header comment. Qt6::Core alone, same reasoning as app-settings-test
# above: QCoreApplication only, no widgets/network involved. ---
qt_add_executable(bandwidth-throttle-test EXCLUDE_FROM_ALL
    src/bandwidth_throttle_test.cpp
    src/backends/BandwidthThrottle.cpp
)
target_include_directories(bandwidth-throttle-test PRIVATE src)
target_link_libraries(bandwidth-throttle-test PRIVATE Qt6::Core)

# --- Trust-prompt test: drives the REAL QMessageBox HostKeyVerifier/
# CertificateVerifier pop, same live-dialog technique
# conflict-resolution-test uses (QT_QPA_PLATFORM=offscreen still renders a
# real, drivable QMessageBox) ---
qt_add_executable(trust-prompt-test EXCLUDE_FROM_ALL
    src/trust_prompt_test.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
)
target_include_directories(trust-prompt-test PRIVATE src)
target_link_libraries(trust-prompt-test PRIVATE Qt6::Widgets)

# --- Navigation history test (back/forward/up, real LocalBackend + real temp dirs) ---
qt_add_executable(navigation-test EXCLUDE_FROM_ALL
    src/navigation_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(navigation-test PRIVATE src)
target_link_libraries(navigation-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg Qt6::Test ${CREDENTIAL_STORE_TARGET})

# --- Transfer pause/resume orchestration test (fake backend, no real server needed) ---
qt_add_executable(transfer-pause-test EXCLUDE_FROM_ALL
    src/transfer_pause_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(transfer-pause-test PRIVATE src)
target_link_libraries(transfer-pause-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- File management operations test (delete/rename/create, real LocalBackend + real temp dirs) ---
qt_add_executable(file-operations-test EXCLUDE_FROM_ALL
    src/file_operations_test.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/ui/PermissionsDialog.cpp
)
target_include_directories(file-operations-test PRIVATE src)
# Qt6::Widgets only for PermissionsDialog.cpp's permissionsStringToMode()/
# modeToPermissionsString() free functions — this target was previously
# GUI-free (Qt6::Core alone), and stays GUI-free in spirit: chmod is now
# genuinely one of the "file operations" this test's own name promises
# to cover, and these two functions are real, pure, no-QApplication-
# needed logic worth verifying without pulling in a live PermissionsDialog.
# Qt6::Concurrent also needed now — LocalBackend.cpp's downloadFile()/
# uploadFile() background their actual copy via QtConcurrent::run() (a
# real fix for a live-reported GUI-freeze bug; see LocalBackend.h's own
# doc comment), and this target links LocalBackend.cpp.
target_link_libraries(file-operations-test PRIVATE Qt6::Core Qt6::Widgets Qt6::Concurrent)

# --- Whole-folder transfer test (recursive enumeration + mirrored dirs, real LocalBackend) ---
qt_add_executable(folder-transfer-test EXCLUDE_FROM_ALL
    src/folder_transfer_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(folder-transfer-test PRIVATE src)
target_link_libraries(folder-transfer-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Directory Compare-and-Sync test (diff classification, bottom-up delete ordering, real end-to-end copy) ---
qt_add_executable(compare-sync-test EXCLUDE_FROM_ALL
    src/compare_sync_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(compare-sync-test PRIVATE src)
target_link_libraries(compare-sync-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Scripting/automation test (ScriptParser + ScriptRunner, real LocalBackend, no server or open() needed) ---
# Needs SftpBackend/FtpBackend/SavedSite compiled in even though this
# test never exercises the `open` command (attachRemotePaneForTesting()
# bypasses it) — ScriptRunner.cpp's runOpen() references those symbols
# unconditionally, same reason smoke-test's own source list already
# carries them despite not connecting to a real server either.
qt_add_executable(script-runner-test EXCLUDE_FROM_ALL
    src/script_runner_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/SavedSite.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    src/cli/ScriptParser.cpp
    src/cli/ScriptRunner.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(script-runner-test PRIVATE src)
target_link_libraries(script-runner-test PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${OPENSSL_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- Destination conflict resolution test (Overwrite/Skip, Write Into/Skip, real dialog interaction) ---
qt_add_executable(conflict-resolution-test EXCLUDE_FROM_ALL
    src/conflict_resolution_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(conflict-resolution-test PRIVATE src)
target_link_libraries(conflict-resolution-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

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
    src/backends/BandwidthThrottle.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
    src/ui/CertificateVerifier.cpp
)
target_include_directories(ftp-parsing-test PRIVATE src)
target_link_libraries(ftp-parsing-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network ${OPENSSL_TARGET})

# Protocol-selection wiring: ConnectionDialog's protocol choice, and
# SavedSite's persistence of it. Needs Widgets (it constructs a real
# ConnectionDialog) and Svg (IconTheme, pulled in transitively). Note
# RemoteBackend.h listed explicitly — see CONTRIBUTING.md's build gotcha
# on AUTOMOC and Q_OBJECT headers in test targets.
qt_add_executable(protocol-selection-test EXCLUDE_FROM_ALL
    src/protocol_selection_test.cpp
    src/ui/ConnectionDialog.cpp
    src/backends/SavedSite.cpp
    src/backends/QuickConnectParser.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(protocol-selection-test PRIVATE src)
target_link_libraries(protocol-selection-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# --- Sort/Commands-pane regression test: default folders-first/name-
# descending order, a real numeric header-column sort, entryForRow()'s
# row-independence after that sort, and CommandsPaneWidget/FilePaneWidget's
# commandLogged forwarding (including across a setBackend() swap). Added
# after the ten-target suite above was already an established, documented
# count elsewhere (CLAUDE.md, CONTRIBUTING.md) — self-contained and
# EXCLUDE_FROM_ALL like the ten, but deliberately NOT folded into that
# specific number here; see src/sort_and_commands_test.cpp's own header
# comment for what it covers. Needs CommandsPaneWidget.cpp explicitly
# (the ten above don't, since none of them exercise it directly) and
# RemoteBackend.h explicitly for the same AUTOMOC/Q_OBJECT reason
# CONTRIBUTING.md's build gotcha describes.
qt_add_executable(sort-and-commands-test EXCLUDE_FROM_ALL
    src/sort_and_commands_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/CommandsPaneWidget.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(sort-and-commands-test PRIVATE src)
target_compile_definitions(sort-and-commands-test PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(sort-and-commands-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Remote-to-remote (server-to-server) staged-transfer orchestration
# test: direction/phase assignment, the phase-1 (download-to-temp) ->
# phase-2 (upload-from-temp) transition and m_currentBackend re-pointing,
# temp-file cleanup on success/cancel-mid-either-phase, and retryItem()'s
# phase reset. Two independent fake RemoteBackends (see
# src/remote_to_remote_test.cpp's own header comment) — no real server
# needed. Self-contained and EXCLUDE_FROM_ALL like the ten and
# sort-and-commands-test above, and — same reasoning as that one — kept as
# an explicit twelfth target rather than folded into "the ten" count
# CLAUDE.md/CONTRIBUTING.md document. RemoteBackend.h listed explicitly for
# the same AUTOMOC/Q_OBJECT reason CONTRIBUTING.md's build gotcha describes.
qt_add_executable(remote-to-remote-test EXCLUDE_FROM_ALL
    src/remote_to_remote_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(remote-to-remote-test PRIVATE src)
target_link_libraries(remote-to-remote-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Move (server-side rename between two panes on the same
# filesystem/server) test: TransferManager::moveEligible()'s
# connectionIdentity()-equality guard, moveEntry()/moveFolder()'s
# request-id-correlated dispatch to a backend's moveEntry() (confirming a
# folder move skips FolderEnumerator entirely — exactly one backend call,
# zero listDirectoryForEnumeration() calls), and LocalBackend::moveEntry()
# against real temp files/directories (including the overwrite-an-existing-
# file case). Self-contained and EXCLUDE_FROM_ALL like remote-to-remote-test
# above, same "additional, not folded into the fixed count" treatment — see
# src/move_entry_test.cpp's own header comment and
# CONTRIBUTING.md/ARCHITECTURE.md for the full detail.
qt_add_executable(move-entry-test EXCLUDE_FROM_ALL
    src/move_entry_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(move-entry-test PRIVATE src)
target_link_libraries(move-entry-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Edit-in-place test: EditSessionManager's download -> real
# QFileSystemWatcher-detected save -> re-upload -> re-edit-without-
# re-download -> teardown-deletes-temp-file lifecycle, against a real
# LocalBackend standing in for "the remote" (see src/edit_session_test.cpp's
# own header comment for why that's a legitimate stand-in here, same
# reasoning navigation-test/transfer-pause-test already rely on for
# their own LocalBackend-backed panes). Self-contained and
# EXCLUDE_FROM_ALL like remote-to-remote-test/move-entry-test above,
# same "additional, not folded into the fixed count" treatment.
qt_add_executable(edit-session-test EXCLUDE_FROM_ALL
    src/edit_session_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/ui/EditSessionManager.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(edit-session-test PRIVATE src)
target_link_libraries(edit-session-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Queue persistence test: TransferQueueStore's save/load round trip,
# TransferManager::restorePersistedQueue()'s LocalToLocal-dispatches-
# immediately vs. LocalToRemote/RemoteToLocal-becomes-PendingReconnect
# split, and tryReclaimPendingItems()'s reconnect-matching (including a
# non-matching reconnect leaving an item PendingReconnect, and one
# reconnect claiming several items at once) — against a real LocalBackend
# standing in for "the remote" (see src/queue_persistence_test.cpp's own
# header comment). Self-contained and EXCLUDE_FROM_ALL like
# edit-session-test/move-entry-test above, same "additional, not folded
# into the fixed count" treatment.
qt_add_executable(queue-persistence-test EXCLUDE_FROM_ALL
    src/queue_persistence_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/SavedSite.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(queue-persistence-test PRIVATE src)
target_link_libraries(queue-persistence-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Transfer concurrency test: TransferManager's per-backend-instance
# concurrent scheduling (two items on DIFFERENT backend instances run
# genuinely at the same time; two on the SAME instance still serialize;
# cancelling one concurrently-active item doesn't affect another) — see
# src/transfer_concurrency_test.cpp's own header comment. Self-contained
# and EXCLUDE_FROM_ALL like queue-persistence-test above, same
# "additional, not folded into the fixed count" treatment.
qt_add_executable(transfer-concurrency-test EXCLUDE_FROM_ALL
    src/transfer_concurrency_test.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/backends/LocalBackend.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(transfer-concurrency-test PRIVATE src)
target_link_libraries(transfer-concurrency-test PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${CREDENTIAL_STORE_TARGET})

# --- Live-server verification harnesses (NOT part of the ten-target
# self-contained test suite above — these need an external server
# already running, started via tools/local-test-servers/*.sh; see each
# .cpp's own header comment and tools/local-test-servers/README.md) ---
qt_add_executable(verify-sftp-pubkey EXCLUDE_FROM_ALL
    src/verify_sftp_pubkey.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
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
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-sftp-pause-cancel PRIVATE src)
target_link_libraries(verify-sftp-pause-cancel PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET}
)

# --- Bandwidth throttle against a REAL server — see
# src/verify_bandwidth_throttle_live.cpp's own header comment. Same
# single-server precondition/source set as verify-sftp-pause-cancel
# above (tools/local-test-servers/start-sftp-pubkey.sh already running).
qt_add_executable(verify-bandwidth-throttle-live EXCLUDE_FROM_ALL
    src/verify_bandwidth_throttle_live.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-bandwidth-throttle-live PRIVATE src)
target_link_libraries(verify-bandwidth-throttle-live PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET}
)

# --- Move (server-side rename between two panes on the same connection)
# against a REAL server — see src/verify_sftp_move.cpp's own header
# comment. Unlike every other verify-* target above, this drives
# TransferManager/FilePaneWidget against real SftpBackend connections
# (not just a raw backend), so it needs the same source set
# move-entry-test does, plus SftpBackend/HostKeyVerifier for the real
# connection. Needs tools/local-test-servers/start-sftp-pubkey.sh
# already running.
qt_add_executable(verify-sftp-move EXCLUDE_FROM_ALL
    src/verify_sftp_move.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(verify-sftp-move PRIVATE src)
target_link_libraries(verify-sftp-move PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- Remote-to-remote staged transfer against TWO REAL, independent
# SFTP servers — see src/verify_remote_to_remote_live.cpp's own header
# comment for the specific "not cleanly, repeatably confirmed" gap this
# closes. Same source set as verify-sftp-move above (drives
# TransferManager/FilePaneWidget against real connections). Needs TWO
# tools/local-test-servers/start-sftp-pubkey.sh instances already
# running on different ports.
qt_add_executable(verify-remote-to-remote-live EXCLUDE_FROM_ALL
    src/verify_remote_to_remote_live.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(verify-remote-to-remote-live PRIVATE src)
target_link_libraries(verify-remote-to-remote-live PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${CREDENTIAL_STORE_TARGET}
)

# --- Concurrent transfers against TWO REAL, independent SFTP servers —
# closes the one gap transfer-concurrency-test's own header comment
# names outright: it proves TransferManager's scheduling allows
# concurrent InProgress items, not that two real libssh2 sessions
# actually move bytes over the network at the same time. See
# src/verify_concurrent_transfers_live.cpp's own header comment for the
# full design (serial baseline, then a concurrent 3-item batch, timed
# and compared). Same source set/server setup as
# verify-remote-to-remote-live above.
qt_add_executable(verify-concurrent-transfers-live EXCLUDE_FROM_ALL
    src/verify_concurrent_transfers_live.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(verify-concurrent-transfers-live PRIVATE src)
target_link_libraries(verify-concurrent-transfers-live PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${CREDENTIAL_STORE_TARGET}
)


# --- Move (server-side rename between two panes on the same connection)
# against a REAL FTP server — the FTP counterpart to verify-sftp-move
# above, confirming RNFR/RNTO genuinely relocates a DIRECTORY, not just
# a file, over FTP specifically. See src/verify_ftp_move.cpp's own
# header comment. No SftpBackend/libssh2/HostKeyVerifier needed —
# FtpBackend uses CertificateVerifier instead (only actually exercised
# for FTPS, but still needs linking since FtpBackend.h references the
# type). Needs tools/local-test-servers/start-ftp.sh already running.
qt_add_executable(verify-ftp-move EXCLUDE_FROM_ALL
    src/verify_ftp_move.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/ui/CertificateVerifier.cpp
    src/backends/LocalBackend.cpp
    src/backends/FtpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(verify-ftp-move PRIVATE src)
target_link_libraries(verify-ftp-move PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${OPENSSL_TARGET} ${CREDENTIAL_STORE_TARGET}
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
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-sftp-vendors PRIVATE src)
target_link_libraries(verify-sftp-vendors PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET}
)

# --- SFTP throughput vs. a real, non-loopback remote server — see
# src/verify_sftp_throughput.cpp's own header comment. Needs a real
# externally-provided server (host/user/password via env vars); unlike
# every other harness above, this project can't spin up its own for this
# since the whole point is a real, non-zero round-trip time. ---
qt_add_executable(verify-sftp-throughput EXCLUDE_FROM_ALL
    src/verify_sftp_throughput.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-sftp-throughput PRIVATE src)
target_link_libraries(verify-sftp-throughput PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET}
)

qt_add_executable(verify-ftp-live EXCLUDE_FROM_ALL
    src/verify_ftp_live.cpp
    src/backends/FtpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
    src/ui/CertificateVerifier.cpp
)
target_include_directories(verify-ftp-live PRIVATE src)
target_link_libraries(verify-ftp-live PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network ${OPENSSL_TARGET})

# --- FTPS certificate trust-on-first-use, against the real self-signed
# test server (start-ftps.sh) — see src/verify_ftps_trust.cpp's own
# header comment for the three phases this drives. ---
qt_add_executable(verify-ftps-trust EXCLUDE_FROM_ALL
    src/verify_ftps_trust.cpp
    src/ui/CertificateVerifier.cpp
    src/backends/FtpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-ftps-trust PRIVATE src)
target_link_libraries(verify-ftps-trust PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent ${OPENSSL_TARGET})

# --- Real vendor-diversity verification: actual vsftpd/proftpd, not
# pyftpdlib — see src/verify_ftp_vendors.cpp's own header comment. Needs
# tools/local-test-servers/start-vsftpd.sh/start-proftpd.sh's podman
# containers already running, a different (heavier) precondition than
# verify-ftp-live's, hence its own target. ---
qt_add_executable(verify-ftp-vendors EXCLUDE_FROM_ALL
    src/verify_ftp_vendors.cpp
    src/backends/FtpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/ProxyConnect.cpp
    src/backends/RemoteBackend.h
    src/ui/CertificateVerifier.cpp
)
target_include_directories(verify-ftp-vendors PRIVATE src)
target_link_libraries(verify-ftp-vendors PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network ${OPENSSL_TARGET})

# --- SOCKS5 proxy, real end to end — see src/verify_socks5_proxy.cpp's
# own header comment. Needs tools/local-test-servers/
# start-sftp-pubkey.sh, start-socks5-proxy.sh, start-ftp.sh, and
# start-ftps-trusted.sh all already running (SFTP/FTP/FTPS each proxied
# through the same real SOCKS5 proxy). ---
qt_add_executable(verify-socks5-proxy EXCLUDE_FROM_ALL
    src/verify_socks5_proxy.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-socks5-proxy PRIVATE src)
target_link_libraries(verify-socks5-proxy PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET} ${OPENSSL_TARGET}
)

# --- HTTP CONNECT proxy, real end to end — see
# src/verify_http_connect_proxy.cpp's own header comment. Needs
# tools/local-test-servers/start-sftp-pubkey.sh,
# start-http-connect-proxy.sh, start-ftp.sh, and start-ftps-trusted.sh
# all already running. ---
qt_add_executable(verify-http-connect-proxy EXCLUDE_FROM_ALL
    src/verify_http_connect_proxy.cpp
    src/ui/HostKeyVerifier.cpp
    src/ui/CertificateVerifier.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/RemoteBackend.h
)
target_include_directories(verify-http-connect-proxy PRIVATE src)
target_link_libraries(verify-http-connect-proxy PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Concurrent ${LIBSSH2_TARGET} ${OPENSSL_TARGET}
)

# --- CredentialStore against the REAL OS credential store — see
# src/verify_credential_store.cpp's own header comment. Unlike every
# other verify-* target above, its "external precondition" is a real,
# already-in-use OS credential store rather than a disposable local
# server, which is also why it isn't one of the ten self-contained
# EXCLUDE_FROM_ALL targets despite needing no other setup to run. ---
qt_add_executable(verify-credential-store EXCLUDE_FROM_ALL
    src/verify_credential_store.cpp
    src/backends/CredentialStore.cpp
    src/AppSettings.cpp
)
target_include_directories(verify-credential-store PRIVATE src)
target_link_libraries(verify-credential-store PRIVATE Qt6::Core ${CREDENTIAL_STORE_TARGET})

# Covers the real `open <site-name>` path script-runner-test deliberately
# can't (no OS keyring/live-server dependency there) — see this file's
# own header comment. Needs the same full ScriptRunner dependency set as
# script-runner-test, plus ui/HostKeyVerifier.cpp for its one-time real
# trust-setup phase.
qt_add_executable(verify-script-runner-live EXCLUDE_FROM_ALL
    src/verify_script_runner_live.cpp
    src/AppSettings.cpp
    src/backends/CredentialStore.cpp
    src/ui/FilePaneWidget.cpp
    src/backends/ConnectionHistory.cpp
    src/ui/PermissionsDialog.cpp
    src/ui/FileTreeView.cpp
    src/ui/IconTheme.cpp
    src/ui/HostKeyVerifier.cpp
    src/backends/LocalBackend.cpp
    src/backends/SftpBackend.cpp
    src/backends/BandwidthThrottle.cpp
    src/backends/ProxyConnect.cpp
    src/backends/FtpBackend.cpp
    src/backends/FtpTlsSocket.cpp
    src/backends/SavedSite.cpp
    src/backends/RemoteBackend.h
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueStore.cpp
    src/transfer/FolderEnumerator.cpp
    src/compare/DirectoryComparer.cpp
    src/compare/CompareSyncExecutor.cpp
    src/cli/ScriptParser.cpp
    src/cli/ScriptRunner.cpp
    resources/icons.qrc
    resources/theme.qrc
)
target_include_directories(verify-script-runner-live PRIVATE src)
target_link_libraries(verify-script-runner-live PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Svg ${LIBSSH2_TARGET} ${OPENSSL_TARGET} ${CREDENTIAL_STORE_TARGET}
)
