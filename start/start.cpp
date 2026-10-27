// start.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <aclapi.h>
#include <Shlobj.h>
#include <filesystem>
#include "resource.h"

PSECURITY_DESCRIPTOR sd = nullptr, sdFile = nullptr;

bool RegistryGrantAll(HKEY hKey)
{
	bool bResult = false;

	DWORD ok = 0;
	const TCHAR* szSD =
		TEXT("D:")                          // Discretionary ACL
		TEXT("(A;OICI;KR;;;S-1-5-12)")      // NT AUTHORITY\RESTRICTED
		TEXT("(A;OICI;KA;;;S-1-5-18)")      // NT AUTHORITY\SYSTEM
		TEXT("(A;OICI;KA;;;S-1-5-32-545)")  // BUILTIN\Users
		TEXT("(A;OICI;KA;;;S-1-5-32-544)")  // BUILTIN\Administrators
		TEXT("(A;OICI;KA;;;S-1-15-2-1)");   // APPLICATION PACKAGE AUTHORITY\ALL APPLICATION PACKAGES

	if (ConvertStringSecurityDescriptorToSecurityDescriptor((LPCTSTR)szSD, SDDL_REVISION_1, &sd, 0))
	{
		auto result = RegSetKeySecurity(hKey, DACL_SECURITY_INFORMATION, sd);
		if (ERROR_SUCCESS == result)
			bResult = true;
		else
			SetLastError(result);
		// Free the memory allocated for the SECURITY_DESCRIPTOR.
		LocalFree(sd);
	}
	return bResult;
}

int FileGrantAll(LPCWSTR file)
{
	const TCHAR* szSD =
		TEXT("D:AI")                          // Discretionary ACL
		TEXT("(A;OICI;0x1200a9;;;AC)")
		TEXT("(A;OICIID;FA;;;BA)")
		TEXT("(A;OICIID;FA;;;SY)")
		TEXT("(A;OICIID;0x1200a9;;;BU)")
		TEXT("(A;ID;0x1301bf;;;AU)")
		TEXT("(A;OICIIOID;SDGXGWGR;;;AU)");
	if (ConvertStringSecurityDescriptorToSecurityDescriptor((LPCTSTR)szSD, SDDL_REVISION_1, &sdFile, 0))
	{
		SetFileSecurity(file, DACL_SECURITY_INFORMATION, sdFile);
	}
	return 1;
}

int CreateDwords(HKEY subKey, LPCWSTR value, DWORD defVal)
{
	DWORD dwSize = sizeof(DWORD);
	if (ERROR_FILE_NOT_FOUND == RegGetValue(HKEY_CURRENT_USER, L"Software\\TranslucentSM", value, RRF_RT_DWORD, NULL, NULL, &dwSize))
	{
		return RegSetValueEx(subKey, value, 0, REG_DWORD, (const BYTE*)&defVal, sizeof(defVal));
	}
	return -1;
}

// ---- 开机自启：把自身（改名 TranslucentSM.exe）+ StartTAP.dll 部署到 Startup 目录 ----
// 不用 VBS：Windows 登录时直接运行 Startup 里的 exe。用户删掉原目录的 exe/dll 不影响已部署的
// 自启副本；再次运行会刷新副本（升级）。旧的 TranslucentSM.vbs 会被顺手清掉。
// 卸载自启：手动删除 Startup 里的 TranslucentSM.exe + StartTAP.dll 即可（见 README）。
static std::wstring GetStartupDir()
{
	wchar_t appdata[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata)))
		return L"";
	return std::wstring(appdata) + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
}

static std::wstring GetStartupExePath() { return GetStartupDir() + L"\\TranslucentSM.exe"; }
static std::wstring GetStartupDllPath() { return GetStartupDir() + L"\\StartTAP.dll"; }

static bool IsRunningFromStartup()
{
	wchar_t self[MAX_PATH] = {};
	GetModuleFileNameW(NULL, self, MAX_PATH);
	return _wcsicmp(self, GetStartupExePath().c_str()) == 0;
}

