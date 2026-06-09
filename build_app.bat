@echo off
REM ================================================================
REM CPB アプリ ビルドスクリプト (Visual Studio Developer Command Prompt)
REM 使い方: VS の「Developer Command Prompt」を開いてこのファイルを実行
REM ================================================================

setlocal

REM ── NuGet で WebView2 SDK を取得 ──────────────────────────────
where nuget >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [INFO] NuGet が見つかりません。nuget.exe をダウンロードします...
    powershell -Command "Invoke-WebRequest -Uri 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile 'nuget.exe'"
)

echo [CPB] WebView2 SDK を取得中...
nuget.exe install Microsoft.Web.WebView2 -OutputDirectory packages -NonInteractive -Verbosity quiet
if %ERRORLEVEL% neq 0 (
    echo [ERROR] WebView2 SDK の取得に失敗しました
    pause & exit /b 1
)

REM ── WebView2 のバージョン検出 ────────────────────────────────
for /d %%d in (packages\Microsoft.Web.WebView2.*) do set WV2_DIR=%%d
echo [CPB] WebView2: %WV2_DIR%

set WV2_INC=%WV2_DIR%\build\native\include
set WV2_LIB=%WV2_DIR%\build\native\x64\WebView2LoaderStatic.lib

REM ── WRL/WIL ヘッダー (Windows SDK 付属) ─────────────────────
REM Visual Studio の vcvarsall.bat で設定済みのはず

REM ── コンパイル ───────────────────────────────────────────────
echo [CPB] コンパイル中...

cl /std:c++17 /O2 /MT /EHsc ^
   /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE ^
   /DCPB_NO_ZSTD /DNO_FRAME_IO ^
   /I. /I"%WV2_INC%" ^
   /W3 /nologo ^
   cpb_app.cpp ^
   /Fe:cpb.exe ^
   /link /SUBSYSTEM:WINDOWS ^
   "%WV2_LIB%" ^
   user32.lib gdi32.lib ole32.lib oleaut32.lib ^
   shlwapi.lib version.lib shell32.lib ^
   /MANIFEST /MANIFESTUAC:"level='asInvoker' uiAccess='false'"

if %ERRORLEVEL% equ 0 (
    echo.
    echo [CPB] ビルド成功! cpb.exe が生成されました
    echo [CPB] cpb.exe だけを配布すれば動作します (インストール不要)
    echo.
    start "" cpb.exe
) else (
    echo [ERROR] ビルド失敗
    pause
)
