; Venture installer for Windows 10/11 x64. Built by build_installer.py, which
; passes the defines below and generates the file lists from the release
; layout's manifest. Per-user by default: no administrator rights, installs to
; %LOCALAPPDATA%\Programs\Venture (user data stays in %LOCALAPPDATA%\Venture).
;
; Required defines: VERSION (x.y.z), VERSION_QUAD (x.y.z.w), OUTFILE,
; INSTALL_FILES, UNINSTALL_LIST, SIZE_KB. Optional: ICON.

Unicode true
; The x86 stub: official NSIS releases ship no amd64 stubs. It installs the
; 64-bit release, refuses 32-bit Windows and reads the 64-bit registry view.
Target x86-unicode
ManifestDPIAware true
ManifestSupportedOS Win10
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!include MUI2.nsh
!include LogicLib.nsh
!include WinVer.nsh
!include x64.nsh
!include FileFunc.nsh

!define APP "Venture"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Venture"
!define APP_KEY "Software\Venture"
; Microsoft Edge WebView2 Runtime (Evergreen) client id, used only to tell
; the user that Meta sign-in needs it. Nothing is downloaded or installed.
!define WEBVIEW2_CLIENT "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
!define WEBVIEW2_URL "https://go.microsoft.com/fwlink/p/?LinkId=2124703"

Name "${APP}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\Venture"
InstallDirRegKey HKCU "${APP_KEY}" "InstallLocation"
BrandingText "${APP} ${VERSION}"
ShowInstDetails nevershow
ShowUninstDetails nevershow

VIProductVersion "${VERSION_QUAD}"
VIFileVersion "${VERSION_QUAD}"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "LegalCopyright" "MIT License"

!ifdef ICON
  !define MUI_ICON "${ICON}"
  !define MUI_UNICON "${ICON}"
!endif
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\venture.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Venture"
; The finish page's second checkbox creates an optional desktop shortcut.
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create a desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION DesktopShortcut

!insertmacro MUI_PAGE_WELCOME
; Leaving the folder page refuses a folder this account cannot write and
; waits for Venture and its games to close (CheckInstallDir).
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirectoryLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Remove what a previous installation listed in uninstall.lst: files, then
; (non-recursively) directories, so nothing the installer did not place there
; is ever deleted. Lines are paths relative to $INSTDIR; directories end in
; a backslash and come deepest first. Absolute or parent-relative lines are
; ignored.
!macro REMOVE_LISTED PREFIX
  Function ${PREFIX}RemoveListed
    Pop $R0
    ClearErrors
    FileOpen $R1 "$R0" r
    ${If} ${Errors}
      Return
    ${EndIf}
    ${Do}
      ClearErrors
      FileRead $R1 $R2
      ${If} ${Errors}
        ${Break}
      ${EndIf}
      ; Strip the line terminator.
      ${Do}
        StrCpy $R3 $R2 1 -1
        ${If} $R3 == "$\r"
        ${OrIf} $R3 == "$\n"
          StrCpy $R2 $R2 -1
        ${Else}
          ${Break}
        ${EndIf}
      ${Loop}
      ${If} $R2 == ""
        ${Continue}
      ${EndIf}
      StrCpy $R3 $R2 1
      ${If} $R3 == "\"
      ${OrIf} $R3 == "/"
        ${Continue}
      ${EndIf}
      ; Reject drive-qualified paths and ".." components.
      StrCpy $R4 "\$R2\"
      StrCpy $R5 0
      StrCpy $R6 ""
      ${Do}
        StrCpy $R3 $R4 1 $R5
        ${If} $R3 == ""
          ${Break}
        ${EndIf}
        StrCpy $R7 $R4 4 $R5
        ${If} $R3 == ":"
        ${OrIf} $R7 == "\..\"
          StrCpy $R6 "unsafe"
          ${Break}
        ${EndIf}
        IntOp $R5 $R5 + 1
      ${Loop}
      ${If} $R6 != ""
        ${Continue}
      ${EndIf}
      StrCpy $R3 $R2 1 -1
      ${If} $R3 == "\"
        RMDir "$INSTDIR\$R2"
      ${Else}
        Delete "$INSTDIR\$R2"
      ${EndIf}
    ${Loop}
    FileClose $R1
  FunctionEnd
