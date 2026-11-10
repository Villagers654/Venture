@echo off
rem Builds venture_ngx.dll with MSVC (NGX's static library is MSVC-only): Venture NR's DLSS-NR.
rem   build.cmd <NVIDIA NGX SDK directory> [output directory]
setlocal
set "ROOT=%~dp0..\.."
set "NGX=%~1"
if "%NGX%"=="" (echo Usage: build.cmd ^<NGX SDK directory^> [output directory] & exit /b 1)
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%ROOT%\build\windows-release\bin"
set "VULKAN=%ROOT%\.deps\fetch\venture_vulkan_headers-src\include"
if not exist "%NGX%\include\nvsdk_ngx_vk.h" (echo NGX SDK not found: %NGX% & exit /b 1)
if not exist "%VULKAN%\vulkan\vulkan_core.h" (echo Vulkan headers not found: %VULKAN% & exit /b 1)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /O2 /MT /EHsc /LD /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  /I"%NGX%\include" /I"%VULKAN%" /I"%ROOT%\src\runtime" ^
  "%~dp0venture_ngx.cpp" /Fo"%OUT%\venture_ngx.obj" /Fe"%OUT%\venture_ngx.dll" ^
  /link "%NGX%\nvsdk_ngx_s.lib" advapi32.lib user32.lib
