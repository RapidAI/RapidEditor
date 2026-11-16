; RapidEditor NSIS installer -- x64 / ARM64 adaptive
;
; Build from the repository root (build_installer.bat does this for you):
;   makensis -DPRODUCT_VERSION=1.0.0 ^
;            -DPAYLOAD_X64=build-win-x64\RapidEditor.exe ^
;            -DPAYLOAD_ARM64=build-win-arm64\RapidEditor.exe ^
;            -DOUTFILE=dist\RapidEditor-1.0.0-windows-universal-setup.exe ^
;            packaging\windows\installer.nsi
;
; At least one payload is required. When both are given, .onInit detects the
; machine (IsNativeARM64 / RunningX64) and the section installs the matching
; binary as RapidEditor.exe -- one installer, no architecture choice to make.
;
; NOTE: this file is UTF-8 with BOM. NSIS 3 reads .nsi files in the system ANSI
; code page unless a BOM is present; Chinese menu text needs the BOM.

Unicode true

!include "LogicLib.nsh"
!include "x64.nsh"

!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "1.0.0"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\..\RapidEditor-${PRODUCT_VERSION}-windows-setup.exe"
!endif
; Both payloads are optional individually, but at least one is required.
!ifndef PAYLOAD_X64
!ifndef PAYLOAD_ARM64
  !error "Pass -DPAYLOAD_X64=<path> and/or -DPAYLOAD_ARM64=<path> (see build_installer.bat)"
!endif
!endif

!define PRODUCT_NAME      "RapidEditor"
!define PRODUCT_PUBLISHER "RapidAI"
!define PRODUCT_WEB_SITE  "https://github.com/RapidAI/RapidEditor"
!define APP_EXE           "RapidEditor.exe"
; NSIS changes the working directory to the .nsi's directory, so paths in
; File/Icon are relative to packaging/windows/.
!define RE_ASSETS          "..\..\assets"

Name             "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile          "${OUTFILE}"
InstallDir       "$PROGRAMFILES64\${PRODUCT_NAME}"
InstallDirRegKey HKLM "Software\${PRODUCT_NAME}" "Install_Dir"
RequestExecutionLevel admin
ShowInstDetails  show
SetCompressor    /SOLID lzma
Icon             "${RE_ASSETS}\rapideditor.ico"
UninstallIcon    "${RE_ASSETS}\rapideditor.ico"
ManifestDPIAware true

VIProductVersion "${PRODUCT_VERSION}.0"
VIAddVersionKey /LANG=2052 "ProductName"     "${PRODUCT_NAME}"
VIAddVersionKey /LANG=2052 "FileDescription" "${PRODUCT_NAME} Installer"
VIAddVersionKey /LANG=2052 "CompanyName"     "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=2052 "LegalCopyright"  "Copyright (c) RapidAI"
VIAddVersionKey /LANG=2052 "FileVersion"     "${PRODUCT_VERSION}"

; ---------------------------------------------------------------------------
; Right-click verbs.
;   HKCR\*                              -> every file
;   HKCR\txtfile                        -> the plain-text class explicitly
;   HKCR\SystemFileAssociations\.ext    -> each common text/code type below
; SystemFileAssociations is the documented place to attach a verb to an
; extension without taking over its ProgID, so the user's default app and
; file icons are never changed. The extension list lives in TEXT_EXTS only.
; ---------------------------------------------------------------------------
!define TEXT_EXTS "txt text log md markdown rst adoc org tex json jsonc json5 yaml yml toml ini cfg conf properties env csv tsv xml xsl xsd html htm xhtml css scss less sass js jsx mjs cjs ts tsx vue svelte c h cpp hpp cc cxx hxx cs java kt kts scala go rs py pyw rb php pl pm lua sh bash zsh fish ps1 bat cmd sql swift m mm r dart groovy gradle asm s v vhdl diff patch gitignore gitattributes editorconfig dockerfile makefile cmake lock"

!macro ADD_CONTEXT_MENU KEY
  WriteRegStr HKCR "${KEY}\shell\RapidEditor" "" "用 RapidEditor 打开"
  WriteRegStr HKCR "${KEY}\shell\RapidEditor" "Icon" "$INSTDIR\${APP_EXE},0"
  WriteRegStr HKCR "${KEY}\shell\RapidEditor\command" "" '"$INSTDIR\${APP_EXE}" "%1"'
!macroend

!macro DEL_CONTEXT_MENU KEY
  DeleteRegKey HKCR "${KEY}\shell\RapidEditor"
!macroend

Var /GLOBAL RE_ExtRest
Var /GLOBAL RE_ExtTok
Var /GLOBAL RE_ExtOp

