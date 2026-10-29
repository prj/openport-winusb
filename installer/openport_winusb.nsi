; Built by build.ps1, which stages the signed files in stage\.
!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh

!define NAME "OpenPort 2.0 WinUSB"
!define VERSION "1.0.0"
!define REGKEY "Software\OpenPort 2.0 WinUSB"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenPort20WinUSB"
!define J2534KEY "SOFTWARE\PassThruSupport.04.04\Tactrix Inc. - OpenPort 2.0 J2534 ISO/CAN/VPW/PWM"

Name "${NAME}"
OutFile "stage\OpenPort20WinUSB-${VERSION}.exe"
RequestExecutionLevel admin
Unicode True
InstallDir "$PROGRAMFILES32\${NAME}"

!define MUI_WELCOMEPAGE_TEXT "Replaces the OpenPort 2.0 driver with Microsoft's WinUSB driver, which works on Windows 11.$\r$\n$\r$\nProgrammes keep using the OpenPort 2.0 J2534 device as before.$\r$\n$\r$\nClose all diagnostic programmes before continuing."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!macro Driver ACTION
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\driver\driver.ps1" ${ACTION}'
  Pop $0
!macroend

Section "Install"
  SectionIn RO

  ; Whether Tactrix's DLL and J2534 entry were here before us, so uninstall puts back only what it found.
  ReadRegStr $0 HKLM "${REGKEY}" "HadTactrixDll"
  ${If} $0 == ""
    StrCpy $0 0
    ${If} ${FileExists} "$SYSDIR\op20pt32.dll"
    ${AndIfNot} ${FileExists} "$SYSDIR\op20pt32_tactrix.dll"
      StrCpy $0 1
    ${EndIf}
    WriteRegStr HKLM "${REGKEY}" "HadTactrixDll" $0
    StrCpy $1 0
    ReadRegStr $2 HKLM "${J2534KEY}" "FunctionLibrary"
    ${If} $2 != ""
      StrCpy $1 1
    ${EndIf}
    WriteRegStr HKLM "${REGKEY}" "HadTactrixKey" $1
  ${EndIf}

  SetOutPath "$INSTDIR\driver"
  File "stage\openport_winusb.inf"
  File "stage\openport_winusb.cat"
  File "stage\publisher.cer"
  File "driver.ps1"
  SetOutPath "$INSTDIR"
  File "stage\LICENSE"
  File "stage\Tactrix-License.txt"

  ; Before the DLLs: deleting Tactrix's package deletes the op20pt32.dll its INF installed, whatever it now is.
  DetailPrint "Installing the WinUSB driver..."
  !insertmacro Driver install
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "The driver could not be installed (code $0). Unplug and replug the OpenPort, then run setup again."
  ${EndIf}

  ; The shim takes Tactrix's file name; on 64-bit Windows $SYSDIR here is SysWOW64.
  SetOutPath "$SYSDIR"
  File "/oname=op20pt32_tactrix.dll" "stage\op20pt32_tactrix.dll"
  File "/oname=op20pt32.dll" "stage\op20winusb.dll"

  ${If} ${RunningX64}
    StrCpy $3 "$WINDIR\SysWOW64\op20pt32.dll"
  ${Else}
    StrCpy $3 "$SYSDIR\op20pt32.dll"
  ${EndIf}
  WriteRegStr HKLM "${J2534KEY}" "Name" "OpenPort 2.0 J2534 ISO/CAN/VPW/PWM"
  WriteRegStr HKLM "${J2534KEY}" "Vendor" "Tactrix Inc."
  WriteRegStr HKLM "${J2534KEY}" "ConfigApplication" ""
  WriteRegStr HKLM "${J2534KEY}" "FunctionLibrary" $3
  WriteRegDWORD HKLM "${J2534KEY}" "DeviceId" 0
  WriteRegDWORD HKLM "${J2534KEY}" "CAN" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO9141" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO9141_CH1" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO9141_CH2" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO9141_CH3" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO14230" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO14230_CH1" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO14230_CH2" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO14230_CH3" 1
  WriteRegDWORD HKLM "${J2534KEY}" "ISO15765" 1
  WriteRegDWORD HKLM "${J2534KEY}" "J1850PWM" 0
  WriteRegDWORD HKLM "${J2534KEY}" "J1850VPW" 0
  ; The separate entry from bench testing would show a second OpenPort.
  DeleteRegKey HKLM "SOFTWARE\PassThruSupport.04.04\OpenPort 2.0 WinUSB"

  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${NAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "VehiCAL"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  DetailPrint "Removing the WinUSB driver..."
  !insertmacro Driver uninstall

  ReadRegStr $0 HKLM "${REGKEY}" "HadTactrixDll"
  ReadRegStr $1 HKLM "${REGKEY}" "HadTactrixKey"
  Delete /REBOOTOK "$SYSDIR\op20pt32.dll"
  ${If} $0 == 1
    Rename /REBOOTOK "$SYSDIR\op20pt32_tactrix.dll" "$SYSDIR\op20pt32.dll"
  ${Else}
    Delete /REBOOTOK "$SYSDIR\op20pt32_tactrix.dll"
  ${EndIf}
  ${If} $1 != 1
    DeleteRegKey HKLM "${J2534KEY}"
  ${EndIf}

  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "${REGKEY}"
  RMDir /r /REBOOTOK "$INSTDIR"
SectionEnd
