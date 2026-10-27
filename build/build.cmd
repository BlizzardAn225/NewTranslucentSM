@echo off
rem TranslucentSM 一键构建（双击即可）—— 需要 MSVC + Windows SDK
cd /d "%~dp0.."
python build\build_adapted.py --out build_out
if errorlevel 1 (
    echo.
    echo 构建失败，看 build_out\build.log
    pause
    exit /b 1
)
echo.
echo 构建完成：build_out\StartTAP.dll、build_out\start.exe
pause
