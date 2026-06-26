@REM Snapmaker_Orca Windows release build using MSVC + Ninja for the app.
@REM
@REM This keeps the existing VS2022 dependency build path, then configures the
@REM main project with Ninja in separate build-ninja* directories.
@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "WP=%~dp0"
set "WP=%WP:~0,-1%"

set "mode=all"
set "debug=OFF"
set "debuginfo=OFF"
set "dryrun=OFF"
set "jobs="

if "%~1"=="" goto :args_done

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="help" goto :usage
if /I "%~1"=="-h" goto :usage
if /I "%~1"=="/?" goto :usage
if /I "%~1"=="deps" set "mode=deps"
if /I "%~1"=="slicer" set "mode=slicer"
if /I "%~1"=="configure" set "mode=configure"
if /I "%~1"=="config" set "mode=configure"
if /I "%~1"=="pack" set "mode=pack"
if /I "%~1"=="debug" set "debug=ON"
if /I "%~1"=="debuginfo" set "debuginfo=ON"
if /I "%~1"=="dryrun" set "dryrun=ON"
echo %~1 | findstr /R /I "^jobs=[0-9][0-9]*$" >nul
if not errorlevel 1 for /F "tokens=2 delims==" %%J in ("%~1") do set "jobs=%%J"
shift
goto :parse_args

:args_done
if "%debug%"=="ON" (
    set "build_type=Debug"
    set "build_dir=build-ninja-dbg"
    set "vs_deps_build_dir=deps\build-dbg"
) else (
    if "%debuginfo%"=="ON" (
        set "build_type=RelWithDebInfo"
        set "build_dir=build-ninja-dbginfo"
        set "vs_deps_build_dir=deps\build-dbginfo"
    ) else (
        set "build_type=Release"
        set "build_dir=build-ninja"
        set "vs_deps_build_dir=deps\build"
    )
)

set "app_build_dir=%WP%\%build_dir%"
set "deps_root=%WP%\%vs_deps_build_dir%\OrcaSlicer_dep"
set "deps_prefix=%deps_root%\usr\local"
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

echo build type set to %build_type%
echo main build dir: %app_build_dir%
echo deps prefix:    %deps_prefix%

if /I "%mode%"=="pack" goto :pack_deps

call :ensure_tools
if errorlevel 1 exit /b %ERRORLEVEL%

if /I "%mode%"=="slicer" goto :slicer
if /I "%mode%"=="configure" goto :slicer

call :build_deps
if errorlevel 1 exit /b %ERRORLEVEL%
if /I "%mode%"=="deps" exit /b 0

:slicer
if not exist "%deps_prefix%\include" (
    echo.
    echo Missing deps include directory:
    echo   %deps_prefix%\include
    echo Build deps first with:
    echo   %~nx0 deps
    exit /b 1
)
if not exist "%deps_prefix%\lib" (
    echo.
    echo Missing deps lib directory:
    echo   %deps_prefix%\lib
    echo Build deps first with:
    echo   %~nx0 deps
    exit /b 1
)

echo.
echo building Snapmaker Orca with Ninja...

set "SDK_ARG="
if not defined WindowsSdkDir goto :sdk_arg_done
if not defined WindowsSDKVersion goto :sdk_arg_done
set "SDK_VERSION=%WindowsSDKVersion%"
if "%SDK_VERSION:~-1%"=="\" set "SDK_VERSION=%SDK_VERSION:~0,-1%"
set "SDK_ARG=-DWIN10SDK_PATH=%WindowsSdkDir%Include\%SDK_VERSION%"
:sdk_arg_done

if defined SDK_ARG (
    call :run cmake -S "%WP%" -B "%app_build_dir%" -G Ninja "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%" -DBBL_RELEASE_TO_PUBLIC=1 -DORCA_TOOLS=ON -DSLIC3R_PCH=ON -DSLIC3R_MSVC_COMPILE_PARALLEL=ON %SIG_FLAG% "-DCMAKE_PREFIX_PATH=%deps_prefix%" "-DCMAKE_INSTALL_PREFIX=%app_build_dir%\Snapmaker_Orca" -DCMAKE_BUILD_TYPE=%build_type% "%SDK_ARG%"
) else (
    call :run cmake -S "%WP%" -B "%app_build_dir%" -G Ninja "-DCMAKE_MAKE_PROGRAM=%NINJA_EXE%" -DBBL_RELEASE_TO_PUBLIC=1 -DORCA_TOOLS=ON -DSLIC3R_PCH=ON -DSLIC3R_MSVC_COMPILE_PARALLEL=ON %SIG_FLAG% "-DCMAKE_PREFIX_PATH=%deps_prefix%" "-DCMAKE_INSTALL_PREFIX=%app_build_dir%\Snapmaker_Orca" -DCMAKE_BUILD_TYPE=%build_type%
)
if errorlevel 1 exit /b 1

