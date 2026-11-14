@echo off
rem Coldchain Guardian: build for QNX x86_64, copy to the target, run it.
rem Usage (from a cmd window):  deploy.bat 192.168.56.104
rem Stop it with Ctrl+C (this also stops it on QNX).
rem   deploy.bat 192.168.56.104 nobuild   skips compiling and reuses build\coldchaind
setlocal
if "%~1"=="" (
  echo usage: deploy.bat ^<target-ip^>
  exit /b 1
)
set IP=%~1
set SSHOPT=-c aes128-ctr -o MACs=hmac-sha2-256

if not defined QNX_HOST call C:\Users\Husayn\qnx800\qnxsdp-env.bat >nul
@echo off
cd /d %~dp0

if not exist build mkdir build
if /i "%~2"=="nobuild" (
  echo [1/4] skipping compile, using existing build\coldchaind
  goto copy
)
echo [1/4] compiling for QNX...
qcc -Vgcc_ntox86_64 -O2 -std=gnu11 -Wall -Wno-format-truncation -o build\coldchaind src\main.c src\platform.c src\world.c src\plant.c src\resmgr.c src\guardian.c src\stress.c src\http.c -lsocket -lm || exit /b 1

:copy
echo [2/4] stopping any running copy on %IP% ...
ssh %SSHOPT% root@%IP% "mkdir -p /data/coldchain; slay -s KILL -f coldchaind >/dev/null 2>&1; sleep 1; true" || exit /b 1

echo [3/4] copying files ...
scp -q %SSHOPT% build\coldchaind root@%IP%:/data/coldchain/coldchaind.new || exit /b 1
scp -q %SSHOPT% tools\trace.sh root@%IP%:/data/coldchain/ || exit /b 1
if exist "%QNX_TARGET%\x86_64\usr\bin\traceprinter" scp -q %SSHOPT% "%QNX_TARGET%\x86_64\usr\bin\traceprinter" root@%IP%:/data/coldchain/
scp -q %SSHOPT% -r web root@%IP%:/data/coldchain/ || exit /b 1
ssh %SSHOPT% root@%IP% "mv -f /data/coldchain/coldchaind.new /data/coldchain/coldchaind && chmod +x /data/coldchain/coldchaind; chmod +x /data/coldchain/traceprinter 2>/dev/null; rm -f /data/coldchain/*.kev?" || exit /b 1

echo [4/4] running coldchaind - open http://%IP%:8080   (Ctrl+C to stop)
ssh -t %SSHOPT% root@%IP% "/data/coldchain/coldchaind --web /data/coldchain/web"
