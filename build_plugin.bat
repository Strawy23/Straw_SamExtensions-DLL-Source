@echo off
setlocal EnableExtensions EnableDelayedExpansion


cd /d "%~dp0"

set "PLUGIN_NAME=Straw_SamExtensions"
set "GENERATOR=Visual Studio 18 2026"
set "ARCH=x64"
set "CONFIG=Release"
set "BUILD_DIR=%TEMP%\Straw_SamExtensions_build_vs2026_x64"
set "DIST_DIR=dist"
set "DIST_DLL=%DIST_DIR%\Data\F4SE\Plugins\%PLUGIN_NAME%.dll"

set "DEFAULT_FALLOUT4_DIR=D:\Steam\steamapps\common\Fallout 4"
set "DEFAULT_F4SE_SDK=%DEFAULT_FALLOUT4_DIR%\src"
set "LOCAL_F4SE_SDK=%CD%\external\f4se_sdk"

echo.
echo ============================================================
echo  %PLUGIN_NAME% - Visual Studio 2026 x64 Release build
echo ============================================================
echo.
echo [info] Project folder: %CD%
echo [info] Build folder:   %BUILD_DIR%
echo [info] Dist DLL:       %DIST_DLL%
echo [info] Default FO4:    %DEFAULT_FALLOUT4_DIR%
echo [info] Default SDK:    %DEFAULT_F4SE_SDK%
echo.

if not exist "CMakeLists.txt" (
    echo [ERROR] CMakeLists.txt was not found.
    echo         Run this BAT from Native\Straw_SamExtensions.
    echo.
    pause
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake was not found in PATH.
    echo         Install CMake and reopen Command Prompt/Explorer.
    echo.
    pause
    exit /b 1
)

echo [info] CMake version:
cmake --version
echo.

if "%F4SE_SDK%"=="" (
    if exist "%DEFAULT_F4SE_SDK%" (
        set "F4SE_SDK=%DEFAULT_F4SE_SDK%"
    ) else if exist "%LOCAL_F4SE_SDK%" (
        set "F4SE_SDK=%LOCAL_F4SE_SDK%"
    )
)

if "%F4SE_SDK%"=="" (
    echo [ERROR] F4SE_SDK is not set and no candidate folder was found.
    echo.
    echo Expected one of these:
    echo   1. Default SDK folder:
    echo      %DEFAULT_F4SE_SDK%
    echo.
    echo   2. Or local SDK folder:
    echo      %LOCAL_F4SE_SDK%
    echo.
    echo   3. Or set it manually before running this BAT:
    echo      set F4SE_SDK=D:\Steam\steamapps\common\Fallout 4\src
    echo.
    pause
    exit /b 1
)

if not exist "%F4SE_SDK%" (
    echo [ERROR] F4SE_SDK points to a folder that does not exist:
    echo   %F4SE_SDK%
    echo.
    pause
    exit /b 1
)

echo [info] F4SE_SDK candidate:
echo        %F4SE_SDK%
echo.
echo [info] Searching for PluginAPI.h under the SDK candidate...
set "PLUGIN_API_FOUND="
for /f "delims=" %%F in ('dir /b /s "%F4SE_SDK%\PluginAPI.h" 2^>nul') do (
    set "PLUGIN_API_FOUND=%%F"
    goto :FoundPluginApi
)
:FoundPluginApi
if "%PLUGIN_API_FOUND%"=="" (
    echo [ERROR] Could not find PluginAPI.h anywhere below:
    echo   %F4SE_SDK%
    echo.
    echo Your screenshot shows folders like src\f4se and src\f4se_common.
    echo Please open src\f4se and confirm PluginAPI.h exists somewhere inside the src tree.
    echo.
    pause
    exit /b 1
)
echo [info] Found PluginAPI.h:
echo        %PLUGIN_API_FOUND%
echo.

if "%FALLOUT4_DIR%"=="" (
    if exist "%DEFAULT_FALLOUT4_DIR%\Fallout4.exe" (
        set "FALLOUT4_DIR=%DEFAULT_FALLOUT4_DIR%"
    )
)

echo [info] Using generator: %GENERATOR%
echo [info] Architecture:    %ARCH%
echo.

