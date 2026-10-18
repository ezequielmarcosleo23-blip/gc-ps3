@echo off
rem Receptor del log UDP de gc-ps3 en Windows. Uso: tools\logrecv.cmd [-UntilResult] [-Port 18194]
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0logrecv.ps1" %*
