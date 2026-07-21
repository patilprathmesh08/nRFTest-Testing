@echo off
REM ─── nRF7002 Build Script v3 ────────────────────────────────────────────────
REM Copies source to C: drive first then builds
REM This is needed because west cannot build across different drives
REM Does NOT affect system PATH or local VS Code build
REM
REM Usage: build_nrf.bat <source_dir> <build_dir>

REM ─── Set nRF Toolchain Paths ────────────────────────────────────────────────
set TOOLCHAIN=C:\ncs\toolchains\936afb6332
set NCS_PATH=C:\ncs\v3.3.0
set C_SOURCE=C:\Workspace\nRF_build_source
set C_BUILD=C:\Workspace\nRF_build_output

REM ─── Set Environment Variables ──────────────────────────────────────────────
set PATH=%TOOLCHAIN%\opt\bin;%TOOLCHAIN%\opt\bin\Scripts;%PATH%
set ZEPHYR_BASE=%NCS_PATH%\zephyr
set ZEPHYR_TOOLCHAIN_VARIANT=zephyr
set ZEPHYR_SDK_INSTALL_DIR=%TOOLCHAIN%\opt\zephyr-sdk
set CMAKE_PREFIX_PATH=%TOOLCHAIN%\opt\zephyr-sdk

REM ─── Verify Python ──────────────────────────────────────────────────────────
echo Using Python: %TOOLCHAIN%\opt\bin\python.exe
%TOOLCHAIN%\opt\bin\python.exe --version

REM ─── Verify West ────────────────────────────────────────────────────────────
echo Using West: %TOOLCHAIN%\opt\bin\Scripts\west.exe
%TOOLCHAIN%\opt\bin\python.exe %TOOLCHAIN%\opt\bin\Scripts\west.exe --version

REM ─── Copy Source to C: Drive ────────────────────────────────────────────────
echo.
echo Copying source to C: drive...
if exist %C_SOURCE% rmdir /S /Q %C_SOURCE%
xcopy /E /I /Q %1 %C_SOURCE%
echo Source copied to %C_SOURCE%

REM ─── Clean old build ────────────────────────────────────────────────────────
if exist %C_BUILD% rmdir /S /Q %C_BUILD%

REM ─── Run West Build from NCS workspace ──────────────────────────────────────
echo.
echo Building nRF7002 firmware...
echo Source: %C_SOURCE%
echo Build:  %C_BUILD%
echo.

cd /d %NCS_PATH%
%TOOLCHAIN%\opt\bin\python.exe %TOOLCHAIN%\opt\bin\Scripts\west.exe ^
  build -b nrf7002dk/nrf5340/cpuapp/ns ^
  %C_SOURCE% ^
  --sysbuild ^
  -d %C_BUILD%

REM ─── Check Build Result ─────────────────────────────────────────────────────
if %ERRORLEVEL% == 0 (
    echo.
    echo Build successful!
    echo Output files:
    dir %C_BUILD%\merged.hex /b 2>nul
    dir %C_BUILD%\merged_CPUNET.hex /b 2>nul

    REM Copy hex files back to original build folder
    if not exist %2 mkdir %2
    copy %C_BUILD%\merged.hex %2\ 2>nul
    copy %C_BUILD%\merged_CPUNET.hex %2\ 2>nul
    echo Hex files copied to %2
) else (
    echo.
    echo Build failed with error code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)
