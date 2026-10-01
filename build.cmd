@echo off
rem Builds MemStat.exe with MinGW-w64 gcc, no C runtime.
cd /d "%~dp0"
gcc -Os -s -nostdlib -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns ^
    -fno-asynchronous-unwind-tables -fno-ident -fno-stack-protector -mno-stack-arg-probe ^
    -Wall -Wno-missing-braces -e WinMainCRTStartup -Wl,--subsystem,windows,--gc-sections ^
    memstat.c -o MemStat.exe ^
    -lkernel32 -luser32 -lgdi32 -ladvapi32 -ldwmapi -ldxgi -lpdh
