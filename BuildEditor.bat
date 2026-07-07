@echo off
REM ============================================================
REM  BuildEditor.bat - Compile GMCGASP Editor via UnrealBuildTool
REM  Stores engine/project paths in BuildEditor.paths for reuse.
REM ============================================================

setlocal enabledelayedexpansion

set PATHS_FILE=%~dp0BuildEditor.paths
set ENGINE_DIR=
set PROJECT_FILE=
set TARGET=GMCGASPEditor
set PLATFORM=Win64
set CONFIG=Development

REM --- Load saved paths if they exist ---
if exist "%PATHS_FILE%" (
    for /f "usebackq tokens=1,* delims==" %%A in ("%PATHS_FILE%") do (
        if "%%A"=="ENGINE_DIR" set "ENGINE_DIR=%%B"
        if "%%A"=="PROJECT_FILE" set "PROJECT_FILE=%%B"
    )
)

REM --- Defaults for first run ---
if "%ENGINE_DIR%"=="" set "ENGINE_DIR=D:\Games\UE_5.8"
if "%PROJECT_FILE%"=="" set "PROJECT_FILE=F:\GitHub New\GMCGASP\GMCGASP.uproject"

echo.
echo ============================================================
echo  Current paths:
echo    Engine:  %ENGINE_DIR%
echo    Project: %PROJECT_FILE%
echo ============================================================
echo.
echo  1) Change paths
echo  2) Build with current paths
echo.
set /p CHOICE="Select [1/2]: "

if "%CHOICE%"=="1" (
    echo.
    set /p "NEW_ENGINE=Engine install path [%ENGINE_DIR%]: "
    if "!NEW_ENGINE!"=="" set "NEW_ENGINE=%ENGINE_DIR%"
    set "ENGINE_DIR=!NEW_ENGINE!"

    set /p "NEW_PROJECT=Project file path [%PROJECT_FILE%]: "
    if "!NEW_PROJECT!"=="" set "NEW_PROJECT=%PROJECT_FILE%"
    set "PROJECT_FILE=!NEW_PROJECT!"

    REM --- Save to file ---
    > "%PATHS_FILE%" (
        echo ENGINE_DIR=!ENGINE_DIR!
        echo PROJECT_FILE=!PROJECT_FILE!
    )

    echo.
    echo  Paths saved to BuildEditor.paths
)

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
