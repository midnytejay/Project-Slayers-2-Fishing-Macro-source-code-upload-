@echo off
setlocal
where g++ >nul 2>nul
if %errorlevel%==0 goto :mingw
where cl >nul 2>nul
if %errorlevel%==0 goto :msvc
echo No compiler found.
echo Run this from a Visual Studio Developer Command Prompt,
echo or install MSYS2 MinGW-w64 and add g++ to PATH.
pause
exit /b 1

:mingw
g++ -std=c++17 -O3 -s -mwindows -static -municode main.cpp -o FishingHyperTracker.exe -lgdi32 -ldwmapi -lwinmm -lcomctl32 -lgdiplus -lole32
goto :done

:msvc
cl /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE main.cpp /link /SUBSYSTEM:WINDOWS /OUT:FishingHyperTracker.exe user32.lib gdi32.lib dwmapi.lib winmm.lib comctl32.lib gdiplus.lib ole32.lib

:done
if errorlevel 1 (
  echo Build failed.
  pause
  exit /b 1
)
echo Built FishingHyperTracker.exe
pause
