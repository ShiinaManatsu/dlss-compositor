; Dev-only NR feature DLL: the packaging whitelist never embeds
; nvngx_dlssnr.dll into the installer. On dev machines seed-nr.mjs places it
; next to the Setup exe; if present at install time, seed it into the CLI dir.
!include "LogicLib.nsh"

!macro customInstall
  ${If} ${FileExists} "$EXEDIR\nvngx_dlssnr.dll"
    CreateDirectory "$INSTDIR\resources\backend\cli"
    CopyFiles /SILENT "$EXEDIR\nvngx_dlssnr.dll" "$INSTDIR\resources\backend\cli\nvngx_dlssnr.dll"
  ${EndIf}
!macroend
