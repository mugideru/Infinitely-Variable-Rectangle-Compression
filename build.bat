@echo off
set Z_DIR=.\zstd
echo Compiling...

cl ivr_codec.c %Z_DIR%\common\*.c %Z_DIR%\compress\*.c %Z_DIR%\decompress\*.c /O2 /MT /I. /I"%Z_DIR%" /I"%Z_DIR%\common" /I"%Z_DIR%\compress" /I"%Z_DIR%\decompress" /Fe:ivr_converter.exe /utf-8 && (
    echo Compilation Succeeded!
    chcp 65001 > nul
    ivr_converter.exe input.bmp output.ivr 1 1
) || (
    echo [ERROR] Compilation failed.
)
pause