if exist "%BUILD_DIR%" (
    echo [info] Removing old %BUILD_DIR% folder...
    rmdir /s /q "%BUILD_DIR%"
    if exist "%BUILD_DIR%" (
        echo [ERROR] Could not remove %BUILD_DIR%.
        echo         Close Visual Studio/CMake/MSBuild windows using it and try again.
        echo.
        pause
        exit /b 1
    )
)

if exist "%DIST_DIR%" (
    echo [info] Removing old %DIST_DIR% folder...
    rmdir /s /q "%DIST_DIR%"
    if exist "%DIST_DIR%" (
        echo [ERROR] Could not remove %DIST_DIR%.
        echo.
        pause
        exit /b 1
    )
)

echo.
echo [step 1/2] Configuring CMake...
cmake -S . -B "%BUILD_DIR%" -G "%GENERATOR%" -A %ARCH% -DF4SE_SDK="%F4SE_SDK%"
if errorlevel 1 (
    echo.
    echo [ERROR] CMake configure failed.
    echo.
    echo Common fixes:
    echo   1. Install Desktop development with C++ in Visual Studio Installer.
    echo   2. Install a Windows 10/11 SDK.
    echo   3. Make sure your CMake supports generator:
    echo      %GENERATOR%
    echo   4. Check with:
    echo      cmake --help
    echo.
    pause
    exit /b 1
)

echo.
echo [step 2/2] Building %CONFIG%...
cmake --build "%BUILD_DIR%" --config %CONFIG%
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed.
    echo         Scroll up and look for the first compiler/linker error.
    echo.
    pause
    exit /b 1
)

echo.
echo [info] Searching for built %PLUGIN_NAME%.dll...
set "DLL_PATH="
for /f "delims=" %%F in ('dir /b /s "%BUILD_DIR%\%PLUGIN_NAME%.dll" 2^>nul') do (
    set "DLL_PATH=%%F"
)

if not defined DLL_PATH (
    echo.
    echo [ERROR] Build completed, but %PLUGIN_NAME%.dll was not found.
    echo         Check the CMake target name/output folder.
    echo.
    pause
    exit /b 1
)

echo [info] Found DLL:
echo        %DLL_PATH%

if not exist "%DIST_DIR%\Data\F4SE\Plugins" mkdir "%DIST_DIR%\Data\F4SE\Plugins"
copy /y "%DLL_PATH%" "%DIST_DLL%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy DLL to:
    echo   %DIST_DLL%
    echo.
    pause
    exit /b 1
)

if exist "%DLL_PATH:~0,-4%.pdb" (
    copy /y "%DLL_PATH:~0,-4%.pdb" "%DIST_DIR%\Data\F4SE\Plugins\%PLUGIN_NAME%.pdb" >nul
)

echo.
echo ============================================================
echo  Build succeeded.
echo.
echo  Output:
echo    %CD%\%DIST_DLL%
echo.
echo  Install this file to:
echo    Fallout 4\Data\F4SE\Plugins\%PLUGIN_NAME%.dll
echo ============================================================
echo.

if not "%FALLOUT4_DIR%"=="" (
    if exist "%FALLOUT4_DIR%\Fallout4.exe" (
        echo [info] FALLOUT4_DIR detected. Copying DLL into your Fallout 4 install...
        if not exist "%FALLOUT4_DIR%\Data\F4SE\Plugins" mkdir "%FALLOUT4_DIR%\Data\F4SE\Plugins"
        copy /y "%CD%\%DIST_DLL%" "%FALLOUT4_DIR%\Data\F4SE\Plugins\%PLUGIN_NAME%.dll" >nul
        if errorlevel 1 (
            echo [ERROR] Auto-copy failed.
            pause
            exit /b 1
        )
        echo [info] Copied to:
        echo        %FALLOUT4_DIR%\Data\F4SE\Plugins\%PLUGIN_NAME%.dll
        echo.
    ) else (
        echo [warn] FALLOUT4_DIR is set, but Fallout4.exe was not found there.
        echo        Skipping auto-copy.
        echo.
    )
)

pause
exit /b 0
