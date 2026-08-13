@echo off
set BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set CMAKE=%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
cd /d "D:\Projects\Tom and Jerry in War of the Whiskers (U)"
"%CMAKE%" -S port -B port/build -A Win32 >nul
if errorlevel 1 ( echo CONFIGURE FAILED & exit /b 1 )
"%CMAKE%" --build port/build --config Release --target tj_hybrid tj_loader
