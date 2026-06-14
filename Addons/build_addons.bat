@echo off
:: ─────────────────────────────────────────────────────────────────────────────
::  build_addons.bat — Build all Patchy addons in one go (Windows)
::
::  Usage:
::    build_addons.bat              Build only
::    build_addons.bat --install    Build + install to Patchy addons folder
::    build_addons.bat --clean      Clean build directory first, then build
::    build_addons.bat --clean --install
::
::  Built binaries land in:  Addons\build\addons\
::  Install destination:     %APPDATA%\Patchy\Addons\
:: ─────────────────────────────────────────────────────────────────────────────

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"
set "INSTALL=false"
set "CLEAN=false"

:: ── Parse arguments ──────────────────────────────────────────────────────────
for %%A in (%*) do (
    if "%%A"=="--install" set "INSTALL=true"
    if "%%A"=="--clean"   set "CLEAN=true"
)

:: ── Clean ────────────────────────────────────────────────────────────────────
if "%CLEAN%"=="true" (
    echo ^> Cleaning build directory...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

:: ── Configure ────────────────────────────────────────────────────────────────
echo ^> Configuring...
cmake -S "%SCRIPT_DIR%" ^
      -B "%BUILD_DIR%" ^
      -DCMAKE_BUILD_TYPE=Release ^
      --log-level=WARNING
if errorlevel 1 goto :error

:: ── Build ────────────────────────────────────────────────────────────────────
echo ^> Building...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :error

:: ── Report ───────────────────────────────────────────────────────────────────
echo.
echo Built addons:
for %%F in ("%BUILD_DIR%\addons\Release\*.dll") do echo     %%~nxF

:: ── Install ──────────────────────────────────────────────────────────────────
if "%INSTALL%"=="true" (
    echo.
    echo ^> Installing...
    cmake --install "%BUILD_DIR%" --prefix "%USERPROFILE%"
    echo.
    echo Addons installed to:
    echo     %APPDATA%\Patchy\Addons\
)

echo.
echo Done.
goto :end

:error
echo.
echo Build failed.
exit /b 1

:end
endlocal
