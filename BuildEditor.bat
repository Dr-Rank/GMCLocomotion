@echo off
REM ============================================================
REM  BuildEditor.bat - Compile GMCGASP Editor via UnrealBuildTool
REM  Bypasses MSBuild/Visual Studio to avoid SetEnv length limit
REM ============================================================

setlocal

set ENGINE_DIR=D:\Games\UE_5.8
set PROJECT_FILE=F:\GitHub New\GMCGASP\GMCGASP.uproject
set TARGET=GMCGASPEditor
set PLATFORM=Win64
set CONFIG=Development

echo.
echo ============================================================
echo  Building %TARGET% (%PLATFORM% %CONFIG%)
echo  Engine: %ENGINE_DIR%
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
