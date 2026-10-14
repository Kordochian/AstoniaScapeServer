@echo off
rem Starts the account manager on port 8088 with PHP's built-in web server.
rem Needs PHP 8.1+ for Windows on the PATH (see install.md).
cd /d "%~dp0"
where php >nul 2>&1 || (echo PHP not found. See install.md. & exit /b 1)
start "Astonia account manager" /min php -S 0.0.0.0:8088 -t public
echo Started on http://localhost:8088
