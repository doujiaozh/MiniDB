@echo off
setlocal
cd /d "%~dp0"
python tests\test_resp.py
exit /b %ERRORLEVEL%