; Walks TEXT_EXTS and adds/removes the per-extension verb.
; $RE_ExtOp is "add" or "del"; callers run with the correct SetRegView.
; Generated twice so the uninstaller gets its own un.RE_TextExtAssoc.
!macro RE_DEFINE_TEXT_EXT_ASSOC UN
Function ${UN}RE_TextExtAssoc
  StrCpy $RE_ExtRest "${TEXT_EXTS}"
  RE_TextExt_loop:
    StrLen $0 $RE_ExtRest
    StrCmp $0 0 RE_TextExt_done
    StrCpy $1 0
  RE_TextExt_scan:
    IntCmp $1 $0 RE_TextExt_last RE_TextExt_continue RE_TextExt_last
  RE_TextExt_continue:
    StrCpy $2 $RE_ExtRest 1 $1
    StrCmp $2 " " RE_TextExt_found
    IntOp $1 $1 + 1
    Goto RE_TextExt_scan
  RE_TextExt_found:
    StrCpy $RE_ExtTok $RE_ExtRest $1
    IntOp $1 $1 + 1
    StrCpy $RE_ExtRest $RE_ExtRest "" $1
    Goto RE_TextExt_use
  RE_TextExt_last:
    StrCpy $RE_ExtTok $RE_ExtRest
    StrCpy $RE_ExtRest ""
  RE_TextExt_use:
    ${If} $RE_ExtOp == "del"
      DeleteRegKey HKCR "SystemFileAssociations\.$RE_ExtTok\shell\RapidEditor"
    ${Else}
      WriteRegStr HKCR "SystemFileAssociations\.$RE_ExtTok\shell\RapidEditor" "" "用 RapidEditor 打开"
      WriteRegStr HKCR "SystemFileAssociations\.$RE_ExtTok\shell\RapidEditor" "Icon" "$INSTDIR\${APP_EXE},0"
      WriteRegStr HKCR "SystemFileAssociations\.$RE_ExtTok\shell\RapidEditor\command" "" '"$INSTDIR\${APP_EXE}" "%1"'
    ${EndIf}
    Goto RE_TextExt_loop
  RE_TextExt_done:
FunctionEnd
!macroend
!insertmacro RE_DEFINE_TEXT_EXT_ASSOC ""
!insertmacro RE_DEFINE_TEXT_EXT_ASSOC "un."

; $R0 = x64 | arm64 | x86. Checked before anything is written, so a wrong-arch
; installer exits with a clear message instead of leaving a broken install.
Function .onInit
  ${If} ${IsNativeARM64}
    StrCpy $R0 "arm64"
  ${ElseIf} ${RunningX64}
    StrCpy $R0 "x64"
  ${Else}
    StrCpy $R0 "x86"
  ${EndIf}

  ${If} $R0 == "arm64"
    !ifndef PAYLOAD_ARM64
      MessageBox MB_OK|MB_ICONSTOP "此安装包只包含 x64 版本，无法在 ARM64 Windows 上安装。$\r$\n请下载 windows-arm64 或 windows-universal 安装包。"
      Abort
    !endif
  ${ElseIf} $R0 == "x64"
    !ifndef PAYLOAD_X64
      MessageBox MB_OK|MB_ICONSTOP "此安装包只包含 ARM64 版本，无法在 x64 Windows 上安装。$\r$\n请下载 windows-x64 或 windows-universal 安装包。"
      Abort
    !endif
  ${Else}
    MessageBox MB_OK|MB_ICONSTOP "RapidEditor 需要 64 位 Windows（x64 或 ARM64）。"
    Abort
  ${EndIf}
FunctionEnd

Section "RapidEditor" SEC_MAIN
  SetRegView 64
  SetOutPath "$INSTDIR"

  ; Stage every payload, then rename the one matching this machine.
  !ifdef PAYLOAD_X64
    File "/oname=bin-x64.tmp" "${PAYLOAD_X64}"
  !endif
  !ifdef PAYLOAD_ARM64
    File "/oname=bin-arm64.tmp" "${PAYLOAD_ARM64}"
  !endif

  ${If} $R0 == "arm64"
    !ifdef PAYLOAD_ARM64
      Delete "$INSTDIR\${APP_EXE}"
      Rename "$INSTDIR\bin-arm64.tmp" "$INSTDIR\${APP_EXE}"
      DetailPrint "已安装 ARM64 版本"
    !endif
  ${Else}
    !ifdef PAYLOAD_X64
      Delete "$INSTDIR\${APP_EXE}"
      Rename "$INSTDIR\bin-x64.tmp" "$INSTDIR\${APP_EXE}"
      DetailPrint "已安装 x64 版本"
    !endif
  ${EndIf}
  Delete "$INSTDIR\bin-x64.tmp"
  Delete "$INSTDIR\bin-arm64.tmp"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\${PRODUCT_NAME}" "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM "Software\${PRODUCT_NAME}" "Arch" "$R0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                   "DisplayName"     "${PRODUCT_NAME} ${PRODUCT_VERSION} ($R0)"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                   "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                   "DisplayVersion"  "${PRODUCT_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                   "DisplayIcon"    "$INSTDIR\${APP_EXE},0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                   "Publisher"       "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}" \
                   "URLInfoAbout"    "${PRODUCT_WEB_SITE}"

  ; Right-click: any file, the text-file class, and all common text/code types.
  !insertmacro ADD_CONTEXT_MENU "*"
  !insertmacro ADD_CONTEXT_MENU "txtfile"
  StrCpy $RE_ExtOp "add"
  Call RE_TextExtAssoc

  ; Register as a known editor so Windows lists it under "Open with".
  ; This does not change the user's default association.
  WriteRegStr HKCR "Applications\${APP_EXE}" "FriendlyAppName" "${PRODUCT_NAME}"
  WriteRegStr HKCR "Applications\${APP_EXE}\shell\open\command" "" '"$INSTDIR\${APP_EXE}" "%1"'
SectionEnd

Section "Uninstall"
  SetRegView 64
  !insertmacro DEL_CONTEXT_MENU "*"
  !insertmacro DEL_CONTEXT_MENU "txtfile"
  StrCpy $RE_ExtOp "del"
  Call un.RE_TextExtAssoc
  DeleteRegKey HKCR "Applications\${APP_EXE}"

  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\bin-x64.tmp"
  Delete "$INSTDIR\bin-arm64.tmp"
  Delete "$INSTDIR\uninstall.exe"
  RMDir  "$INSTDIR"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
  DeleteRegKey HKLM "Software\${PRODUCT_NAME}"
SectionEnd
