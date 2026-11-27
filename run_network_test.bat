@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
if exist testdata\server_net rmdir /s /q testdata\server_net
start "" /b build\minidb_server.exe --data testdata\server_net --port 6379 --threads 32 --memtable-kb 64 --l0-trigger 3 --level-base-kb 128 --target-kb 128 > build\server.log 2>&1
build\test_network.exe --host 127.0.0.1 --port 6379 --clients 16 --ops 10000 --keys 2000
set RESULT=%ERRORLEVEL%
timeout /t 1 /nobreak > nul
echo NETWORK_TEST_EXIT=%RESULT%
exit /b %RESULT%
