#pragma once
#include "framework.h"

template <typename T>
T convert_from_abi(com_ptr<::IInspectable> from)
{
	T to{ nullptr }; // `T` is a projected type.

	winrt::check_hresult(from->QueryInterface(winrt::guid_of<T>(),
		winrt::put_abi(to)));

	return to;
}

// 递归找指定名字的后代（单个名字，兼容旧调用）
static DependencyObject FindDescendantByName(DependencyObject root, hstring name)
{
	if (root == nullptr) return nullptr;

	int count = 0;
	try { count = VisualTreeHelper::GetChildrenCount(root); }
	catch (...) { return nullptr; }

	for (int i = 0; i < count; i++)
	{
		DependencyObject child = nullptr;
		try { child = VisualTreeHelper::GetChild(root, i); }
		catch (...) { continue; }
		if (child == nullptr) continue;

		hstring childName;
		try { childName = child.GetValue(FrameworkElement::NameProperty()).as<hstring>(); }
		catch (...) {}
		if (childName == name)
			return child;

		DependencyObject result = FindDescendantByName(child, name);
		if (result != nullptr)
			return result;
	}

	return nullptr;
}

// 25H2/22H2 元素改名兼容：按别名表找第一个命中的后代
static FrameworkElement FindFirstByName(DependencyObject root, const wchar_t* const* aliases)
{
	if (root == nullptr || aliases == nullptr) return nullptr;
	for (const wchar_t* const* p = aliases; *p != nullptr; ++p)
	{
		auto found = FindDescendantByName(root, hstring(*p));
		if (found != nullptr)
		{
			auto fe = found.try_as<FrameworkElement>();
			if (fe != nullptr) return fe;
		}
	}
	return nullptr;
}

// 从任意节点往上找视觉树根（设置面板回调里重新定位目标用）
static DependencyObject FindTreeRoot(DependencyObject node)
{
	DependencyObject cur = node;
	for (int i = 0; i < 64; i++)
	{
		DependencyObject parent = nullptr;
		try { parent = VisualTreeHelper::GetParent(cur); }
		catch (...) { break; }
		if (parent == nullptr) break;
		cur = parent;
	}
	return cur;
}

// 轻量日志：%TEMP%\TranslucentSM.log（不用 CRT 流，跨线程安全）
static void TsmLog(const wchar_t* msg)
{
	wchar_t dir[MAX_PATH];
	if (GetTempPathW(MAX_PATH, dir) == 0) return;
	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%sTranslucentSM.log", dir);
	HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	wchar_t line[512];
	SYSTEMTIME st;
	GetLocalTime(&st);
	int n = swprintf_s(line, L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
	DWORD written = 0;
	if (n > 0) WriteFile(h, line, (DWORD)(n * sizeof(wchar_t)), &written, nullptr);
	CloseHandle(h);
}

static DWORD GetVal(LPCWSTR val)
{
	DWORD dw = 0, dwSize = sizeof(DWORD);
	if (RegGetValue(HKEY_CURRENT_USER, L"Software\\TranslucentSM", val, RRF_RT_DWORD, NULL, &dw, &dwSize) != ERROR_SUCCESS)
		return 0;
	return dw;
}

static DWORD GetVal(LPCWSTR val, DWORD defVal)
{
	DWORD dw = 0, dwSize = sizeof(DWORD);
	if (RegGetValue(HKEY_CURRENT_USER, L"Software\\TranslucentSM", val, RRF_RT_DWORD, NULL, &dw, &dwSize) != ERROR_SUCCESS)
		return defVal;
	return dw;
}

static HRESULT SetVal(HKEY subkey, LPCWSTR key, DWORD val)
{
	return RegSetValueEx(subkey, key, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
}
