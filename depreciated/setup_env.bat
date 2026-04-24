@echo off
cmake -B build -S .
cmake --build build --target python_venv

echo.
echo =======================================================
echo Setup Complete!
echo To activate your environment, run: call .venv\Scripts\activate
echo =======================================================
pause
