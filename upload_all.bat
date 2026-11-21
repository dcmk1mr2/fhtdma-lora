@echo off
rem Double-click to build and flash every connected board (see upload_all.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0upload_all.ps1" %*
pause
