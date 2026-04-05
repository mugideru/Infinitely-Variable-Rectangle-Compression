@echo off
set Z_DIR=.\zstd
echo Compiling...

:: コンパイルだけを実行し、結果を ERRORLEVEL で判定する
clang-cl ivr_codec.c %Z_DIR%\common\*.c %Z_DIR%\compress\*.c %Z_DIR%\decompress\*.c ^
    /O3 /Ot /Oi /Gw /Gy /GL ^
    -mllvm -inline-threshold=1000 ^
    /arch:AVX2 ^
    /MT /I. /I"%Z_DIR%" /I"%Z_DIR%\common" /I"%Z_DIR%\compress" /I"%Z_DIR%\decompress" ^
    /Fe:ivr_converter.exe /utf-8 ^
    /link /LTCG

if %ERRORLEVEL% equ 0 (
    echo "Compilation Succeeded!"
) else (
    echo "[ERROR] Compilation failed."
)

pause