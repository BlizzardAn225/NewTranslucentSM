"""TranslucentSM 25H2 适配版构建脚本（无需 VS 工程 / NuGet）。

只需要：
  - Visual Studio Build Tools（MSVC v143+，含 C++ 工具集）
  - Windows 10/11 SDK（含 cppwinrt 头文件，10.0.22621+ 即可）

用法：
  python build\\build_adapted.py
  python build\\build_adapted.py --out D:\\some\\dir     # 指定输出目录

产出：StartTAP.dll、start.exe（x64，静态 CRT，无第三方依赖）
"""
import argparse, os, shutil, subprocess, sys

SRC_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(os.environ.get("TEMP", "."), "tsm_build")

def find_vcvars():
    env = os.environ.get("VCVARS")
    if env and os.path.exists(env):
        return env
    roots = [
        r"C:\Program Files (x86)\Microsoft Visual Studio",
        r"C:\Program Files\Microsoft Visual Studio",
    ]
    candidates = []
    for root in roots:
        if not os.path.isdir(root):
            continue
        for edition in os.listdir(root):
            for flavor in ("BuildTools", "Community", "Professional", "Enterprise"):
                p = os.path.join(root, edition, flavor, r"VC\Auxiliary\Build\vcvars64.bat")
                if os.path.exists(p):
                    candidates.append(p)
    return sorted(candidates, reverse=True)[0] if candidates else None

def find_sdk_include():
    kits = r"C:\Program Files (x86)\Windows Kits\10\Include"
    if os.environ.get("WINSDK_INCLUDE") and os.path.isdir(os.environ["WINSDK_INCLUDE"]):
        return os.environ["WINSDK_INCLUDE"]
    if not os.path.isdir(kits):
        return None
    versions = sorted((d for d in os.listdir(kits) if d.startswith("10.")), reverse=True)
    for v in versions:
        p = os.path.join(kits, v, "cppwinrt")
        if os.path.isdir(p):
            return os.path.join(kits, v)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(SRC_ROOT, "build_out"))
    args = ap.parse_args()

    vcvars = find_vcvars()
    sdkinc = find_sdk_include()
    if not vcvars:
        sys.exit("找不到 vcvars64.bat，可用环境变量 VCVARS 指定")
    if not sdkinc:
        sys.exit("找不到 Windows SDK 的 cppwinrt 头文件，可用环境变量 WINSDK_INCLUDE 指定")
    print("vcvars :", vcvars)
    print("sdk    :", sdkinc)

    os.makedirs(WORK, exist_ok=True)
    for name in ("dllmain.cpp", "VisualTreeWatcher.cpp", "VisualTreeWatcher.h",
                 "Helpers.h", "framework.h", "exports.def", "start.cpp", "resource.h"):
        sub = "start" if name in ("start.cpp", "resource.h") else "StartTAP"
        shutil.copyfile(os.path.join(SRC_ROOT, sub, name), os.path.join(WORK, name))

    bat = os.path.join(WORK, "build.cmd")
    with open(bat, "w", encoding="ascii") as f:
        f.write("@echo off\n")
        f.write(f'call "{vcvars}" >nul 2>&1\n')
        f.write("if errorlevel 1 ( echo VCVARS FAILED & exit /b 1 )\n")
        f.write(f'cd /d "{WORK}"\n')
        f.write("del /q StartTAP.dll start.exe 2>nul\n")
        # StartTAP.dll (x64)：C++/WinRT 只用 Windows SDK 自带头文件
        # /wd4828：屏蔽源文件编码提示（无害但会刷屏）
        f.write("cl /nologo /std:c++20 /utf-8 /EHsc /O2 /MT /W3 /wd4828 /DUNICODE /D_UNICODE ")
        f.write("/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS ")
        f.write(f'/I"{sdkinc}\\cppwinrt" ')
        f.write("dllmain.cpp VisualTreeWatcher.cpp ")
        f.write(f'/link /DLL /OUT:"{WORK}\\StartTAP.dll" /DEF:"{WORK}\\exports.def" ')
        f.write("WindowsApp.lib ole32.lib runtimeobject.lib advapi32.lib\n")
        f.write("if errorlevel 1 ( echo TAP BUILD FAILED & exit /b 1 )\n")
        # start.exe (x64)
        f.write("cl /nologo /std:c++20 /utf-8 /EHsc /O2 /MT /W3 /wd4828 /DUNICODE /D_UNICODE start.cpp ")
        # Windows subsystem: no console flash at logon/double-click (AttachConsole for terminals)
        f.write(f'/link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /OUT:"{WORK}\\start.exe" advapi32.lib shell32.lib ole32.lib user32.lib\n')
        f.write("if errorlevel 1 ( echo START BUILD FAILED & exit /b 1 )\n")
        f.write("echo BUILD OK\n")

    r = subprocess.run(f'cmd /c "{bat}"', capture_output=True, text=True, errors="replace")
    full = (r.stdout or "") + ("\n" + r.stderr if r.stderr else "")

    os.makedirs(args.out, exist_ok=True)
    ok = True
    built = []
    for out in ("StartTAP.dll", "start.exe"):
        p = os.path.join(WORK, out)
        if not os.path.exists(p):
            ok = False
            continue
        shutil.copyfile(p, os.path.join(args.out, out))
        built.append(os.path.join(args.out, out))

    log = os.path.join(args.out, "build.log")
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write(full)

    problems = [ln for ln in full.splitlines() if "error C" in ln or "error LNK" in ln or "FAILED" in ln]
    if problems:
        print("---- 编译问题（前 20 条，完整见 build.log）----")
        for ln in problems[:20]:
            print(" ", ln)
    print("---- 构建结果 ----")
    for b in built:
        print("  OK ->", b)
    if not ok:
        print("  FAILED：产物缺失，完整日志 ->", log)
    print("  日志 ->", log)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
