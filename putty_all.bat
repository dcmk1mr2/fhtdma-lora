@echo off
rem Opens a PuTTY serial window (115200 8N1) for every USB serial port found.
rem Edit PUTTY if putty.exe is somewhere else.
set "PUTTY=C:\Program Files\PuTTY\putty.exe"
if not exist "%PUTTY%" set "PUTTY=putty.exe"
set FOUND=0
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique"`) do (
  echo Opening %%P
  start "" "%PUTTY%" -serial %%P -sercfg 115200,8,n,1,N
  set FOUND=1
)
if %FOUND%==0 echo No COM ports found. & pause
