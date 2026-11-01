; Instalador do Framux para Windows (NSIS 3, Unicode).
; Gerado por packaging/windows/build-dist.sh, que passa:
;   /DVERSION=0.4.7  /DSRC=<pasta com os arquivos>  /DOUT=<instalador .exe>
;
; O que faz além de copiar os arquivos:
;  - registra a camada Vulkan em HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers;
;  - põe o usuário que instala em "Usuários do log de desempenho" (SID
;    S-1-5-32-559), para o daemon ler os eventos de present (FPS de jogos
;    DirectX/OpenGL) sem ser administrador. Vale a partir do próximo login.

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "x64.nsh"

!define APP "Framux"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Framux"
!define LAYERS_KEY "SOFTWARE\Khronos\Vulkan\ImplicitLayers"

Name "${APP} ${VERSION}"
OutFile "${OUT}"
InstallDir "$PROGRAMFILES64\${APP}"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
BrandingText "${APP} ${VERSION}"

!define MUI_ICON "${SRC}\framux.ico"
!define MUI_UNICON "${SRC}\framux.ico"
!define MUI_ABORTWARNING
; Sem elevação: o instalador roda como administrador e um framuxd iniciado
; por uma janela elevada não recebe as imagens dos jogos Vulkan. O Explorer
; abre o programa com o token comum do usuário.
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION RunUnelevated
!define MUI_FINISHPAGE_RUN_TEXT "Abrir o Framux"
!define MUI_FINISHPAGE_TEXT "O Framux foi instalado.$\r$\n$\r$\nPara medir o FPS de jogos DirectX e OpenGL, saia e entre de novo no Windows (a permissão de leitura dos eventos de desempenho vale a partir do próximo login)."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRC}\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "PortugueseBR"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "O Framux requer o Windows 10 ou 11 de 64 bits."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function RunUnelevated
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\framux-gui.exe"'
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Framux" SecMain
  SectionIn RO
  ; Um framuxd da versão anterior seguraria os arquivos.
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM framuxd.exe'
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM framux-gui.exe'

  SetOutPath "$INSTDIR"
  File /r "${SRC}\*.*"

  ; Camada Vulkan implícita (só ativa com FRAMUX=1: `framux run` ou modo automático).
  WriteRegDWORD HKLM "${LAYERS_KEY}" "$INSTDIR\framux_vk_x86_64.json" 0

  ; Leitura dos eventos de present (ETW) sem administrador.
  ; O usuário conectado no console (não o do UAC, que pode ser outro administrador).
  nsExec::ExecToLog `"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$$u = (Get-CimInstance Win32_ComputerSystem).UserName; if ($$u) { try { Add-LocalGroupMember -SID S-1-5-32-559 -Member $$u -ErrorAction Stop } catch { if ($$_.FullyQualifiedErrorId -notlike 'MemberExists*') { throw } } }"`

  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortcut "$SMPROGRAMS\${APP}\${APP}.lnk" "$INSTDIR\framux-gui.exe" "" "$INSTDIR\framux-gui.exe" 0
  CreateShortcut "$SMPROGRAMS\${APP}\Desinstalar o ${APP}.lnk" "$INSTDIR\uninstall.exe"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Framux"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\framux-gui.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://github.com/viniciussoaresbr/framux"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM framuxd.exe'
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM framux-gui.exe'

  DeleteRegValue HKLM "${LAYERS_KEY}" "$INSTDIR\framux_vk_x86_64.json"
  ; Integração do usuário que desinstala (registro feito pela janela).
  DeleteRegValue HKCU "${LAYERS_KEY}" "$INSTDIR\framux_vk_x86_64.json"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Framux"
  DeleteRegValue HKCU "Environment" "FRAMUX"
  DeleteRegValue HKCU "Environment" "FRAMUX_AUTO"
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=2000

  Delete "$SMPROGRAMS\${APP}\${APP}.lnk"
  Delete "$SMPROGRAMS\${APP}\Desinstalar o ${APP}.lnk"
  RMDir "$SMPROGRAMS\${APP}"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
  ; A config (%APPDATA%\framux), os vídeos, screenshots e benchmarks ficam.
SectionEnd
