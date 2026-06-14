@echo off
:: ─────────────────────────────────────────────────────────────────────────────
::  build_pax.bat — Build all Patchy Pax in one go (Windows)
::
::  Usage:
::    build_pax.bat              Build only
::    build_pax.bat --install    Build + install to Patchy Pax folder
::    build_pax.bat --clean      Clean build directory first, then build
::    build_pax.bat --clean --install
::
::  Built binaries land in:  Pax\build\pax\
::  Install destination:     %APPDATA%\Patchy\Pax\
:: ─────────────────────────────────────────────────────────────────────────────

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"
set "INSTALL=false"
set "CLEAN=false"

for %%A in (%*) do (
    if "%%A"=="--install" set "INSTALL=true"
    if "%%A"=="--clean"   set "CLEAN=true"
)

if "%CLEAN%"=="true" (
    echo ^> Cleaning build directory...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

echo ^> Configuring...
cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release --log-level=WARNING
if errorlevel 1 goto :error

echo ^> Building...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 goto :error

echo.
echo Built Pax:
for %%F in ("%BUILD_DIR%\pax\Release\*.dll") do echo     %%~nxF

if "%INSTALL%"=="true" (
    echo.
    echo ^> Installing...
    cmake --install "%BUILD_DIR%" --prefix "%USERPROFILE%"
    echo.
    echo Pax installed to:
    echo     %APPDATA%\Patchy\Pax\
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
