@echo off
del ZKY_win32.pdb 2>nul

set "CC=cl.exe"

where /q %CC% || call vcvars64.bat

%CC% -nologo -W4 -WX -Z7 -Oi -I"%VULKAN_SDK%\Include"^
 ZKY_win32.c kernel32.lib user32.lib ws2_32.lib winmm.lib vulkan-1.lib^
 -link -subsystem:windows -incremental:no -libpath:"%VULKAN_SDK%\Lib" || goto :end

if "%1"=="run" ( start ZKY_win32.exe
) else if "%1"=="debug" ( start windbgx ZKY_win32.exe
) else if "%1"=="doc" ( start qrenderdoc ZKY_win32.exe
) else if not "%1"=="" ( echo command "%1" not found & goto :end )

:end
del ZKY_win32.obj 2>nul
