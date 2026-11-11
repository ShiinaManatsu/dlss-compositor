; Standard installer only: if seed-nr.mjs placed a local NR DLL next to Setup,
; copy it into the CLI dir. The standalone local NR installer embeds the DLL
; in its payload and skips this hook.
!include "LogicLib.nsh"

!macro customInstall
  ${If} ${FileExists} "$EXEDIR\nvngx_dlssnr.dll"
    CreateDirectory "$INSTDIR\resources\backend\cli"
    CopyFiles /SILENT "$EXEDIR\nvngx_dlssnr.dll" "$INSTDIR\resources\backend\cli\nvngx_dlssnr.dll"
  ${EndIf}
!macroend
