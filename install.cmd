@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set "ventureInstallExit=%errorlevel%"
if not "%ventureInstallExit%"=="0" pause
exit /b %ventureInstallExit%
