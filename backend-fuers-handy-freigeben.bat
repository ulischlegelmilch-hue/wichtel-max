@echo off
REM ============================================================
REM  Wichtel-Backend (laeuft in WSL) fuers Handy im WLAN oeffnen
REM  >>> RECHTSKLICK -> "Als Administrator ausfuehren" <<<
REM ============================================================
setlocal

REM aktuelle WSL-IP automatisch holen
for /f "tokens=1" %%a in ('wsl hostname -I') do set WSLIP=%%a
echo WSL-IP: %WSLIP%

REM Portweiterleitung Windows(0.0.0.0:8080) -> WSL(%WSLIP%:8080)
netsh interface portproxy delete v4tov4 listenport=8080 listenaddress=0.0.0.0 >nul 2>&1
netsh interface portproxy add    v4tov4 listenport=8080 listenaddress=0.0.0.0 connectport=8080 connectaddress=%WSLIP%

REM Windows-Firewall fuer Port 8080 oeffnen
netsh advfirewall firewall delete rule name="Wichtel Backend 8080" >nul 2>&1
netsh advfirewall firewall add    rule name="Wichtel Backend 8080" dir=in action=allow protocol=TCP localport=8080 >nul

echo.
echo ============================================================
echo  Fertig!  Trage in der App unter Einstellungen ein:
echo.
echo        http://192.168.68.124:8080
echo.
echo  (Handy muss im selben WLAN sein.)
echo ============================================================
echo.
echo Aktive Weiterleitungen:
netsh interface portproxy show v4tov4
echo.
pause
