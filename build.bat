@echo off
REM ============================================================
REM CPB Build Script — Visual Studio Developer Command Prompt (x64)
REM ============================================================
echo.
echo Building CPB...
echo.

REM Main GUI app
echo [1/3] cpb.exe (main app)
cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO ^
   cpb_gui.cpp /Fe:cpb.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
   shell32.lib ole32.lib cabinet.lib
if %errorlevel% neq 0 ( echo FAILED: cpb.exe & exit /b 1 )

REM Dictionary trainer
echo [2/3] cpb_train.exe
cl /std:c++17 /O2 /EHsc /nologo /utf-8 ^
   cpb_train.cpp /Fe:cpb_train.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib
if %errorlevel% neq 0 ( echo FAILED: cpb_train.exe & exit /b 1 )

REM Archive browser
echo [3/3] cpb_reader.exe
cl /std:c++17 /O2 /EHsc /nologo /utf-8 /DCPB_NO_ZSTD /DNO_FRAME_IO ^
   cpb_reader.cpp /Fe:cpb_reader.exe /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comctl32.lib comdlg32.lib ^
   shell32.lib ole32.lib cabinet.lib
if %errorlevel% neq 0 ( echo FAILED: cpb_reader.exe & exit /b 1 )

echo.
echo ============================================================
echo All 3 apps built successfully!
echo   cpb.exe        — Main GUI (pack / unpack)
echo   cpb_train.exe  — Dictionary trainer
echo   cpb_reader.exe — Archive browser
echo ============================================================
