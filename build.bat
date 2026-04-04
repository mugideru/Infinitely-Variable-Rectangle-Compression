@echo off
rem --- 1. コンパイルを実行 ---
echo Compiling...

rem 変更点： "> nul" で通常ログを非表示にし、 "2>&1" を付けないことでエラーだけ表示させます
cl ivr_codec.c zlib\*.c /O2 /MT /I. /Izlib /Fe:ivr_converter.exe /utf-8 && (
    rem --- 2. コンパイルが成功した場合 ---
    echo.
    echo Compilation Succeeded! Running converter.exe...
    echo ----------------------------------------
    chcp 65001 > nul
    ivr_converter.exe
    echo ----------------------------------------
    echo Done.
) || (
    rem --- 3. エラーが出た場合 ---
    echo.
    echo [ERROR] Compilation failed.
    echo Please run "cl" command manually if you need to see details.
)
pause