!macroend
!insertmacro REMOVE_LISTED ""
!insertmacro REMOVE_LISTED "un."

Function .onInit
  SetRegView 64
  ${IfNot} ${RunningX64}
  ${OrIfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP|MB_OK "Venture needs 64-bit Windows 10 or 11." /SD IDOK
    Abort
  ${EndIf}
  ; This release's file list, for finding files of the folder still in use.
  InitPluginsDir
  File "/oname=$PLUGINSDIR\release.lst" "${UNINSTALL_LIST}"
FunctionEnd

; Pushes 1 when this account can write the folder $INSTDIR (or create it),
; else 0. Nothing is left behind: a test file in an existing folder, or a test
; folder in the nearest existing parent of one still to be created.
Function Writable
  Push $0
  Push $1
  StrCpy $0 $INSTDIR
  StrCpy $1 0
  ${DoUntil} ${FileExists} "$0\*.*"
    ${If} ${FileExists} "$0"
      ; A file, or a folder this account cannot even list.
      StrCpy $1 blocked
      ${Break}
    ${EndIf}
    ${GetParent} $0 $0
    ${If} $0 == ""
      ${Break}
    ${EndIf}
  ${Loop}
  ${If} $1 == blocked
  ${OrIf} $0 == ""
    ; Not a folder that can be written, or a missing drive.
    StrCpy $1 0
  ${ElseIf} $0 == $INSTDIR
    ClearErrors
    FileOpen $1 "$0\venture-write-test.tmp" w
    ${If} ${Errors}
      StrCpy $1 0
    ${Else}
      FileClose $1
      Delete "$0\venture-write-test.tmp"
      StrCpy $1 1
    ${EndIf}
  ${Else}
    ClearErrors
    CreateDirectory "$0\venture-write-test.tmp"
    ${If} ${FileExists} "$0\venture-write-test.tmp\*.*"
      RMDir "$0\venture-write-test.tmp"
      StrCpy $1 1
    ${EndIf}
  ${EndIf}
  Exch $1
  Exch
  Pop $0
FunctionEnd

; Pushes the release files in $INSTDIR that cannot be opened for writing,
; comma separated, or "" when none is. A running venture.exe,
; venture-runtime.exe (the game) or venture-java.exe locks its executable and
; the libraries it loaded; overwriting those fails with "Error opening file
; for writing". Opening a file to write without writing changes nothing.
Function InUseFiles
  Push $R0
  Push $R1
  Push $R2
  Push $R3
  Push $R4
  StrCpy $R0 ""
  StrCpy $R4 0
  ClearErrors
  FileOpen $R1 "$PLUGINSDIR\release.lst" r
  ${IfNot} ${Errors}
    ${Do}
      ClearErrors
      FileRead $R1 $R2
      ${If} ${Errors}
        ${Break}
      ${EndIf}
      ${Do}
        StrCpy $R3 $R2 1 -1
        ${If} $R3 == "$\r"
        ${OrIf} $R3 == "$\n"
          StrCpy $R2 $R2 -1
        ${Else}
          ${Break}
        ${EndIf}
      ${Loop}
      StrCpy $R3 $R2 1 -1
      ${If} $R2 == ""
      ${OrIf} $R3 == "\"
        ${Continue}
      ${EndIf}
      ${IfNot} ${FileExists} "$INSTDIR\$R2"
        ${Continue}
      ${EndIf}
      ClearErrors
      FileOpen $R3 "$INSTDIR\$R2" a
      ${If} ${Errors}
        IntOp $R4 $R4 + 1
        ${If} $R4 <= 8
          ${If} $R0 != ""
            StrCpy $R0 "$R0, "
          ${EndIf}
          StrCpy $R0 "$R0$R2"
        ${ElseIf} $R4 == 9
          StrCpy $R0 "$R0, ..."
        ${EndIf}
      ${Else}
        FileClose $R3
      ${EndIf}
    ${Loop}
    FileClose $R1
  ${EndIf}
  Pop $R4
  Pop $R3
  Pop $R2
  Pop $R1
  Exch $R0
FunctionEnd

; Sets $0 to "ok" when $INSTDIR can be installed to, else "cancel" after
; telling the user why: a folder this account cannot write (Program Files
; needs administrator rights, which this installer never asks for), or files
; there still in use, which the user can close and retry.
Function CheckInstallDir
  Call Writable
  Pop $0
  ${If} $0 != 1
    MessageBox MB_ICONEXCLAMATION|MB_OK "Venture cannot be installed in $INSTDIR because this Windows account cannot write to it. Folders such as Program Files need administrator rights, and this installer never asks for them.$\r$\n$\r$\nChoose a folder you own, such as the default:$\r$\n$LOCALAPPDATA\Programs\Venture" /SD IDOK
    StrCpy $0 cancel
    Return
  ${EndIf}
  ${Do}
    Call InUseFiles
    Pop $0
    ${If} $0 == ""
      StrCpy $0 ok
      Return
    ${EndIf}
    MessageBox MB_ICONEXCLAMATION|MB_RETRYCANCEL "Venture, or a game it started, is still running from $INSTDIR, so its files cannot be replaced.$\r$\n$\r$\nClose Venture and any game it is running, then choose Retry.$\r$\n$\r$\nIn use: $0" /SD IDCANCEL IDRETRY retry
    StrCpy $0 cancel
    Return
  retry:
  ${Loop}
FunctionEnd

Function DirectoryLeave
  Call CheckInstallDir
  ${If} $0 != ok
    Abort  ; stays on the folder page
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Function DesktopShortcut
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\venture.exe"
FunctionEnd

Function WebView2Notice
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_CLIENT}" "pv"
  ${EndIf}
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    MessageBox MB_ICONINFORMATION|MB_YESNO "Sign in with Meta needs the Microsoft Edge WebView2 Runtime, which this PC does not have yet. Everything else in Venture works without it.$\r$\n$\r$\nOpen Microsoft's download page now?" /SD IDNO IDNO done
    ExecShell "open" "${WEBVIEW2_URL}"
  done:
  ${EndIf}
