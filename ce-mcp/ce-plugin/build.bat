@echo off
REM build.bat — Build ce-mcp-plugin-x64.dll with Zydis static disassembler
REM Requires: Visual Studio 2022 BuildTools (or VS 2022 Community/Pro/Enterprise)
REM Usage: Run from x64 Native Tools Command Prompt, or this script auto-detects.

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM ---- Auto-detect VS environment ----
if not defined VCINSTALLDIR (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"  2>nul
)
if not defined VCINSTALLDIR (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul
)
if not defined VCINSTALLDIR (
    echo ERROR: Cannot find Visual Studio 2022. Run from x64 Native Tools Command Prompt.
    exit /b 1
)

REM ---- SDK include paths ----
set SDK_ZYDIS=sdk\zydis\include
set SDK_ZYDIS_SRC=sdk\zydis\src

set INCLUDES=/I"%SDK_ZYDIS%" /I"."

REM ---- Collect all Zydis + Zycore source files ----
set ZYDIS_SRC=
for %%f in (%SDK_ZYDIS_SRC%\*.c) do set ZYDIS_SRC=!ZYDIS_SRC! "%%f"
for %%f in (%SDK_ZYDIS_SRC%\API\*.c) do set ZYDIS_SRC=!ZYDIS_SRC! "%%f"

REM ---- Plugin source files ----
set PLUGIN_SRC=plugin-core.c plugin-analyze.c plugin-scan.c plugin-gen.c plugin-debug.c

REM ---- Compile flags ----
set CFLAGS=/nologo /MT /O2 /GS- /W3 /DNDEBUG /D_WINDOWS /D_USRDLL /D_WIN32_WINNT=0x0601 ^
    /DZYDIS_STATIC_BUILD /DZYCORE_STATIC_BUILD ^
    %INCLUDES%

set LDFLAGS=/nologo /DLL /OPT:REF /OPT:ICF /NODEFAULTLIB:libcmt.lib

set LIBS=ws2_32.lib dbghelp.lib kernel32.lib user32.lib advapi32.lib

REM ---- Output ----
set OUT=ce-mcp-plugin-x64.dll
set PDB=ce-mcp-plugin-x64.pdb

echo.
echo ========================================
echo Building %OUT%
echo ========================================
echo.

REM ---- Compile all .c to .obj ----
set OBJS=
set FAILED=0

for %%s in (%PLUGIN_SRC% %ZYDIS_SRC%) do (
    set "src=%%~s"
    set "obj=%%~ns.obj"
    set OBJS=!OBJS! "!obj!"
    echo [CC] !src!
    cl.exe /c %CFLAGS% /Fo"!obj!" "!src!"
    if errorlevel 1 set FAILED=1
)

if %FAILED%==1 (
    echo.
    echo ERROR: Compilation failed.
    exit /b 1
)

REM ---- Link ----
echo.
echo [LD] %OUT%
link.exe %LDFLAGS% /OUT:"%OUT%" /PDB:"%PDB%" %OBJS% %LIBS%
if errorlevel 1 (
    echo ERROR: Link failed.
    exit /b 1
)

REM ---- Cleanup .obj ----
del /q *.obj 2>nul

echo.
echo ========================================
echo SUCCESS: %OUT% built.
echo ========================================
endlocal
