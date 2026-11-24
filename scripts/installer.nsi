; ---------------------------------------------------------------------------
; NSIS installer for MCU Register Debugger.
;
; Built by scripts\package.bat, which stages deploy\ first (Qt runtime + data +
; SVD packs) and then calls makensis with DEPLOY_DIR / OUT_FILE / APP_VERSION
; defined on the command line.
;
; Installs per-user into %LOCALAPPDATA%\Programs, so no administrator rights are
; needed. That matches how the app already behaves at runtime: learned DEV_IDs
; and the release notes go to %APPDATA%\MCUTools\McuRegDebugger anyway.
; ---------------------------------------------------------------------------

Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "WordFunc.nsh" ; ${WordFind}, for parsing the J-Link version
!include "LogicLib.nsh"

; ---- filled in by package.bat ---------------------------------------------
!ifndef APP_VERSION
    !define APP_VERSION "1.1.1.0"
!endif
!ifndef DEPLOY_DIR
    !define DEPLOY_DIR "..\deploy"
!endif
!ifndef OUT_FILE
    !define OUT_FILE "..\dist\McuRegDebugger-setup.exe"
!endif
; J-Link payloads, staged by scripts\fetch-jlink.py. Both are optional at compile time:
; the installer still builds without them and then guides the user instead.
!ifndef JLINK_INSTALLER_DIR
    !define JLINK_INSTALLER_DIR "..\tools\jlink-installer"
!endif
!ifndef JLINK_DEVICES_DIR
    !define JLINK_DEVICES_DIR "..\tools\jlink-devices"
!endif

; Registry key SEGGER's own installer writes. Reading HKLM is allowed for a per-user
; installer; we only ever read it.
!define BS "\"
!define SEGGER_REG "SOFTWARE\SEGGER\J-Link"

!define APP_NAME      "MCU Register Debugger"
!define APP_SHORT     "McuRegDebugger"
!define APP_PUBLISHER "MCU Tools"
!define APP_EXE       "mcuRegDebugger.exe"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_SHORT}"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_SHORT}"
InstallDirRegKey HKCU "Software\${APP_SHORT}" "InstallDir"
RequestExecutionLevel user
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${APP_VERSION}"
VIAddVersionKey /LANG=2052 "ProductName"     "${APP_NAME}"
VIAddVersionKey /LANG=2052 "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion"     "${APP_VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion"  "${APP_VERSION}"
VIAddVersionKey /LANG=2052 "CompanyName"     "${APP_PUBLISHER}"
VIAddVersionKey /LANG=2052 "LegalCopyright"  "GPL v2"

; ---- look ---------------------------------------------------------------
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${APP_NAME} ${APP_VERSION}"
!define MUI_WELCOMEPAGE_TEXT "这是一个 MCU 寄存器级调试器：\r\n\r\n  * 连接 J-Link，按 DEV_ID 识别芯片，用 CMSIS-SVD 重建寄存器视图\r\n  * 导入 Keil 的 .axf，勾选变量后用 HSS 实时采样并绘图 / 做频谱分析\r\n\r\n安装包内含 Qt 运行时、11 套 Nationstech SVD 支持包与器件数据库，无需另外安装。"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "立即启动 ${APP_NAME}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"


; ---------------------------------------------------------------------------
; J-Link runtime dependencies
;
; The application talks to the hardware through SEGGER's J-Link DLL (the only backend) and
; uses JLink.exe only for flashing. Neither is shipped here -- they are commercial software,
; and segger.com blocks scripted downloads -- so the installer makes sure they end up present:
;
;   * already installed  -> nothing to do;
;   * payload available  -> run SEGGER's own installer with /S (it is an NSIS installer, so
;                           silent mode is supported). That installs *on this machine*
;                           under SEGGER's licence; nothing of theirs is redistributed;
;   * neither            -> say so plainly and carry on, so the app is ready the moment
;                           J-Link appears.
;
; The device database is separate and IS shipped: SEGGER's installer knows nothing about
; N32 parts, and without it J-Link answers "unknown device" for every Nationstech chip.
; Both layout variants are carried (they are ~1 MB each) because NSIS "File" runs at
; compile time -- the choice has to be a runtime copy, not a conditional include.
; ---------------------------------------------------------------------------

