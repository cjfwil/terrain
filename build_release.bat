@echo off
setlocal

:: Simple config
set SRC=main.cpp
set OUTDIR=release
set OUTNAME=engine.exe

:: Minimal release flags: optimize, disable debug, use DLL CRT
set CLFLAGS=/O2 /DNDEBUG /MD /W3 /EHsc

:: Ensure output dir
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

:: Compile+link in one step
echo Building %SRC% ...
cl %CLFLAGS% "%SRC%" /Fe"%OUTDIR%\%OUTNAME%" /link /VERBOSE > link_verbose.txt 2>&1
findstr /I "LIBCMT" link_verbose.txt

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

:: Copy runtime assets
if exist "shaders.hlsl" copy /Y "shaders.hlsl" "%OUTDIR%\"
if exist "sky.hlsl" copy /Y "sky.hlsl" "%OUTDIR%\"
if exist "ConstantBuffer.hlsl" copy /Y "ConstantBuffer.hlsl" "%OUTDIR%\"
if exist "shaders_baked_heightmap.hlsl" copy /Y "shaders_baked_heightmap.hlsl" "%OUTDIR%\"
@REM if exist "gravel.dds" copy /Y "gravel.dds" "%OUTDIR%\"
@REM if exist "heightmap.png" copy /Y "heightmap.png" "%OUTDIR%\"
@REM if exist "greece_heightmap.dds" copy /Y "greece_heightmap.dds" "%OUTDIR%\"
@REM if exist "greece_albedo.dds" copy /Y "greece_albedo.dds" "%OUTDIR%\"
:: Copy entire data folder recursively
if exist "data" (
    robocopy "data" "%OUTDIR%\data" /E /NFL /NDL /NJH /NJS /NP
)



:: Copy SDL runtime DLLs from System32
if exist "%WINDIR%\System32\SDL3.dll" copy /Y "%WINDIR%\System32\SDL3.dll" "%OUTDIR%\"
if exist "%WINDIR%\System32\SDL3_image.dll" copy /Y "%WINDIR%\System32\SDL3_image.dll" "%OUTDIR%\"
if exist "%WINDIR%\System32\SDL3_ttf.dll" copy /Y "%WINDIR%\System32\SDL3_ttf.dll" "%OUTDIR%\"

:: Copy DXC (DirectX Shader Compiler)
:: if exist "%WINDIR%\System32\dxcompiler.dll" copy /Y "%WINDIR%\System32\dxcompiler.dll" "%OUTDIR%\"



echo Build succeeded. Output: %OUTDIR%\%OUTNAME%
endlocal

release\engine.exe