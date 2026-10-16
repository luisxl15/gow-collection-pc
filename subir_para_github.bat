@echo off
rem De dois cliques neste arquivo. Ele roda o subir_para_github.ps1 da mesma pasta.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0subir_para_github.ps1" %*
echo.
pause