Var JLINK_DIR

; Push the installed SEGGER directory, or "" when J-Link is absent.
Function FindJLinkDir
    Push $0
    Push $1
    Push $2

    ; This installer is a 32-bit process, so plain HKLM${BS}SOFTWARE reads are redirected to
    ; WOW6432Node -- where SEGGER's 64-bit installer wrote nothing. Without SetRegView 64 the
    ; lookup silently finds nothing and the device database step is skipped, which is exactly
    ; the bug this replaced. Read the 64-bit view first, then fall back to the 32-bit one for
    ; the (rare) 32-bit-only J-Link install.
    SetRegView 64
    ReadRegStr $0 HKLM "${SEGGER_REG}" "InstallPath"
    ReadRegStr $2 HKCU "${SEGGER_REG}" "InstallPath"
    ${If} $0 == ""
        StrCpy $0 $2
    ${EndIf}
    SetRegView 32
    ${If} $0 == ""
        ReadRegStr $0 HKLM "${SEGGER_REG}" "InstallPath"
    ${EndIf}
    ${If} $0 == ""
        ReadRegStr $0 HKCU "${SEGGER_REG}" "InstallPath"
    ${EndIf}
    SetRegView lastused

    ${If} $0 != ""
    ${AndIfNot} ${FileExists} "$0${BS}JLink.exe"
        StrCpy $0 ""
    ${EndIf}

    ${If} $0 == ""
        ; No usable registry entry (portable copy, or the key was cleaned): scan the version
        ; folders. $PROGRAMFILES64 matters for the same reason as SetRegView above -- in a
        ; 32-bit process $PROGRAMFILES points at "Program Files (x86)".
        FindFirst $1 $2 "$PROGRAMFILES64${BS}SEGGER${BS}JLink*"
        ${DoWhile} $2 != ""
            ${If} ${FileExists} "$PROGRAMFILES64${BS}SEGGER${BS}$2${BS}JLink.exe"
                StrCpy $0 "$PROGRAMFILES64${BS}SEGGER${BS}$2"
                ${Break}
            ${EndIf}
            FindNext $1 $2
        ${Loop}
        FindClose $1

        ${If} $0 == ""
            FindFirst $1 $2 "$PROGRAMFILES${BS}SEGGER${BS}JLink*"
            ${DoWhile} $2 != ""
                ${If} ${FileExists} "$PROGRAMFILES${BS}SEGGER${BS}$2${BS}JLink.exe"
                    StrCpy $0 "$PROGRAMFILES${BS}SEGGER${BS}$2"
                    ${Break}
                ${EndIf}
                FindNext $1 $2
            ${Loop}
            FindClose $1
        ${EndIf}
    ${EndIf}

    Pop $2
    Pop $1
    Push $0
    Exch
    Pop $0
FunctionEnd

; Push 1 when the installed J-Link needs the "7.7 and above" device database layout.
;
; J-Link 7.7 changed the schema: the older file carries extra *_UNLOCKOPT device entries
; that the newer one drops. The version is read from the DLL's own file version rather
; than from the folder name ("JLink_V826c" and friends are not worth parsing).
Function IsJLink77OrNewer
    Push $0
    Push $1
    Push $2
    Push $3

    StrCpy $0 1 ; unreadable version -> assume current; SEGGER has been on 8.x for years
    ${If} $JLINK_DIR != ""
        ${GetFileVersion} "$JLINK_DIR${BS}JLink_x64.dll" $1
        ${If} $1 != ""
            StrCpy $0 0
            ; "+1"/"+2" take the n-th token. The "/1" form does NOT -- it returns the whole
            ; string, which silently made every version compare as unparseable.
            ${WordFind} "$1" "." "+1" $2 ; major
            ${WordFind} "$1" "." "+2" $3 ; minor
            ; Only the leading digits are numeric: a build suffix makes the comparison below
            ; silently false (measured: V7.96c was classified as older than 7.7).
            StrCpy $3 $3 2
            ; SEGGER's minor is two digits (V7.70, V7.96, V8.26), so a one-digit minor such as
            ; "7.7" means 7.70, not 7.07 -- pad it, or the boundary test would sort 7.60 above
            ; 7.7 and hand a new J-Link the old database.
            ${If} $3 < 10
                IntOp $3 $3 * 10
            ${EndIf}
            ${If} $2 > 7
                StrCpy $0 1
            ${ElseIf} $2 == 7
            ${AndIf} $3 >= 70
                StrCpy $0 1
            ${EndIf}
        ${EndIf}
    ${EndIf}

    Pop $3
    Pop $2
    Pop $1
    Push $0
    Exch
    Pop $0
