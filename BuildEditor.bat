@echo off
REM ============================================================
REM  BuildEditor.bat - Compile GMCGASPEditor via UnrealBuildTool
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
if "!ENGINE_DIR!"=="" set "ENGINE_DIR=D:\Games\UE_5.8"
if "!PROJECT_DIR!"=="" set "PROJECT_DIR=F:\GitHub New\GMCLocomotion"

echo.
echo ============================================================
echo  Current paths:
echo    Engine:  !ENGINE_DIR!
echo    Project: !PROJECT_DIR!
echo ============================================================
echo.
echo  1) Change paths
echo  2) Build with current paths
echo.
set /p CHOICE="Select [1/2]: "

if "!CHOICE!"=="1" (
    echo.
    set /p "NEW_ENGINE=Engine install folder [!ENGINE_DIR!]: "
    if "!NEW_ENGINE!"=="" set "NEW_ENGINE=!ENGINE_DIR!"
    set "ENGINE_DIR=!NEW_ENGINE!"

    set /p "NEW_PROJECT=Project folder [!PROJECT_DIR!]: "
    if "!NEW_PROJECT!"=="" set "NEW_PROJECT=!PROJECT_DIR!"
    set "PROJECT_DIR=!NEW_PROJECT!"

    > "!PATHS_FILE!" (
        echo ENGINE_DIR=!ENGINE_DIR!
        echo PROJECT_DIR=!PROJECT_DIR!
    )

    echo.
    echo  Paths saved to BuildEditor.paths
)

REM ============================================================
REM  Validate paths
REM ============================================================
set "PROJECT_FILE=!PROJECT_DIR!\GMCGASP.uproject"

if not exist "!PROJECT_FILE!" (
    echo.
    echo  ERROR: Could not find GMCGASP.uproject in:
    echo    !PROJECT_DIR!
    echo.
    pause
    exit /b 1
)

if not exist "!ENGINE_DIR!\Engine\Build\BatchFiles\Build.bat" (
    echo.
    echo  ERROR: Could not find Build.bat in:
    echo    !ENGINE_DIR!\Engine\Build\BatchFiles\
    echo.
    pause
    exit /b 1
)

REM ============================================================
REM  Check for the GMC plugin (simple dir search)
REM ============================================================
set GMC_FOUND=0

dir /s /b "!PROJECT_DIR!\Plugins\GMC.uplugin" >nul 2>nul
if !ERRORLEVEL! EQU 0 (
    echo  GMC plugin found in project Plugins folder.
    set GMC_FOUND=1
)

if "!GMC_FOUND!"=="0" (
    dir /s /b "!ENGINE_DIR!\Engine\Plugins\Marketplace\GMC.uplugin" >nul 2>nul
    if !ERRORLEVEL! EQU 0 (
        echo  GMC plugin found in Engine Marketplace folder.
        set GMC_FOUND=1
    )
)

if "!GMC_FOUND!"=="0" (
    echo.
    echo  WARNING: Could not find the GMC plugin.
    echo  Searched project Plugins and Engine Marketplace.
    echo  The build may fail with "Unable to find plugin 'GMC'".
    echo.
    echo  Install GMC v2 from the Epic Marketplace, or copy it to:
    echo    !PROJECT_DIR!\Plugins\
    echo.
    set /p "CONTINUE=Continue anyway? [y/N]: "
    if /i not "!CONTINUE!"=="y" (
        pause
        exit /b 1
    )
)

REM ============================================================
REM  Build
REM ============================================================
echo.
echo ============================================================
echo  Building %TARGET% (%PLATFORM% %CONFIG%)
echo  Engine:  !ENGINE_DIR!
echo  Project: !PROJECT_FILE!
echo ============================================================
echo.

call "!ENGINE_DIR!\Engine\Build\BatchFiles\Build.bat" %TARGET% %PLATFORM% %CONFIG% -Project="!PROJECT_FILE!" -WaitMutex -FromMsBuild %*

if !ERRORLEVEL! NEQ 0 (
    echo.
    echo  BUILD FAILED  (exit code !ERRORLEVEL!)
    pause
    exit /b !ERRORLEVEL!
)

echo.
echo  BUILD SUCCEEDED
pause
