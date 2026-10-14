@echo off
rem Stops the account manager started by start.bat.
taskkill /FI "WINDOWTITLE eq Astonia account manager*" >nul 2>&1
echo Done