if /I "%mode%"=="configure" (
    echo.
    echo Configured Ninja build files under:
    echo   %app_build_dir%
    exit /b 0
)

if defined jobs (
    call :run cmake --build "%app_build_dir%" --parallel %jobs%
) else (
    call :run cmake --build "%app_build_dir%" --parallel
)
if errorlevel 1 exit /b %ERRORLEVEL%

call :run call "%WP%\scripts\run_gettext.bat"
if errorlevel 1 exit /b %ERRORLEVEL%

if defined jobs (
    call :run cmake --build "%app_build_dir%" --target install --parallel %jobs%
) else (
    call :run cmake --build "%app_build_dir%" --target install --parallel
)
if errorlevel 1 exit /b %ERRORLEVEL%

echo.
echo Done. Installed build is under:
echo   %app_build_dir%\Snapmaker_Orca
exit /b 0

:build_deps
echo.
echo building deps with existing VS2022 dependency script...
if "%debug%"=="ON" (
    call :run call "%WP%\build_release_vs2022.bat" deps debug
    if errorlevel 1 exit /b 1
) else (
    if "%debuginfo%"=="ON" (
        call :run call "%WP%\build_release_vs2022.bat" deps debuginfo
        if errorlevel 1 exit /b 1
    ) else (
        call :run call "%WP%\build_release_vs2022.bat" deps
        if errorlevel 1 exit /b 1
    )
)
exit /b 0

:ensure_tools
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"

where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo CMake was not found on PATH.
    exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    call :setup_vsdevcmd
    if errorlevel 1 exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo cl.exe was not found after setting up the VS2022 developer environment.
    exit /b 1
)

if not defined NINJA_EXE (
    for /F "delims=" %%N in ('where ninja.exe 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%N"
)
if not defined NINJA_EXE (
    echo ninja.exe was not found on PATH.
    echo Install Ninja or run:
    echo   winget install Ninja-build.Ninja
    exit /b 1
)

echo using ninja: %NINJA_EXE%
exit /b 0

:setup_vsdevcmd
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
    for /F "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%V"
)

if defined VSINSTALL if exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" (
    call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    exit /b 0
)

if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    exit /b 0
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    exit /b 0
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    exit /b 0
)
if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    exit /b 0
)

echo Could not find VS2022 VsDevCmd.bat.
echo Run this script from an x64 Native Tools Command Prompt for VS 2022, or install the VS2022 C++ tools.
exit /b 1

:pack_deps
setlocal EnableDelayedExpansion
set "pack_dir=%WP%\%vs_deps_build_dir%"
if not exist "!pack_dir!\OrcaSlicer_dep" (
    echo Missing deps directory:
    echo   !pack_dir!\OrcaSlicer_dep
    exit /b 1
)
pushd "!pack_dir!"
for /F "tokens=2-4 delims=/ " %%a in ('date /t') do set "build_date=%%c%%b%%a"
if not defined build_date set "build_date=unknown-date"
echo packing deps: OrcaSlicer_dep_win64_!build_date!_vs2022.zip
"%WP%\tools\7z.exe" a "OrcaSlicer_dep_win64_!build_date!_vs2022.zip" OrcaSlicer_dep
set "pack_result=!ERRORLEVEL!"
popd
exit /b !pack_result!

:run
echo.
echo ^> %*
if /I "%dryrun%"=="ON" (
    echo ^(dry run^)
    exit /b 0
)
%*
exit /b %ERRORLEVEL%

:usage
echo Usage: %~nx0 [deps^|slicer^|configure^|pack] [debug^|debuginfo] [jobs=N] [dryrun]
echo.
echo   %~nx0                 Build deps with VS2022, then build and install app with Ninja.
echo   %~nx0 deps            Build only deps with the existing VS2022 dependency script.
echo   %~nx0 slicer          Build and install only the main app with Ninja.
echo   %~nx0 configure       Configure the Ninja app build without compiling.
echo   %~nx0 slicer jobs=16  Limit Ninja parallelism.
echo   %~nx0 slicer dryrun   Print commands without running them.
exit /b 0
