@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-GT7-Experimental.ps1"
if errorlevel 1 pause
