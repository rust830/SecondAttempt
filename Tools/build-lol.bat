@echo off
REM ---------------------------------------------------------------------------
REM  Build this Unreal project inside the ALREADY RUNNING Visual Studio 2022.
REM
REM  Double-click to rebuild. Flags are passed straight through, e.g.
REM      build-lol.bat --close-editor
REM      build-lol.bat --close-editor --launch-editor
REM      build-lol.bat --check-only
REM
REM  Run with no flags to build using the currently active VS configuration.
REM ---------------------------------------------------------------------------
setlocal
set "PYTHONIOENCODING=utf-8"

set "PROJECT_DIR=%~dp0.."
set "PY=C:\Users\qqq\anaconda3\python.exe"
if not exist "%PY%" set "PY=py"

"%PY%" "%~dp0vs_build.py" --project-dir "%PROJECT_DIR%" %*
set "RC=%ERRORLEVEL%"

echo.
echo exit code: %RC%
pause
endlocal
