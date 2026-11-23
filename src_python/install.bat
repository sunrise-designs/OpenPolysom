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

rem ---------------------------------------------------------------------------
rem Web viewer: needs Node.js to build src_web\dist\chart.js from TypeScript.
rem ---------------------------------------------------------------------------
where npm >nul 2>nul
if errorlevel 1 (
    echo.
    echo Node.js not found - installing Node.js LTS with winget...
    where winget >nul 2>nul
    if errorlevel 1 goto :no_node
    winget install --id OpenJS.NodeJS.LTS -e --accept-source-agreements --accept-package-agreements
    rem winget updates PATH for new terminals only; add it for this one.
    set "PATH=%ProgramFiles%\nodejs;%PATH%"
)
where npm >nul 2>nul
if errorlevel 1 goto :no_node

pushd "%~dp0..\src_web"
call npm install
if errorlevel 1 (
    popd
    echo ERROR: npm install failed.
    goto :end
)
call npm run build
if errorlevel 1 (
    popd
    echo ERROR: building the web viewer failed.
    goto :end
)
popd
echo.
echo Install complete.
goto :end

:no_node
echo.
echo ERROR: Node.js/npm is not available, so the web viewer was not built.
echo Install Node.js LTS from https://nodejs.org, open a new terminal, and
echo run this script again.

:end
pause
