@echo off
rem Starts the chat server and every area of Astonia 3.5 on Windows.
rem Each area runs in its own minimized console window. Use stop.bat to stop them.
cd /d "%~dp0"

if exist stop.flag del stop.flag

rem Chatserver needs to run first
start "Astonia chatserver" /min chatserver.exe
timeout /t 1 /nobreak >nul

call :area 1  "Cameron"
call :area 2  "Below Aston"
call :area 3  "Aston"
call :area 4  "Earth Pents"
call :area 5  "Aston Sewers"
call :area 6  "Earth Underground"
call :area 7  "Fire Pents"
call :area 8  "Fire Underground"
call :area 9  "Ice Pents"
call :area 10 "Ice Underground"
call :area 11 "Ice Palace"
call :area 12 "Mines"
call :area 13 "Clan Halls"
call :area 14 "Random Dungeons"
call :area 15 "Swamps"
call :area 16 "Exkordon Forest"
call :area 17 "Exkordon"
call :area 18 "Bone Tower"
call :area 19 "Nomad Plains"
rem 20-21 empty
call :area 22 "Labs"
call :area 23 "Ice Army Caves 1"
call :area 24 "Ice Army Caves 2"
call :area 25 "Rodney's Warped World"
call :area 26 "Below Aston 2"
rem 27 empty
call :area 28 "Brannington Forest + Robbers"
call :area 29 "Brannington"
call :area 30 "Clan Spawners"
rem 31-32 empty
call :area 33 "Long Tunnels"
rem 34-35 empty
call :area 36 "Caligar"
call :area 37 "Arkhata"
call :area 40 "Thornwood Vale"

echo Done
goto :eof

:area
echo area %1 %~2
start "Astonia area %1 - %~2" /min server35.exe -a %1
timeout /t 1 /nobreak >nul
goto :eof
