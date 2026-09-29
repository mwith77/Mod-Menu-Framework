@echo off
rem Build mod_menu_framework.dll.
rem Run from "x64 Native Tools Command Prompt for VS".

where cl.exe >nul 2>nul || (echo cl.exe not found - use the x64 Native Tools prompt & exit /b 1)
where rc.exe >nul 2>nul || (echo rc.exe not found & exit /b 1)

rem The version resource, so the DLL carries its author, product and version.
rc /nologo /fo mod_menu_framework.res mod_menu_framework.rc
if errorlevel 1 (echo RC FAILED & exit /b 1)

rem mod_menu_framework.map turns an offset in a crash line back into a
rem function name.
cl /nologo /LD /O2 /EHsc /W4 /w14459 mod_menu_framework.cpp ^
   mod_menu_framework.res ^
   /link /OUT:mod_menu_framework.dll /MAP:mod_menu_framework.map
if errorlevel 1 (echo BUILD FAILED & exit /b 1)

del /q mod_menu_framework.obj mod_menu_framework.exp mod_menu_framework.lib ^
       mod_menu_framework.res 2>nul

echo.
echo Built mod_menu_framework.dll
