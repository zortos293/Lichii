; SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
; SPDX-License-Identifier: GPL-3.0-or-later

; Usage:
;   get the latest nsis: https://nsis.sourceforge.io/Download
;   makensis /DPRODUCT_VERSION=<version> /DARCH=<arch> /DVARIANT=<variant> \
;     [/DBINARY_SOURCE_DIR=<bin dir>] [/DMANIFEST_FILE=<file>] [/DOUTPUT_FILE=<file>] installer.nsi
;   MANIFEST_FILE lists the files of BINARY_SOURCE_DIR in the updater's install-manifest.txt
;   format; the uninstaller and the in-place updater both read it.

; Require these for makensis.
!ifndef PRODUCT_VERSION
  !error "PRODUCT_VERSION must be defined"
!endif

!ifndef ARCH
  !error "ARCH must be defined"
!endif

!ifndef VARIANT
  !error "VARIANT must be defined"
!endif

Unicode true
ManifestDPIAware true
; A per-user install: no UAC prompt, and $LOCALAPPDATA and HKCU are the user's own.
RequestExecutionLevel user

!define PRODUCT_NAME "Lichii"
!define PRODUCT_PUBLISHER "Lichii contributors"
!define PRODUCT_WEB_SITE "https://github.com/zortos293/Lichii"
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\${PRODUCT_NAME}.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"

!define MANIFEST_NAME "install-manifest.txt"

!ifndef BINARY_SOURCE_DIR
  !define BINARY_SOURCE_DIR "..\bin"
!endif

!ifndef OUTPUT_FILE
  !define OUTPUT_FILE "${PRODUCT_NAME}-Windows-${PRODUCT_VERSION}-${ARCH}-${VARIANT}-installer.exe"
!endif

Name "${PRODUCT_NAME}"
OutFile "${OUTPUT_FILE}"
SetCompressor /SOLID lzma
InstallDir "$LOCALAPPDATA\$(^Name)"
; Reinstalling goes to the existing install, wherever it is.
InstallDirRegKey HKCU "${PRODUCT_UNINST_KEY}" "InstallLocation"
ShowInstDetails show
ShowUnInstDetails show

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "TextFunc.nsh"
!include "WordFunc.nsh"
; Custom page plugin
!include "nsDialogs.nsh"

; MUI Settings
!define MUI_ICON "lichii.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
; The language picked at install is kept for the uninstaller and offered first on a reinstall.
!define MUI_LANGDLL_REGISTRY_ROOT HKCU
!define MUI_LANGDLL_REGISTRY_KEY "${PRODUCT_UNINST_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "Installer Language"
!define MUI_LANGDLL_ALWAYSSHOW
!define MUI_LANGDLL_ALLLANGUAGES

; License page
!insertmacro MUI_PAGE_LICENSE "..\LICENSE.txt"
; Options page
Page custom optionsPageCreate optionsPageLeave
; Directory page
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckNotRunning
!insertmacro MUI_PAGE_DIRECTORY
; Instfiles page
!insertmacro MUI_PAGE_INSTFILES
; Finish page
!define MUI_FINISHPAGE_RUN "$INSTDIR\lichii.exe"
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_INSTFILES

; Variables
Var OptionsPageDialog
Var DesktopShortcutCheckbox
Var DesktopShortcut
Var FileAssocCheckbox
Var FileAssoc

; Language files: English and the languages Lichii ships (dist/languages).
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Arabic"
!insertmacro MUI_LANGUAGE "Catalan"
!insertmacro MUI_LANGUAGE "Czech"
!insertmacro MUI_LANGUAGE "Danish"
!insertmacro MUI_LANGUAGE "Dutch"
!insertmacro MUI_LANGUAGE "Finnish"
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "Greek"
!insertmacro MUI_LANGUAGE "Hungarian"
!insertmacro MUI_LANGUAGE "Indonesian"
!insertmacro MUI_LANGUAGE "Italian"
!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "Korean"
!insertmacro MUI_LANGUAGE "Norwegian"
!insertmacro MUI_LANGUAGE "Polish"
!insertmacro MUI_LANGUAGE "Portuguese"
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "Russian"
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "Spanish"
!insertmacro MUI_LANGUAGE "Swedish"
!insertmacro MUI_LANGUAGE "TradChinese"
!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "Ukrainian"
!insertmacro MUI_LANGUAGE "Vietnamese"

!include /CHARSET=UTF8 "installer-strings.nsh"

; MUI end ------