// 复制单个文件；源=目标时跳过，目标被占用（正在运行的副本）时删后重试
static bool CopyOne(const std::wstring& src, const std::wstring& dst)
{
	if (_wcsicmp(src.c_str(), dst.c_str()) == 0) return true;
	if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
	DeleteFileW(dst.c_str());
	return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

static bool InstallAutostart()
{
	std::wstring dir = GetStartupDir();
	if (dir.empty()) return false;
	CreateDirectoryW(dir.c_str(), nullptr);

	wchar_t self[MAX_PATH] = {};
	GetModuleFileNameW(NULL, self, MAX_PATH);
	std::wstring selfPath(self);
	std::wstring srcDll = selfPath.substr(0, selfPath.find_last_of(L'\\') + 1) + L"StartTAP.dll";

	bool ok = CopyOne(selfPath, GetStartupExePath());
	if (GetFileAttributesW(srcDll.c_str()) != INVALID_FILE_ATTRIBUTES)
		ok = CopyOne(srcDll, GetStartupDllPath()) && ok;

	// 清理旧的 VBS 方案（迁移）
	DeleteFileW((dir + L"\\TranslucentSM.vbs").c_str());
	return ok;
}

// 双击运行时给个提示；终端里运行只打控制台；开机（从 Startup 启动）完全静默
static bool g_attachedConsole = false;
static void Notify(const wchar_t* msg)
{
	if (g_attachedConsole || IsRunningFromStartup()) return;
	MessageBoxW(NULL, msg, L"TranslucentSM", MB_OK | MB_ICONINFORMATION);
}

int main(int argc, char* argv[])
{
	// 终端里运行能看到输出；双击/开机运行则静默（开机副本不弹窗）
	// 高分屏：声明 Per-Monitor V2 DPI 感知，MessageBox 按实际 DPI 渲染
	// （否则系统会把对话框位图拉伸，高分屏上发虚）
	{
		typedef BOOL(WINAPI* SetCtxFn)(HANDLE);
		auto setCtx = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
		if (setCtx) setCtx((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
		else SetProcessDPIAware();
	}

	g_attachedConsole = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
	std::cout << "Initializing...\n================\n";

	// ---- 开机自启：把自身 + StartTAP.dll 部署到 Startup 目录（无 VBS）----
	// /noautostart 本次不部署（卸载自启=手动删除 Startup 里的两个文件，见 README）
	bool noAutostart = false;
	for (int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		for (auto& x : a) x = tolower(x);
		if (a == "/noautostart") noAutostart = true;
	}
	if (!noAutostart)
	{
		if (InstallAutostart())
			std::cout << "- Autostart installed: Startup\\TranslucentSM.exe + StartTAP.dll\n";
		else
			std::cout << "- Autostart install failed.\n";
	}

	PROCESSENTRY32 entry;
	entry.dwSize = sizeof(PROCESSENTRY32);
	std::wstring ok = L"StartMenuExperienceHost.exe";

	if (argc > 3)
	{
		std::string arg = argv[3];
		for (auto& x : arg) {
			x = tolower(x);
		}
		if (arg == "/process")
		{
			std::wstring ws(argv[4], argv[4] + strlen(argv[4]));
			ok = ws;
		}
	}
	DWORD pid = 0;
	// 24H2+：杀掉 StartMenuExperienceHost.exe 后它要过一会儿才被拉起，
	// 这里轮询等待（最多 30 秒），避免 README 流程里"进程还没回来就退出"的空转。
	for (int attempt = 0; attempt < 60 && pid == 0; attempt++)
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

		if (Process32First(snapshot, &entry) == TRUE)
		{
			do
			{
				if (wcscmp(entry.szExeFile, ok.c_str()) == 0)
				{
					pid = entry.th32ProcessID;
				}
			} while (pid == 0 && Process32Next(snapshot, &entry) == TRUE);
		}
		CloseHandle(snapshot);

		if (pid == 0)
		{
			if (attempt == 0)
				std::wcout << "\n- " << ok.c_str() << " is not running, waiting up to 30s for it to come back...";
			Sleep(500);
		}
	}
	if (pid == 0)
	{
		std::wcout << "\n- " << ok.c_str() << " is not running.";
		std::cout << "\n\n";
		return 0;
	}
	std::wcout << "\n- " << ok.c_str() << " PID: " << pid;

	typedef HRESULT(*InitializeXamlDiagnosticsExProto)(_In_ LPCWSTR endPointName, _In_ DWORD pid, _In_opt_ LPCWSTR wszDllXamlDiagnostics, _In_ LPCWSTR wszTAPDllName, _In_opt_ CLSID tapClsid, _In_ LPCWSTR wszInitializationData);
	InitializeXamlDiagnosticsExProto InitializeXamlDiagnosticsExFn = (InitializeXamlDiagnosticsExProto)GetProcAddress(LoadLibraryEx(L"Windows.UI.Xaml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "InitializeXamlDiagnosticsEx");
	if (InitializeXamlDiagnosticsExFn == nullptr)
	{
		std::cout << "\n- Windows.UI.Xaml.dll does not export InitializeXamlDiagnosticsEx on this system.\n\n";
		return 1;
	}

	HKEY subKey = nullptr;
	DWORD disposition;
	DWORD dwSize = sizeof(DWORD), dwInstalled = 0;
	RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\TranslucentSM", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &subKey, &disposition);
	if (disposition == REG_CREATED_NEW_KEY)
	{
		std::cout << "\n- Created HKCU\\SOFTWARE\\TranslucentSM registry key.";
		RegistryGrantAll(subKey);
	}
	else
	{
		std::cout << "\n- Opened HKCU\\SOFTWARE\\TranslucentSM registry key.";
	}
	
	// do for each key
	CreateDwords(subKey, L"HideSearch", 0);
	CreateDwords(subKey, L"HideBorder", 0);
	CreateDwords(subKey, L"HideRecommended", 0);
	CreateDwords(subKey, L"TintOpacity", 30);
	CreateDwords(subKey, L"TintLuminosityOpacity", 30);

	RegCloseKey(subKey);

	// Get the path to the current executable
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(NULL, path, MAX_PATH);
	std::wstring pathStr = path;

	// Get the path to the current directory
	std::wstring dir = pathStr.substr(0, pathStr.find_last_of(L"\\"));

	// Get the path to the DLL
	std::wstring dllPath = dir + L"\\";
	std::wstring fn;

	if (argc > 1)
	{
		std::string arg = argv[1];
		for (auto& x : arg) {
			x = tolower(x);
		}
		if (arg == "/dllname")
		{
			std::wstring ws(argv[2], argv[2] + strlen(argv[2]));
			fn = ws;
			dllPath += ws;
		}
	}
	else
	{
		dllPath += L"StartTAP.dll";
		fn = L"StartTAP.dll";
	}

	// Convert dllPath to WCHAR
	const wchar_t* dllPathW = dllPath.c_str();

	if (!std::filesystem::exists(dllPathW))
	{
		std::wcout << "\n- " << dllPathW << " not found." << "\n\n";
		return 0;
	}
	auto nResult = FileGrantAll(dllPathW);
	if (nResult != 0)
	{
		std::cout << "\n- Changed ";
		std::wcout << fn.c_str();
		std::cout << " permissions.";
	}

	static constexpr GUID temp = { 0x36162bd3, 0x3531, 0x4131, { 0x9b, 0x8b, 0x7f, 0xb1, 0xa9, 0x91, 0xef, 0x51 } };
	HRESULT hr = InitializeXamlDiagnosticsExFn(L"VisualDiagConnection1", pid, NULL, dllPathW, temp, L"");
	std::wcout << "\n- InitializeXamlDiagnosticsEx hr=0x" << std::hex << (unsigned long)(hr & 0xFFFFFFFF) << std::dec;
	if (SUCCEEDED(hr))
	{
		std::wcout << "\n- Injected " << dllPathW << " into " << ok.c_str();
		std::cout << "\n- Open the Start menu to apply (settings: HKCU\\SOFTWARE\\TranslucentSM).";
		Notify(L"TranslucentSM 已注入开始菜单，并已设置开机自启。\n\n设置面板：开始菜单底栏的铅笔按钮。");
	}
	else
	{
		std::wcout << "\n- Injection FAILED for " << dllPathW;
		Notify(L"注入失败，请查看 %TEMP%\\TranslucentSM.log。");
	}
	std::cout << "\n\n";
	return SUCCEEDED(hr) ? 0 : 1;
}