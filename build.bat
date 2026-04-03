@echo off
rem --- 1. コンパイルを実行 ---
echo Compiling...
cl ivr_codec.c zlib\*.c /O2 /MT /I. /Izlib /Fe:ivr_converter.exe /utf-8 && (
    rem --- 2. コンパイルが成功(exit code 0)した場合のみ実行 ---
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
    echo [ERROR] Compilation failed. Check the errors above.
)
pause