FunctionEnd

; Run SEGGER's installer silently, if we have it and J-Link is missing.
Function EnsureJLinkRuntime
    ${If} $JLINK_DIR != ""
        DetailPrint "已安装 J-Link: $JLINK_DIR"
        Return
    ${EndIf}

    ; The payload is optional at build time, so look for it rather than assuming.
    FindFirst $0 $1 "$PLUGINSDIR\jlink-setup\*.exe"
    ${If} $0 == ""
        FindClose $0
        DetailPrint "未找到 J-Link，且安装包未附带 SEGGER 安装程序"
        MessageBox MB_ICONINFORMATION|MB_OK             "本程序通过 SEGGER 的 J-Link 软件与硬件通信，这台电脑上还没有安装。$\r$\n             $\r$\n请到 SEGGER 官网下载并安装（免费）：$\r$\n             https://www.segger.com/downloads/jlink/$\r$\n             $\r$\n应用程序本身已经装好，装完 J-Link 即可直接连接。"
        Return
    ${EndIf}

    StrCpy $2 "$PLUGINSDIR\jlink-setup\$1"
    FindClose $0
    DetailPrint "静默安装 J-Link: $1"
    ; /S is NSIS silent mode -- confirmed by the payload being a Nullsoft installer.
    ClearErrors
    ExecWait '"$2" /S' $3
    ${If} $3 != 0
        DetailPrint "SEGGER 安装程序返回 $3，继续（可能是用户取消或已安装时拒绝降级）"
    ${EndIf}

    Call FindJLinkDir
    Pop $JLINK_DIR
    ${If} $JLINK_DIR != ""
        DetailPrint "J-Link 安装完成: $JLINK_DIR"
    ${Else}
        DetailPrint "J-Link 仍未检测到，请手动安装后重启本程序"
    ${EndIf}
FunctionEnd

