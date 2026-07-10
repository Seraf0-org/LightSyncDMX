@echo off
setlocal

set "PLUGIN_DIR=%~dp0"
for %%I in ("%PLUGIN_DIR%.") do set "PLUGIN_DIR=%%~fI"

set "ENGINE_DIR=%~1"
if "%ENGINE_DIR%"=="" set "ENGINE_DIR=%UE_ENGINE_DIR%"
if "%ENGINE_DIR%"=="" set "ENGINE_DIR=C:\Program Files\Epic Games\UE_5.7"

set "RUNUAT=%ENGINE_DIR%\Engine\Build\BatchFiles\RunUAT.bat"
if not exist "%RUNUAT%" (
    echo RunUAT.bat was not found: "%RUNUAT%"
    echo Usage: BuildPlugin.bat [UE_ENGINE_DIR]
    exit /b 1
)

set "PLUGIN_FILE=%PLUGIN_DIR%\LightSyncDMX.uplugin"
for %%I in ("%PLUGIN_DIR%\..") do set "PACKAGE_DIR=%%~fI\PackagedPlugin"

call "%RUNUAT%" BuildPlugin -Plugin="%PLUGIN_FILE%" -Package="%PACKAGE_DIR%" -TargetPlatforms=Win64
exit /b %ERRORLEVEL%
