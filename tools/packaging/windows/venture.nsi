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