; Copy the matching device database into the per-user SEGGER directory.
;
; %APPDATA%\SEGGER\JLinkDevices is where SEGGER looks for vendor databases, and it is
; per-user -- matching this installer, which needs no administrator rights.
Function InstallDeviceDatabase
    ${If} $JLINK_DIR == ""
        DetailPrint "未安装 J-Link，跳过 N32 器件库"
        Return
    ${EndIf}

    Call IsJLink77OrNewer
    Pop $0
    ${If} $0 == 1
        StrCpy $1 "v77"
        StrCpy $2 "7.7 及以上"
    ${Else}
        StrCpy $1 "v64"
        StrCpy $2 "6.4 - 7.6"
    ${EndIf}

    ${IfNot} ${FileExists} "$INSTDIR\jlink-devices\$1\Nations-JLinkDevices.xml"
        DetailPrint "安装包里没有 N32 器件库，跳过"
        Return
    ${EndIf}

    DetailPrint "安装 N32 器件库（J-Link $2 布局）"
    CreateDirectory "$APPDATA\SEGGER\JLinkDevices"
    ; xcopy rather than CopyFiles: the database nests its algorithms one level down
    ; (Devices/Nationstech/*.FLM) and CopyFiles does not recurse. /Y overwrites an older
    ; pack instead of prompting, /I does not ask "file or directory", /Q keeps the log clean.
    nsExec::ExecToLog 'xcopy /E /I /Y /Q "$INSTDIR\jlink-devices\$1" "$APPDATA\SEGGER\JLinkDevices"'
    Pop $3
    ${If} $3 != 0
        DetailPrint "复制器件库时 xcopy 返回 $3（详见上方输出）"
    ${EndIf}
FunctionEnd

; ---- install ------------------------------------------------------------
Section "主程序" SecMain
    SetOutPath "$INSTDIR"

    ; An upgrade over a running copy leaves locked DLLs behind, so stop it first.
    nsExec::Exec 'taskkill /IM "${APP_EXE}" /F'
    Pop $0
    Sleep 300

    ; deploy\ already holds exactly the tree the app expects next to the exe:
    ; Qt DLLs and plugins, data\, svd\ (manifest.json + per-DFP folders).
    File /r "${DEPLOY_DIR}\*.*"

    ; N32 device database, both J-Link layout variants. NSIS "File" is a compile-time
    ; directive, so the version-appropriate one is copied at install time by
    ; InstallDeviceDatabase(); carrying both costs ~2 MB and removes the decision.
    ; Each variant needs its OWN SetOutPath: "File /r" flattens the matched root into
    ; the current out path, so pointing both at jlink-devices\ would let the second
    ; one overwrite the first and the runtime copy would then find no v64/ at all.
    ; /nonfatal before /r is the order NSIS accepts; /x keeps the vendor's stand-alone
    ; unlock tool out -- this application never runs it, and a stray executable inside
    ; a device-database folder is not something to ship quietly.
    SetOutPath "$INSTDIR\jlink-devices\v64"
    File /nonfatal /r /x *.exe /x *.ini "${JLINK_DEVICES_DIR}\v64\*.*"
    SetOutPath "$INSTDIR\jlink-devices\v77"
    File /nonfatal /r /x *.exe /x *.ini "${JLINK_DEVICES_DIR}\v77\*.*"

    ; SEGGER's own installer, if the packager staged one. /nonfatal keeps the build working
    ; when it is absent -- the install then guides the user instead of failing.
    SetOutPath "$PLUGINSDIR\jlink-setup"
    File /nonfatal "${JLINK_INSTALLER_DIR}\*.exe"
    SetOutPath "$INSTDIR"

    WriteRegStr HKCU "Software\${APP_SHORT}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "Software\${APP_SHORT}" "Version" "${APP_VERSION}"

    ; Add/Remove Programs entry. DisplayIcon and EstimatedSize are what make it
    ; look like a normal Windows application rather than a stray folder.
    WriteRegStr   HKCU "${UNINST_KEY}" "DisplayName"     "${APP_NAME}"
    WriteRegStr   HKCU "${UNINST_KEY}" "DisplayVersion"  "${APP_VERSION}"
    WriteRegStr   HKCU "${UNINST_KEY}" "Publisher"       "${APP_PUBLISHER}"
    WriteRegStr   HKCU "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
    WriteRegStr   HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr   HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr   HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; --- J-Link runtime ---------------------------------------------------
    ; Order matters: find out whether J-Link exists (possibly installing it), then drop in
    ; the device database for whichever version we ended up with.
    Call FindJLinkDir
    Pop $JLINK_DIR
    Call EnsureJLinkRuntime
    Call InstallDeviceDatabase

    ; Start menu entries (per-user, since this is a per-user install).
    SetShellVarContext current
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section "桌面快捷方式" SecDesktop
    SetShellVarContext current
    CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

; ---- uninstall ----------------------------------------------------------
Section "Uninstall"
    nsExec::Exec 'taskkill /IM "${APP_EXE}" /F'
    Pop $0
    Sleep 300

    SetShellVarContext current
    Delete "$DESKTOP\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\卸载 ${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"

    ; The device database copied into %APPDATA%\SEGGER is left alone: it is shared with
    ; whatever other SEGGER tooling the user has, so deleting it could break their J-Link
    ; installation for other IDEs.

    ; RMDir /r takes the whole tree, including data\ and svd\.
    RMDir /r "$INSTDIR"

    DeleteRegKey HKCU "${UNINST_KEY}"
    DeleteRegKey HKCU "Software\${APP_SHORT}"

    ; The app's own settings and learned DEV_IDs live in AppData and are left
    ; alone on purpose: an uninstall should not silently discard what the user
    ; taught it about their boards.
SectionEnd
