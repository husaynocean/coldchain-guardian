@echo off
rem Coldchain Guardian: build for QNX x86_64 and copy to the target.
rem Usage (from a cmd window):  deploy.bat 192.168.56.104
setlocal
if "%~1"=="" (
  echo usage: deploy.bat ^<target-ip^>
  exit /b 1
)
set IP=%~1
set SSHOPT=-c aes128-ctr -o MACs=hmac-sha2-256

if not defined QNX_HOST call C:\Users\Husayn\qnx800\qnxsdp-env.bat >nul
cd /d %~dp0

if not exist build mkdir build
echo [1/3] compiling for QNX...
qcc -Vgcc_ntox86_64 -O2 -std=gnu11 -Wall -Wno-format-truncation -o build\coldchaind src\main.c src\platform.c src\world.c src\plant.c src\resmgr.c src\guardian.c src\stress.c src\http.c -lsocket -lm || exit /b 1

echo [2/3] copying to %IP%:/data/coldchain ...
ssh %SSHOPT% root@%IP% "mkdir -p /data/coldchain; slay -f coldchaind 2>/dev/null; true" || exit /b 1
scp %SSHOPT% build\coldchaind tools\trace.sh root@%IP%:/data/coldchain/ || exit /b 1
scp %SSHOPT% -r web root@%IP%:/data/coldchain/ || exit /b 1

echo [3/3] starting coldchaind (Ctrl+C to stop) - open http://%IP%:8080
ssh %SSHOPT% root@%IP% "chmod +x /data/coldchain/coldchaind && /data/coldchain/coldchaind --web /data/coldchain/web"
