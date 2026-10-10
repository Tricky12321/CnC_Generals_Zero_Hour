@echo off
rem Dobbeltklik for at installere. Videresender argumenter til install-windows.ps1.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-windows.ps1" %*
pause