; A running program cannot be opened for writing, and Windows keeps its files from being replaced.
!macro IsRunning FILE
  ${If} ${FileExists} "${FILE}"
    ClearErrors
    FileOpen $R0 "${FILE}" a
    ${If} ${Errors}
      StrCpy $R1 1
    ${Else}
      FileClose $R0
    ${EndIf}
  ${EndIf}
!macroend

; Waits until the user closes Lichii, or aborts: on the directory page that keeps the page open.
!macro CheckNotRunningFunction UN
Function ${UN}CheckNotRunning
  ${Do}
    StrCpy $R1 ""
    !insertmacro IsRunning "$INSTDIR\lichii.exe"
    !insertmacro IsRunning "$INSTDIR\eden-cli.exe"
    ${If} $R1 == ""
      Return
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(Running)" /SD IDCANCEL IDRETRY +2
    Abort
  ${Loop}
FunctionEnd
!macroend
!insertmacro CheckNotRunningFunction ""
!insertmacro CheckNotRunningFunction "un."

Function .onInit
  StrCpy $DesktopShortcut 1
  StrCpy $FileAssoc 1
  ; A reinstall keeps the choices of the install it replaces.
  ReadRegStr $0 HKCU "${PRODUCT_UNINST_KEY}" "InstallLocation"
  ${If} $0 != ""
    ${IfNot} ${FileExists} "$DESKTOP\${PRODUCT_NAME}.lnk"
      StrCpy $DesktopShortcut 0
    ${EndIf}
    ReadRegStr $1 HKCU "Software\Classes\.xci" ""
    ${If} $1 != "${PRODUCT_NAME}"
      StrCpy $FileAssoc 0
    ${EndIf}
  ${EndIf}

  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Function optionsPageCreate
  !insertmacro MUI_HEADER_TEXT "$(OptionsTitle)" "$(OptionsSubtitle)"
  nsDialogs::Create 1018
  Pop $OptionsPageDialog
  ${If} $OptionsPageDialog == error
    Abort
  ${EndIf}

  ${NSD_CreateCheckbox} 0u 0u 100% 12u "$(DesktopShortcut)"
  Pop $DesktopShortcutCheckbox
  ${NSD_SetState} $DesktopShortcutCheckbox $DesktopShortcut

  ${NSD_CreateCheckbox} 0u 16u 100% 12u "$(FileTypes)"
  Pop $FileAssocCheckbox
  ${NSD_SetState} $FileAssocCheckbox $FileAssoc

  nsDialogs::Show
FunctionEnd

Function optionsPageLeave
  ${NSD_GetState} $DesktopShortcutCheckbox $DesktopShortcut
  ${NSD_GetState} $FileAssocCheckbox $FileAssoc
FunctionEnd

; Tells Explorer that file associations changed, so icons and Open with update at once.
!macro NotifyAssociationsChanged
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
!macroend

!macro WriteFileType EXT
  WriteRegStr HKCU "Software\Classes\${EXT}" "" "${PRODUCT_NAME}"
!macroend

; Only a type that still opens with Lichii is removed: another program may have taken it since.
!macro RemoveFileType EXT
  ReadRegStr $0 HKCU "Software\Classes\${EXT}" ""
  ${If} $0 == "${PRODUCT_NAME}"
    DeleteRegValue HKCU "Software\Classes\${EXT}" ""
    DeleteRegKey /ifempty HKCU "Software\Classes\${EXT}"
  ${EndIf}
!macroend

!macro ForEachFileType MACRO
  !insertmacro ${MACRO} ".nsp"
  !insertmacro ${MACRO} ".xci"
  !insertmacro ${MACRO} ".nro"
  !insertmacro ${MACRO} ".kip"
!macroend

Section "Base"
  Call CheckNotRunning
  ExecWait '"$INSTDIR\uninst.exe" /S _?=$INSTDIR'

  SectionIn RO

  SetOutPath "$INSTDIR"

  ; The binplaced build output will be included verbatim.
  File /r "${BINARY_SOURCE_DIR}\*"
!ifdef MANIFEST_FILE
  File "/oname=${MANIFEST_NAME}" "${MANIFEST_FILE}"
!endif

  ; Create start menu and desktop shortcuts
  CreateShortCut "$SMPROGRAMS\$(^Name).lnk" "$INSTDIR\lichii.exe"
  ${If} $DesktopShortcut == 1
    CreateShortCut "$DESKTOP\$(^Name).lnk" "$INSTDIR\lichii.exe"
  ${EndIf}
SectionEnd

