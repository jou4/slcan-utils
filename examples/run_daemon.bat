@echo off
setlocal


rem start "vcan0" /B build\Release\slcd.exe --vcan vcan0 >logs\vcan0.log 2>&1
start "vcan0" /B build\Release\slcd.exe --vcan can0
start "vcan1" /B build\Release\slcd.exe --vcan can1
start "vcan2" /B build\Release\slcd.exe --vcan can2


echo Press Ctrl+C, or just close this window, to stop every channel
echo at once.
pause >nul
