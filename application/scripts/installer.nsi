!ifndef VERSION
  !error "VERSION is required"
!endif
!ifndef SOURCE_DIR
  !error "SOURCE_DIR is required"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE is required"
!endif
!ifndef ICON_FILE
  !error "ICON_FILE is required"
!endif
!ifndef UNINSTALL_LIST
  !error "UNINSTALL_LIST is required"
!endif
!ifndef PREFLIGHT_LIST
  !error "PREFLIGHT_LIST is required"
!endif

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "源音效管理工具"
OutFile "${OUTPUT_FILE}"
!ifdef INSTALLER_TEST_ROOT
  ; Isolated fixture builds never consult or change a real installation.
  InstallDir "${INSTALLER_TEST_ROOT}"
!else
  InstallDir "$LOCALAPPDATA\Programs\YuanAudio"
  InstallDirRegKey HKCU "Software\YuanAudio" "InstallDir"
!endif
RequestExecutionLevel user
SetCompressor zlib
CRCCheck force
ShowInstDetails show
ShowUninstDetails show
Icon "${ICON_FILE}"
UninstallIcon "${ICON_FILE}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "源音效管理工具"
VIAddVersionKey "FileDescription" "源音效管理工具安装程序"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

!include /CHARSET=UTF8 "${PREFLIGHT_LIST}"

!macro DefinePreflight PREFIX
Function ${PREFIX}CheckInstallDirectory
  Push $0
  Push $1
  StrCmp $INSTDIR "" unsafe
  GetFullPathName $0 "$INSTDIR"
  GetFullPathName $1 "$INSTDIR\.."
  StrCmp $0 $1 unsafe
  StrCmp $0 $WINDIR unsafe
  StrCmp $0 $SYSDIR unsafe
  StrCmp $0 $PROFILE unsafe
  StrCmp $0 $DESKTOP unsafe
  StrCmp $0 $LOCALAPPDATA unsafe
  StrCmp $0 $PROGRAMFILES unsafe
  StrCmp $0 "$LOCALAPPDATA\Programs" unsafe
!ifdef INSTALLER_TEST_ROOT
  StrCmp $0 "${INSTALLER_TEST_ROOT}" 0 unsafe
!endif
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r1'
  IntCmp $1 -1 safe
  IntOp $1 $1 & 0x400
  IntCmp $1 0 safe
  unsafe:
    SetErrorLevel 3
    Abort "请选择独立的程序文件夹，不能使用磁盘根目录、系统目录或目录链接。"
  safe:
    Pop $1
    Pop $0
FunctionEnd

Function ${PREFIX}CheckPayloadDirectory
  Exch $0
  Push $1
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r1'
  IntCmp $1 -1 done
  IntOp $1 $1 & 0x400
  IntCmp $1 0 done
    SetErrorLevel 3
    Abort "程序子目录是目录链接，无法安全更新。请改用独立的程序文件夹。"
  done:
    Pop $1
    Pop $0
FunctionEnd

Function ${PREFIX}CheckFileUnlocked
  Exch $0
  Push $1
  IfFileExists "$0" 0 done
  ; OPEN_EXISTING, shared read/write/delete: no file is created, truncated or
  ; written. A running image denies write/delete access, including worker mode.
  System::Call 'kernel32::CreateFileW(w r0, i 0x40010000, i 7, p 0, i 3, i 0x80, p 0) p .r1'
  IntCmp $1 -1 blocked
  System::Call 'kernel32::CloseHandle(p r1)'
  Goto done
  blocked:
    SetErrorLevel 2
    Abort "程序文件正在使用或没有替换权限。请退出源音效管理工具及其后台任务后重试；本次尚未更改安装文件。"
  done:
    Pop $1
    Pop $0
FunctionEnd
!macroend
!insertmacro DefinePreflight ""
!insertmacro DefinePreflight "un."

!macro DeleteInstalledFile RELATIVE
  Delete "$INSTDIR\${RELATIVE}"
  IfFileExists "$INSTDIR\${RELATIVE}" 0 +3
    SetErrorLevel 3
    Abort "部分程序文件无法删除，请关闭程序后重试。快捷方式和安装记录已保留。"
!macroend

Section "源音效管理工具" Main
  SetShellVarContext current
  Call CheckInstallDirectory
  !insertmacro CheckInstalledFiles ""
  ; This check runs before even an old (0.11 and earlier) uninstaller, whose
  ; deletion errors were not checked. Do not remove the old app while it runs.
  Push "$INSTDIR\YuanAudio.exe"
  Call CheckFileUnlocked
  Push "$INSTDIR\Uninstall.exe"
  Call CheckFileUnlocked
  ; Remove the previous installed binaries before copying this clean stage.
  IfFileExists "$INSTDIR\Uninstall.exe" 0 install
    ExecWait '"$INSTDIR\Uninstall.exe" /S _?=$INSTDIR' $0
    ${If} $0 != 0
      SetErrorLevel 3
      Abort "旧版本卸载失败，请关闭正在运行的源音效管理工具后重试。"
    ${EndIf}
    ; _? runs the old uninstaller in place, so the new installer removes that
    ; executable after it exits before writing the replacement uninstaller.
    Delete "$INSTDIR\Uninstall.exe"
    IfFileExists "$INSTDIR\Uninstall.exe" 0 install
      SetErrorLevel 3
      Abort "无法替换旧版本卸载程序，请关闭正在运行的源音效管理工具后重试。"
  install:
  ClearErrors
  SetOutPath "$INSTDIR"
  SetOverwrite try
  File /r "${SOURCE_DIR}\*"
  ${If} ${Errors}
    SetErrorLevel 3
    Abort "程序文件未能完整安装，请检查磁盘空间和目录权限后重新运行安装程序。"
  ${EndIf}
  ClearErrors
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ${If} ${Errors}
    SetErrorLevel 3
    Abort "卸载程序未能保存，请检查目录权限后重新运行安装程序。"
  ${EndIf}

!ifndef INSTALLER_TEST_ROOT
  CreateDirectory "$SMPROGRAMS\源音效管理工具"
  CreateShortCut "$SMPROGRAMS\源音效管理工具\源音效管理工具.lnk" "$INSTDIR\YuanAudio.exe"
  CreateShortCut "$SMPROGRAMS\源音效管理工具\卸载源音效管理工具.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortCut "$DESKTOP\源音效管理工具.lnk" "$INSTDIR\YuanAudio.exe"

  WriteRegStr HKCU "Software\YuanAudio" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio" "DisplayName" "源音效管理工具"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio" "DisplayIcon" "$INSTDIR\YuanAudio.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio" "NoRepair" 1
!endif
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  Call un.CheckInstallDirectory
  !insertmacro CheckInstalledFiles "un."
  !include /CHARSET=UTF8 "${UNINSTALL_LIST}"
!ifndef INSTALLER_TEST_ROOT
  Delete "$DESKTOP\源音效管理工具.lnk"
  Delete "$SMPROGRAMS\源音效管理工具\源音效管理工具.lnk"
  Delete "$SMPROGRAMS\源音效管理工具\卸载源音效管理工具.lnk"
  RMDir "$SMPROGRAMS\源音效管理工具"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\YuanAudio"
  DeleteRegKey HKCU "Software\YuanAudio"
!endif
  ; Leave %LOCALAPPDATA%\AudioLibrary: existing libraries, settings, favorites
  ; and waveform cache belong to the user, not the installer.
SectionEnd