Section -Post
  WriteUninstaller "$INSTDIR\uninst.exe"

  WriteRegStr HKCU "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\lichii.exe"

  ; Write metadata for add/remove programs applet
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayName" "$(^Name)"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\lichii.exe"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "NoRepair" 1
  ; MUI saves it when leaving the install page, which a silent install skips. Its macro does nothing
  ; after the uninstaller pages, so the value is written here.
  WriteRegStr ${MUI_LANGDLL_REGISTRY_ROOT} "${MUI_LANGDLL_REGISTRY_KEY}" "${MUI_LANGDLL_REGISTRY_VALUENAME}" $LANGUAGE
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "EstimatedSize" "$0"

  ${If} $FileAssoc == ${BST_CHECKED}
    !insertmacro ForEachFileType WriteFileType
    WriteRegStr HKCU "Software\Classes\${PRODUCT_NAME}\DefaultIcon" "" "$INSTDIR\lichii.exe,0"
    WriteRegStr HKCU "Software\Classes\${PRODUCT_NAME}\Shell\open\command" "" '"$INSTDIR\lichii.exe" "%1"'
    !insertmacro NotifyAssociationsChanged
  ${EndIf}
SectionEnd

; Deletes the files the manifest lists, and the folders they leave empty. The updater rewrites the
; manifest on every update, so it lists the installed version's files.
Function un.RemoveManifestFiles
  ClearErrors
  FileOpen $0 "$INSTDIR\${MANIFEST_NAME}" r
  ${If} ${Errors}
    Return
  ${EndIf}
  ${Do}
    ClearErrors
    FileRead $0 $1
    ${If} ${Errors}
      ${ExitDo}
    ${EndIf}
    ${TrimNewLines} "$1" $1
    ${WordReplace} "$1" "/" "\" "+" $1
    ${If} $1 == ""
      ${Continue}
    ${EndIf}
    ; The portable user folder stays, and nothing outside the install directory is touched.
    StrCpy $2 $1 5
    ${If} $2 == "user\"
      ${Continue}
    ${EndIf}
    ClearErrors
    ${WordFind} "$1" ".." "E#" $2
    ${IfNot} ${Errors}
      ${Continue}
    ${EndIf}
    Delete "$INSTDIR\$1"
    ${GetParent} "$INSTDIR\$1" $2
    ${Do}
      ${If} $2 == $INSTDIR
      ${OrIf} $2 == ""
        ${ExitDo}
      ${EndIf}
      RMDir $2
      ${GetParent} $2 $2
    ${Loop}
  ${Loop}
  FileClose $0
FunctionEnd

; Lichii keeps settings, keys, saves, mods and caches in the portable user folder beside the exe,
; or else in %APPDATA%\lichii. Deleting them takes an explicit Yes: No is the default, and a silent
; uninstall, which every reinstall runs, keeps them.
Function un.RemoveUserData
  StrCpy $R2 ""
  ${If} ${FileExists} "$INSTDIR\user\*.*"
    StrCpy $R2 "$R2$\r$\n$INSTDIR\user"
  ${EndIf}
  ${If} ${FileExists} "$APPDATA\lichii\*.*"
    StrCpy $R2 "$R2$\r$\n$APPDATA\lichii $(SharedData)"
  ${EndIf}
  ${If} $R2 == ""
    Return
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(DeleteData)$\r$\n$R2$\r$\n$\r$\n$(KeepData)" /SD IDNO IDYES +2
  Return
  RMDir /r "$INSTDIR\user"
  RMDir /r "$APPDATA\lichii"
FunctionEnd

Function un.onInit
  !insertmacro MUI_UNGETLANGUAGE
  Call un.CheckNotRunning
FunctionEnd

Section Uninstall
  Delete "$DESKTOP\$(^Name).lnk"
  Delete "$SMPROGRAMS\$(^Name).lnk"

  Call un.RemoveManifestFiles
  Delete "$INSTDIR\${MANIFEST_NAME}"
  RMDir /r "$INSTDIR\.update-backup"
  RMDir /r "$INSTDIR\.update-staging"
  Delete "$INSTDIR\.update-download.zip"

  ; Be a bit careful to not delete files a user may have put into the install directory.
  Delete "$INSTDIR\lichii.exe"
  Delete "$INSTDIR\eden-cli.exe"
  Delete "$INSTDIR\uninst.exe"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\README.md"
  RMDir /r "$INSTDIR\LICENSES"
  Call un.RemoveUserData
  RMDir "$INSTDIR"

  !insertmacro ForEachFileType RemoveFileType
  DeleteRegKey HKCU "Software\Classes\${PRODUCT_NAME}"
  !insertmacro NotifyAssociationsChanged

  DeleteRegKey HKCU "Software\Classes\discord-1397286652128264252"

  DeleteRegKey HKCU "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKCU "${PRODUCT_DIR_REGKEY}"

  SetAutoClose true
SectionEnd
