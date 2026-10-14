@echo off
rem Asks every area server to save all players and shut down, then stops the chat server.
cd /d "%~dp0"

echo. > stop.flag
echo Waiting for the area servers to save and exit...

:wait
tasklist /FI "IMAGENAME eq server35.exe" 2>nul | find /I "server35.exe" >nul
if not errorlevel 1 (
    timeout /t 1 /nobreak >nul
    goto wait
)

taskkill /IM chatserver.exe >nul 2>&1
taskkill /F /IM chatserver.exe >nul 2>&1
del stop.flag
echo Done
