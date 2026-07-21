@echo off
REM ─── nRF7002 Build Script ───────────────────────────────────────────────────
REM This script sets nRF environment variables temporarily
REM Does NOT affect system PATH or local VS Code build
REM
REM Usage: build_nrf.bat <source_dir> <build_dir>
REM Example: build_nrf.bat C:\Workspace\nRFTest-Testing\CNR-Joyastic C:\Workspace\nRFTest-Testing\CNR-Joyastic\build

REM ─── Set nRF Toolchain Paths ────────────────────────────────────────────────
set TOOLCHAIN=C:\ncs\toolchains\936afb6332
set NCS_PATH=C:\ncs\v3.3.0

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

REM ─── Run West Build ─────────────────────────────────────────────────────────
echo.
echo Building nRF7002 firmware...
echo Source: %1
echo Build:  %2
echo.

cd %NCS_PATH%
%TOOLCHAIN%\opt\bin\python.exe %TOOLCHAIN%\opt\bin\Scripts\west.exe ^
  build -b nrf7002dk/nrf5340/cpuapp/ns ^
  %1 ^
  --sysbuild ^
  -d %2

REM ─── Check Build Result ─────────────────────────────────────────────────────
if %ERRORLEVEL% == 0 (
    echo.
    echo Build successful!
    echo Output files:
    dir %2\merged.hex /b 2>nul
    dir %2\merged_CPUNET.hex /b 2>nul
) else (
    echo.
    echo Build failed with error code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)
