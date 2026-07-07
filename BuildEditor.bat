@echo off
REM ============================================================
REM  BuildEditor.bat - Compile GMCGASPEditor via UnrealBuildTool
REM
REM  User provides:
REM    1. Engine install folder  (e.g. D:\Games\UE_5.8)
REM    2. Project folder         (e.g. F:\GitHub New\GMCGASP)
REM
REM  The script auto-finds GMCGASP.uproject in the project
REM  folder and verifies the GMC plugin is reachable before
REM  building (checks project Plugins, then Engine Marketplace).
REM
REM  Paths are saved to BuildEditor.paths for reuse.
REM ============================================================

setlocal enabledelayedexpansion

set PATHS_FILE=%~dp0BuildEditor.paths
set ENGINE_DIR=
set PROJECT_DIR=
set TARGET=GMCGASPEditor
set PLATFORM=Win64
set CONFIG=Development

REM --- Load saved paths if they exist ---
if exist "%PATHS_FILE%" (
    for /f "usebackq tokens=1,* delims==" %%A in ("%PATHS_FILE%") do (
        if "%%A"=="ENGINE_DIR" set "ENGINE_DIR=%%B"
        if "%%A"=="PROJECT_DIR" set "PROJECT_DIR=%%B"
    )
)

REM --- Defaults for first run ---
if "%ENGINE_DIR%"=="" set "ENGINE_DIR=D:\Games\UE_5.8"
if "%PROJECT_DIR%"=="" set "PROJECT_DIR=F:\GitHub New\GMCGASP"

echo.
echo ============================================================
echo  Current paths:
echo    Engine:  %ENGINE_DIR%
echo    Project: %PROJECT_DIR%
echo ============================================================
echo.
echo  1) Change paths
echo  2) Build with current paths
echo.
set /p CHOICE="Select [1/2]: "

if "%CHOICE%"=="1" (
    echo.
    set /p "NEW_ENGINE=Engine install folder [%ENGINE_DIR%]: "
    if "!NEW_ENGINE!"=="" set "NEW_ENGINE=%ENGINE_DIR%"
    set "ENGINE_DIR=!NEW_ENGINE!"

    set /p "NEW_PROJECT=Project folder [%PROJECT_DIR%]: "
    if "!NEW_PROJECT!"=="" set "NEW_PROJECT=%PROJECT_DIR%"
    set "PROJECT_DIR=!NEW_PROJECT!"

    REM --- Save to file ---
    > "%PATHS_FILE%" (
        echo ENGINE_DIR=!ENGINE_DIR!
        echo PROJECT_DIR=!PROJECT_DIR!
    )

    echo.
    echo  Paths saved to BuildEditor.paths
)

REM ============================================================
REM  Find GMCGASP.uproject in the project folder
REM ============================================================
set "PROJECT_FILE=%PROJECT_DIR%\GMCGASP.uproject"

if not exist "%PROJECT_FILE%" (
    echo.
    echo ============================================================
    echo  ERROR: Could not find GMCGASP.uproject in:
    echo    %PROJECT_DIR%
    echo.
    echo  Make sure the project folder contains GMCGASP.uproject.
    echo ============================================================
    pause
    exit /b 1
)

REM ============================================================
REM  Verify the engine path looks valid
REM ============================================================
if not exist "%ENGINE_DIR%\Engine\Build\BatchFiles\Build.bat" (
    echo.
    echo ============================================================
    echo  ERROR: Could not find Build.bat in:
    echo    %ENGINE_DIR%\Engine\Build\BatchFiles\
    echo.
    echo  Make sure the engine path points to a valid UE install
    echo  (e.g. D:\Games\UE_5.8).
    echo ============================================================
    pause
    exit /b 1
)

REM ============================================================
REM  Check for the GMC plugin
REM  1) Project Plugins folder  (any subfolder with GMC.uplugin)
REM  2) Engine Marketplace      (any subfolder with GMC.uplugin)
REM ============================================================
set GMC_FOUND=0
set GMC_LOCATION=

REM --- Check project plugins ---
if exist "%PROJECT_DIR%\Plugins" (
    for /d %%D in ("%PROJECT_DIR%\Plugins\*") do (
        if exist "%%D\GMC.uplugin" (
            set GMC_FOUND=1
            set "GMC_LOCATION=%%D"
        )
    )
)

REM --- Check engine marketplace ---
if !GMC_FOUND!==0 (
    set "MARKETPLACE_DIR=%ENGINE_DIR%\Engine\Plugins\Marketplace"
    if exist "!MARKETPLACE_DIR!" (
        for /d %%D in ("!MARKETPLACE_DIR!\*") do (
            if exist "%%D\GMC.uplugin" (
                set GMC_FOUND=1
                set "GMC_LOCATION=%%D"
            )
        )
    )
)

if !GMC_FOUND!==0 (
    echo.
    echo ============================================================
    echo  WARNING: Could not find the GMC plugin!
    echo.
    echo  Searched:
    echo    - %PROJECT_DIR%\Plugins\*\GMC.uplugin
    echo    - %ENGINE_DIR%\Engine\Plugins\Marketplace\*\GMC.uplugin
    echo.
    echo  The build will likely fail with:
    echo    "Unable to find plugin 'GMC'"
    echo.
    echo  To fix this, install GMC v2 from the Epic Marketplace,
    echo  or copy the GMC plugin folder into:
    echo    %PROJECT_DIR%\Plugins\
    echo ============================================================
    echo.
    set /p "CONTINUE=Continue anyway? [y/N]: "
    if /i not "!CONTINUE!"=="y" (
        exit /b 1
    )
) else (
    echo.
    echo  GMC plugin found: !GMC_LOCATION!
)

REM ============================================================
REM  Build
REM ============================================================
echo.
echo ============================================================
echo  Building %TARGET% (%PLATFORM% %CONFIG%)
echo  Engine:  %ENGINE_DIR%
echo  Project: %PROJECT_FILE%
echo ============================================================
echo.

call "%ENGINE_DIR%\Engine\Build\BatchFiles\Build.bat" %TARGET% %PLATFORM% %CONFIG% -Project="%PROJECT_FILE%" -WaitMutex -FromMsBuild %*

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ============================================================
    echo  BUILD FAILED  (exit code %ERRORLEVEL%)
    echo ============================================================
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo ============================================================
echo  BUILD SUCCEEDED
echo ============================================================
pause