FunctionEnd

Section "Venture" SecMain
  SectionIn RO
  ; Again for silent installs, which skip the folder page, and for anything
  ; started since.
  Call CheckInstallDir
  ${If} $0 != ok
    DetailPrint "Installation cancelled: $INSTDIR cannot be written or is in use."
    Abort
  ${EndIf}
  ; Upgrade in place: first remove exactly the files the previous version
  ; installed, so files dropped from the release do not linger.
  ${If} ${FileExists} "$INSTDIR\uninstall.lst"
  ${AndIf} ${FileExists} "$INSTDIR\venture.exe"
    Push "$INSTDIR\uninstall.lst"
    Call RemoveListed
  ${EndIf}
  SetOverwrite on
  !include "${INSTALL_FILES}"
  SetOutPath "$INSTDIR"
  File "/oname=uninstall.lst" "${UNINSTALL_LIST}"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\venture.exe"
  WriteRegStr HKCU "${APP_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\venture.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://venture-emu.org/"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" ${SIZE_KB}
  Call WebView2Notice
SectionEnd

Section "Uninstall"
  Push "$INSTDIR\uninstall.lst"
  Call un.RemoveListed
  Delete "$INSTDIR\uninstall.lst"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"
  ; Games, sign-in and settings in %LOCALAPPDATA%\Venture are kept.
SectionEnd
