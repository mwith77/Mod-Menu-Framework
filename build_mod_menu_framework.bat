@echo off
setlocal
rem Build mod_menu_framework.dll. Run from the "x64 Native Tools Command Prompt for VS".

rem Work in this script's own folder, wherever the prompt is.
cd /d "%~dp0"

where cl.exe >nul 2>nul || (echo cl.exe not found - use the x64 Native Tools prompt & exit /b 1)
where rc.exe >nul 2>nul || (echo rc.exe not found & exit /b 1)

rem The resources: the version (author, product and version) and the default
rem settings, mod_menu_framework.ini byte for byte.
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
exit /b 0
