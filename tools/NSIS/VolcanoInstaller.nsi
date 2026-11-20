; Volcano Client installer. Build with: makensis tools\NSIS\VolcanoInstaller.nsi (after a Release build).
; Installs per user, because the client writes its settings, logs, worlds, Java and downloaded game assets next to
; the exe and Program Files isn't writable without admin rights.

Unicode true
!define APP_NAME "Volcano Client"
!define APP_EXE "VolcanoClient.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VolcanoClient"
!define BUILD_DIR "..\..\build-release"

!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "${APP_NAME}"
OutFile "..\..\VolcanoClientSetup.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
RequestExecutionLevel user

; The installer and uninstaller icons, from resources/logo.png (see resources/logo.ico).
!define MUI_ICON "..\..\resources\logo.ico"
!define MUI_UNICON "..\..\resources\logo.ico"
Icon "..\..\resources\logo.ico"
UninstallIcon "..\..\resources\logo.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME}"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
    SetOutPath "$INSTDIR"
    File "${BUILD_DIR}\${APP_EXE}"
    File "${BUILD_DIR}\*.dll"
    File /r /x "minecraft" /x "minecraft-data" /x "resourcepacks" "..\..\resources"
    File /nonfatal /r "..\..\mods"

    ; Mods detected from an earlier install.
    ${If} ${FileExists} "$INSTDIR\mods\*.dll"
        DetailPrint "Mods detected!"
    ${EndIf}

    ; The shortcut's "Start in" folder is the working directory: the client reads and writes everything relative to it.
    SetOutPath "$INSTDIR"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0

    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

; Leaves settings.json, saves, worlds.json, mods and logs alone: they are the user's.
Section "Uninstall"
    Delete "$SMPROGRAMS\${APP_NAME}.lnk"
    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\*.dll"
    RMDir /r "$INSTDIR\resources\minecraft"
    RMDir /r "$INSTDIR\resources\minecraft-data"
    RMDir /r "$INSTDIR\java"
    Delete "$INSTDIR\Uninstall.exe"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    RMDir "$INSTDIR"
SectionEnd
