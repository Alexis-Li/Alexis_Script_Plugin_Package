@echo off
setlocal
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0build_mtou_topia_gui.ps1"
if errorlevel 1 pause
