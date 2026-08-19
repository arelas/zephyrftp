; ZephyrFTP Windows installer (NSIS, Modern UI 2).
;
; Deliberately unsigned for now — see README.md's "Current version" note
; on the v0.8.0 feature freeze and CONTRIBUTING.md's Windows section for
; the planned code-signing path (SignPath Foundation or Azure Trusted
; Signing, still an open decision as of this writing). Signing an
; installer built like this needs no changes to this script itself: the
; signing step runs against the finished zephyrftp-windows-x64-setup.exe
; afterward (signtool.exe or the SignPath/Trusted-Signing CI action),
; same as signing any other prebuilt binary.
;
; Expects the exact "dist/" layout build.yml's own "Assemble deployable
; folder" step already produces — zephyrftp.exe, *.dll, and the
; platforms/iconengines/imageformats/styles/tls plugin subdirectories —
; passed in via -DDISTDIR=path\to\dist. Deliberately does NOT hardcode
; the DLL list: collect-win-runtime.sh's whole reason for existing is
; that the transitive DLL closure isn't fixed (it's computed from a real
; objdump import-table walk), so hardcoding names here would silently
; drift out of sync with it. Recursively including everything already
; assembled there instead means this script needs no changes when a new
; dependency shows up.
;
; Two Fedora packages needed to actually compile this, not just one —
; confirmed the hard way, both required:
;   sudo dnf install mingw64-nsis mingw32-nsis
; mingw64-nsis alone ships makensis plus only the amd64-unicode stubs —
; but makensis unconditionally tries to load an x86-unicode stub at
; startup as an internal sanity check, BEFORE it even reads this
; script's own `Target amd64-unicode` directive below or processes any
; command-line flag (confirmed directly: even `makensis /HELP` fails
; identically with no x86 stubs present). mingw32-nsis supplies exactly
; that x86 stub, purely to satisfy this startup check — nothing in this
; script or the actual installer output ends up 32-bit.
;
; Build with (from the repo root; DISTDIR/LICENSEFILE/ICONFILE/OUTFILE as
; ABSOLUTE paths deliberately — NSIS resolves relative File/Icon/OutFile
; paths against the .nsi script's own directory, not the shell's current
; directory, which is easy to get backwards when this is invoked from a
; different working directory than a human typing the command by hand
; would use; passing absolute paths sidesteps that ambiguity entirely
; rather than relying on it). Note `-D`, not `/D` — confirmed directly
; that this Linux build of makensis parses a leading `/` as the start of
; an absolute path (so `/DVERSION=...` fails with "Can't open script
; /DVERSION=..."), unlike the Windows convention some NSIS docs show:
;   makensis -DVERSION=0.8.0 -DDISTDIR="$(pwd)/dist" \
;       -DLICENSEFILE="$(pwd)/LICENSE" -DICONFILE="$(pwd)/resources/icons/app-icon.ico" \
;       -DOUTFILE="$(pwd)/zephyrftp-windows-x64-setup.exe" \
;       tools/windows-installer.nsi
;
; VERSION/DISTDIR/LICENSEFILE/ICONFILE/OUTFILE all default to
; script-relative paths if not passed, purely so this can be
; syntax-checked standalone (`makensis -DVERSION=0.0.0-dev
; tools\windows-installer.nsi` from the repo root) — always pass the
; real values explicitly for an actual build, per the invocation above.
; Verified end to end (2026-08-19): a real cross-compiled zephyrftp.exe
; + collect-win-runtime.sh's DLL closure, packaged with this script,
; installed/launched/uninstalled cleanly under `wine` — files, both
; Start Menu shortcuts, and both registry keys all present after
; install and all genuinely gone after a silent `uninstall.exe /S`.

!ifndef VERSION
  !define VERSION "0.0.0-dev"
!endif
!ifndef DISTDIR
  !define DISTDIR "..\dist"
!endif
!ifndef LICENSEFILE
  !define LICENSEFILE "..\LICENSE"
!endif
!ifndef ICONFILE
  !define ICONFILE "..\resources\icons\app-icon.ico"
!endif
!ifndef OUTFILE
  !define OUTFILE "zephyrftp-windows-x64-setup.exe"
!endif

!include "MUI2.nsh"
!include "FileFunc.nsh"

; Explicit — this Fedora build of NSIS (mingw64-nsis) only ships amd64
; stubs, not the x86 ones makensis defaults to targeting; confirmed
; directly (`ls /usr/share/nsis/Stubs/` has no `*-x86-*` entries at all)
; after a first build attempt failed with "reading stub
; ...Stubs/zlib-x86-unicode": No such file or directory. Matches this
; installer's own name (zephyrftp-windows-x64-setup.exe) anyway — a
; 32-bit installer wouldn't make sense for an x64-only app build.
Target amd64-unicode

Name "ZephyrFTP"
OutFile "${OUTFILE}"
Unicode true
InstallDir "$PROGRAMFILES64\ZephyrFTP"
; A prior install's own registry key, not a fixed default — respects a
; custom install location chosen on a previous run/upgrade instead of
; silently reinstalling to the default path.
InstallDirRegKey HKLM "Software\ZephyrFTP" "InstallDir"
RequestExecutionLevel admin

; Real publisher/product metadata on the .exe itself (Explorer's
; Properties > Details tab), same identity CMakeLists.txt's macOS
; bundle and the .deb/.rpm CPack metadata already use elsewhere.
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "ZephyrFTP"
VIAddVersionKey "CompanyName" "Bad Cluster"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2026 Bad Cluster"
VIAddVersionKey "FileDescription" "ZephyrFTP Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

!define MUI_ABORTWARNING
!define MUI_ICON "${ICONFILE}"
!define MUI_UNICON "${ICONFILE}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSEFILE}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_STARTMENUPAGE_REGISTRY_ROOT "HKLM"
!define MUI_STARTMENUPAGE_REGISTRY_KEY "Software\ZephyrFTP"
!define MUI_STARTMENUPAGE_REGISTRY_VALUENAME "StartMenuFolder"
Var StartMenuFolder
!insertmacro MUI_PAGE_STARTMENU Application $StartMenuFolder
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\zephyrftp.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "ZephyrFTP" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  ; Everything already assembled under DISTDIR — the .exe, every DLL
  ; collect-win-runtime.sh's import-table walk found, and the Qt plugin
  ; subdirectories, recreated with the same subfolder structure. See
  ; this file's own header comment on why this is recursive rather than
  ; an explicit file list.
  File /r "${DISTDIR}\*.*"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\ZephyrFTP" "InstallDir" "$INSTDIR"

  ; Add/Remove Programs entry. DisplayIcon points at the installed exe
  ; itself (already carries the real app icon via app-icon.rc) rather
  ; than shipping a separate icon just for this — one less thing that
  ; could drift out of sync with the real app icon.
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "DisplayName" "ZephyrFTP"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "Publisher" "Bad Cluster"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "URLInfoAbout" "https://github.com/arelas/zephyrftp"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "DisplayIcon" "$INSTDIR\zephyrftp.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "QuietUninstallString" "$INSTDIR\uninstall.exe /S"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "NoRepair" 1
  ; EstimatedSize wants KiB, $0 is bytes at this point in the script
  ; (SectionGetSize-style totals aren't available mid-Section without
  ; extra plugins) — $INSTDIR's own on-disk size via GetSize is close
  ; enough for Add/Remove Programs' display purposes, not load-bearing
  ; anywhere else.
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP" "EstimatedSize" "$0"

  !insertmacro MUI_STARTMENU_WRITE_BEGIN Application
    CreateDirectory "$SMPROGRAMS\$StartMenuFolder"
    CreateShortCut "$SMPROGRAMS\$StartMenuFolder\ZephyrFTP.lnk" "$INSTDIR\zephyrftp.exe"
    CreateShortCut "$SMPROGRAMS\$StartMenuFolder\Uninstall ZephyrFTP.lnk" "$INSTDIR\uninstall.exe"
  !insertmacro MUI_STARTMENU_WRITE_END
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\ZephyrFTP.lnk" "$INSTDIR\zephyrftp.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "ZephyrFTP itself — required."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Add a shortcut on the Desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  !insertmacro MUI_STARTMENU_GETFOLDER Application $StartMenuFolder

  ; Removes exactly what Section "ZephyrFTP" installed — the app files
  ; and the uninstaller itself. Deliberately does NOT touch
  ; HKCU\Software\Bad Cluster\ZephyrFTP (AppSettings' QSettings key,
  ; set via QApplication::setOrganizationName in main.cpp) or anything
  ; under %APPDATA% — uninstalling the app shouldn't silently discard
  ; someone's saved sites, preferences, or connection history, matching
  ; every other platform's uninstall behavior (deb/rpm/AppImage removal
  ; doesn't touch ~/.config either).
  RMDir /r "$INSTDIR"

  Delete "$SMPROGRAMS\$StartMenuFolder\ZephyrFTP.lnk"
  Delete "$SMPROGRAMS\$StartMenuFolder\Uninstall ZephyrFTP.lnk"
  RMDir "$SMPROGRAMS\$StartMenuFolder"
  Delete "$DESKTOP\ZephyrFTP.lnk"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZephyrFTP"
  DeleteRegKey HKLM "Software\ZephyrFTP"
SectionEnd
