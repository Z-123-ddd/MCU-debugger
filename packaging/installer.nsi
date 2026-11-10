; ---------------------------------------------------------------------------
; MCU Register Debugger - NSIS installer script
;
;   packaging\pack.bat            (calls D:\NSIS\makensis.exe)
;
; Expects a staged folder created by scripts\deploy.bat:
;   deploy\mcuRegDebugger.exe + Qt DLLs + plugins + data\ (+ tools\probe-rs)
; ---------------------------------------------------------------------------

!include "MUI2.nsh"
!include "x64.nsh"

!define APP_NAME "MCU Register Debugger"
!define APP_EXE "mcuRegDebugger.exe"
!define APP_VERSION "1.1.1.0"
!define APP_PUBLISHER "MCU Tools"
!define APP_REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\McuRegDebugger"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "..\dist\McuRegDebugger-${APP_VERSION}-setup.exe"
InstallDir "$PROGRAMFILES64\MCU Register Debugger"
InstallDirRegKey HKLM "Software\McuRegDebugger" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
Unicode true

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

Section "Main" SecMain
    SectionIn RO
    SetOutPath "$INSTDIR"
    File /r "..\deploy\*.*"

    WriteRegStr HKLM "Software\McuRegDebugger" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "${APP_REGKEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "${APP_REGKEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "${APP_REGKEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKLM "${APP_REGKEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKLM "${APP_REGKEY}" "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegDWORD HKLM "${APP_REGKEY}" "NoModify" 1
    WriteRegDWORD HKLM "${APP_REGKEY}" "NoRepair" 1

    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
    CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\uninstall.exe"
    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    RMDir /r "$INSTDIR"
    DeleteRegKey HKLM "${APP_REGKEY}"
    DeleteRegKey HKLM "Software\McuRegDebugger"
SectionEnd
