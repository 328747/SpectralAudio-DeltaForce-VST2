@echo off
setlocal
cd /d "%~dp0"
where powershell >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Windows PowerShell is required.
  pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_plugin.ps1"
if errorlevel 1 (
  echo.
  echo [FAILED] See errors above. No DLL was installed.
  pause
  exit /b 1
)
echo.
echo [DONE] Please use the plugin DLL from the dist folder.
pause
