@echo off
rem Lets you type `hx` from cmd.exe or from a PowerShell whose execution policy
rem would otherwise refuse `.\hx.ps1`. Add this directory to PATH.
rem
rem ASCII ONLY IN THIS FILE. cmd.exe reads .cmd files in the machine's OEM
rem codepage (936 here), not UTF-8. UTF-8 Chinese bytes get re-split as GBK
rem pairs, the alignment shifts, and an ASCII byte from the middle of a
rem multi-byte sequence surfaces as a command separator -- cmd then tries to
rem run fragments of a `rem` line. Observed: "'TH' is not recognized as an
rem internal or external command".
rem
rem -ExecutionPolicy Bypass is why this shim exists at all: a default Windows
rem install is Restricted, so hx.ps1 cannot be launched directly.
setlocal
set "HX_PS=pwsh"
where pwsh >nul 2>nul || set "HX_PS=powershell"
"%HX_PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0hx.ps1" %*
exit /b %ERRORLEVEL%
