; LUMINA v1.3.0 - NSIS installer (user-level, silent-capable, English UI)
;
; Discipline: keep this script PURE ASCII (project convention for Windows
; tooling). The package contains CJK file names; the NSIS Unicode build
; handles them via the filesystem while the script text stays ASCII-only.
;
; Usage:
;   "C:\Program Files (x86)\NSIS\makensis.exe" /V2 scripts\installer.nsi
;   ... /DPKG_SRC=<dir> to build from a different source tree
;
; Properties:
;   - user-level install (RequestExecutionLevel user, default
;     $LOCALAPPDATA\Programs\LUMINA, no admin required)
;   - install directory page is changeable; /S = fully silent install
;   - Start Menu shortcut always; Desktop shortcut is an optional component
;   - per-user Add/Remove Programs entry (HKCU) with EstimatedSize
;   - app data (photos, DB, sidecars) lives in %USERPROFILE%\.lumina and is
;     NEVER touched by uninstall (install dir is runtime read-only)

!define APP_NAME "LUMINA"
!define APP_VERSION "1.3.0"
!define APP_PUBLISHER "LUMINA Team"
!define APP_EXE "LUMINA.exe"

!ifndef PKG_SRC
  !define PKG_SRC "${__FILEDIR__}\..\build\pkg\LUMINA"
!endif

Unicode true
Name "${APP_NAME} ${APP_VERSION}"
OutFile "${__FILEDIR__}\..\build\pkg\${APP_NAME}-${APP_VERSION}-setup.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP_NAME} ${APP_VERSION} - user-level install"

VIProductVersion "1.3.0.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} photography workspace"
VIAddVersionKey "FileVersion" "1.3.0.0"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "Publisher" "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "(c) ${APP_PUBLISHER}"

!include "MUI2.nsh"
!include "FileFunc.nsh"

; ---- installer pages (English UI) ----
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES

; ---- uninstaller pages ----
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "LUMINA" SEC_MAIN
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; recursive copy of the packaged app (renderer bundle + electron runtime + engine)
  File /r "${PKG_SRC}\*.*"

  ; per-user install bookkeeping
  WriteRegStr HKCU "Software\${APP_NAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\${APP_NAME}" "Version" "${APP_VERSION}"

  ; Add/Remove Programs entry (HKCU only - no admin required)
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayName" "${APP_NAME} ${APP_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "UninstallString" "$\"$INSTDIR\uninstaller.exe$\""
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "NoRepair" 1
  ; EstimatedSize in KB (shown by the Add/Remove Programs list)
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "EstimatedSize" "$0"

  ; Start Menu shortcuts (always)
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\uninstaller.exe"

  WriteUninstaller "$INSTDIR\uninstaller.exe"
SectionEnd

Section "Desktop Shortcut" SEC_DESKTOP
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_MAIN} "The LUMINA application (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} "Create a shortcut on the desktop (optional)."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APP_NAME}"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
  DeleteRegKey HKCU "Software\${APP_NAME}"
  ; install dir is runtime read-only; user data in %USERPROFILE%\.lumina is
  ; deliberately left intact (photos / DB / sidecars survive uninstall)
  RMDir /r "$INSTDIR"
SectionEnd
