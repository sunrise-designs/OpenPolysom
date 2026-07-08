@echo off
setlocal

rem Windows ships fake python.exe/python3.exe "App Execution Alias" stubs in
rem WindowsApps that shadow a real install on PATH and print a Store prompt
rem instead of running Python. Remove them (if present) before installing.
set "ALIAS_DIR=%LOCALAPPDATA%\Microsoft\WindowsApps"
if exist "%ALIAS_DIR%\python.exe" del /f "%ALIAS_DIR%\python.exe"
if exist "%ALIAS_DIR%\python3.exe" del /f "%ALIAS_DIR%\python3.exe"

where python
where python3

python -m pip install --upgrade pip
python -m pip install -r "%~dp0requirements.txt"
pause
