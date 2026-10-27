#include "VisualTreeWatcher.h"
#include "Helpers.h"
#include <vector>

// ============================================================================
// TranslucentSM — 25H2 (26200) 适配版
//
// 与 0.6.9 的差异：
//  1) 元素名/类型兼容 25H2 与 22H2/23H2 两套命名（见下面别名表）。
//  2) 全部查找改成 try_as + 空指针保护：0.6.9 里 FindDescendantByName(...).as<T>()
//     在元素改名后会对空对象调 as，直接把 StartMenuExperienceHost 打崩/中断回调。
//  3) 增加 PollUiLayer 兜底：24H2+ 开始菜单在进程启动时就预建 XAML 树，
//     Add 回调经常在 TAP 挂上之前就发完了；这里用 GetUiLayer + dispatcher
//     周期性把设置套到已存在的树上，注入时机不再敏感。
//  4) 25H2 的 RootGrid 不再是菜单根（全应用列表的图块模板有 65 个同名元素），
//     设置面板改挂在 RootPanel（底栏）。
// ============================================================================

namespace
{
	// ---- 25H2 / 22H2 元素名映射 ----
	const wchar_t* const kAcrylicBorder[] = { L"AcrylicBorder", nullptr };
	const wchar_t* const kAcrylicOverlay[] = { L"AcrylicOverlay", nullptr };
	const wchar_t* const kSearchBox[] = { L"SearchBoxToggleButton", L"StartMenuSearchBox", nullptr };
	const wchar_t* const kRecRoot[] = { L"TopLevelSuggestionsRoot", L"TopLevelSuggestionsContainerParent", L"SuggestionsParentContainer", nullptr };
	const wchar_t* const kRecHeader[] = { L"TopLevelSuggestionsListHeader", nullptr };
	const wchar_t* const kRecMore[] = { L"ShowMoreSuggestions", L"ShowMoreSuggestionsButton", nullptr };
	const wchar_t* const kPinnedList[] = { L"StartMenuPinnedList", nullptr };
	const wchar_t* const kNavPaneList[] = { L"NavigationPanePlacesListView", nullptr };
	const wchar_t* const kPowerButton[] = { L"PowerButton", nullptr };
	const wchar_t* const kGearButton[] = { L"TranslucentSMSettings", nullptr };
	const wchar_t* const kRootPanel[] = { L"RootPanel", nullptr };
	const wchar_t* const kTopLevelRoot[] = { L"TopLevelRoot", nullptr };

	// ---- 界面文本：中文 / English 两套 ----
	// 语言选择：注册表 HKCU\SOFTWARE\TranslucentSM\Language="zh" 或 "en" 可强制；
	// 未设置时按系统 UI 语言自动判断（主语言 0x04 = 中文）。
	struct L10n
	{
		const wchar_t* tintOpacityLabel;
		const wchar_t* tintOpacityTip;
		const wchar_t* tintLumLabel;
		const wchar_t* tintLumTip;
		const wchar_t* hideSearch;
		const wchar_t* hideBorder;
		const wchar_t* hideRecommended;
		const wchar_t* gearTooltip;
	};
	const L10n kEn = {
		L"Tint",
		L"Tint colour opacity: 0% shows the backdrop fully, 100% is solid tint colour (fully opaque).",
		L"Opacity",
		L"Luminosity layer opacity: controls the greyish wash over the blurred backdrop.",
		L"Hide search box",
		L"Hide white border",
		L"Hide recommended",
		L"TranslucentSM settings"
	};
	const L10n kZh = {
		L"色调",
		L"色调层不透明度：0%=完全透出背景，100%=被色调（浅灰）完全覆盖=完全不透明。",
		L"不透明度",
		L"亮度层不透明度：控制模糊背景之上的灰白罩强度。",
		L"隐藏搜索栏",
		L"隐藏白色边框",
		L"隐藏推荐区",
		L"TranslucentSM 设置"
	};
	const L10n& Tr()
	{
		static const L10n* sel = []() -> const L10n* {
			wchar_t buf[16] = {};
			DWORD n = sizeof(buf);
			if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TranslucentSM", L"Language",
				RRF_RT_REG_SZ, NULL, buf, &n) == ERROR_SUCCESS)
			{
				if (_wcsnicmp(buf, L"zh", 2) == 0) return &kZh;
				if (_wcsnicmp(buf, L"en", 2) == 0) return &kEn;
			}
			LANGID id = GetUserDefaultUILanguage();
			return (id & 0x3FF) == 0x04 ? &kZh : &kEn;   // 中文（简/繁）都用中文界面
		}();
		return *sel;
	}

	struct ApplyState
	{
		bool panelInstalled = false;
		bool pinnedGrown = false;
		double pinnedSavedHeight = 0;
		bool searchHidden = false;
		double searchSavedHeight = 0;
		Thickness searchSavedMargin{ 0,0,0,0 };
	};
	ApplyState g_state;          // 只在 XAML UI 线程上访问

	HKEY g_subKey = nullptr;

	// ---- 问题 4 修复：缓存目标元素 / 树根 ----
	// Flyout 内容活在独立的 Popup 视觉树里，从 sender 往上走只能到 PopupRoot，
	// 找不到开始菜单的元素。所以 Add 回调 / 轮询命中元素时就缓存下来，处理器直接用缓存。
	// 只在 XAML UI 线程上访问。
	DependencyObject g_root{ nullptr };      // 开始菜单真正的树根
	Border g_acrylic{ nullptr };
	Border g_overlay{ nullptr };
	FrameworkElement g_search{ nullptr };

	Border GetAcrylic()
	{
		if (g_acrylic) return g_acrylic;
		if (g_root) g_acrylic = FindFirstByName(g_root, kAcrylicBorder).try_as<Border>();
		return g_acrylic;
	}
	Border GetOverlay()
	{
		if (g_overlay) return g_overlay;
		if (g_root) g_overlay = FindFirstByName(g_root, kAcrylicOverlay).try_as<Border>();
		return g_overlay;
	}
	FrameworkElement GetSearch()
	{
		if (g_search) return g_search;
		if (g_root) g_search = FindFirstByName(g_root, kSearchBox);
		return g_search;
	}

	// 写注册表（不依赖 g_subKey 句柄）并记日志
	void SaveVal(LPCWSTR key, DWORD v)
	{
		DWORD value = v;
		LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, L"SOFTWARE\\TranslucentSM", key,
			REG_DWORD, &value, sizeof(value));
		wchar_t m[200];
		swprintf_s(m, L"handler: %s=%lu (reg=%ld) acrylic=%d overlay=%d search=%d root=%d",
			key, (unsigned long)v, (long)r,
			g_acrylic ? 1 : 0, g_overlay ? 1 : 0, g_search ? 1 : 0, g_root ? 1 : 0);
		TsmLog(m);
	}

	// 注册表里存 0..100
	double GetPercent(LPCWSTR key, DWORD defVal)
	{
		DWORD v = GetVal(key, defVal);
		if (v > 100) v = 100;
		return double(v) / 100.0;
	}

	void ApplyAcrylic(Border const& border)
	{
		if (!border) return;
		auto brush = border.Background().try_as<AcrylicBrush>();
		if (!brush) return;
		brush.TintOpacity(GetPercent(L"TintOpacity", 30));
		brush.TintLuminosityOpacity(GetPercent(L"TintLuminosityOpacity", 30));
	}

	void ApplyHideBorder(Border const& border)
	{
		if (!border) return;
		if (GetVal(L"HideBorder", 0) != 1) return;
		auto solid = border.Background().try_as<SolidColorBrush>();
		if (solid) solid.Opacity(0);
	}

	void ApplyHideSearch(FrameworkElement const& el)
	{
		if (!el) return;
		if (GetVal(L"HideSearch", 0) != 1) return;
		if (!g_state.searchHidden)
		{
			g_state.searchSavedHeight = el.Height();
			g_state.searchSavedMargin = el.Margin();
			g_state.searchHidden = true;
		}
		el.Height(0);
		el.Margin({ 0 });
	}

	void RestoreSearch(FrameworkElement const& el)
	{
		if (!el) return;
		if (!g_state.searchHidden) return;
		el.Height(g_state.searchSavedHeight);
		el.Margin(g_state.searchSavedMargin);
		g_state.searchHidden = false;
	}

	void ApplyHideRecommended(DependencyObject const& treeRoot)
	{
		if (!treeRoot) return;
		if (GetVal(L"HideRecommended", 0) != 1) return;

		auto recRoot = FindFirstByName(treeRoot, kRecRoot);
		auto recHeader = FindFirstByName(treeRoot, kRecHeader);
		auto recMore = FindFirstByName(treeRoot, kRecMore);

		double recHeight = 0;
		if (recRoot && recRoot.Visibility() != Visibility::Collapsed)
		{
			recHeight = recRoot.ActualHeight();
			recRoot.Visibility(Visibility::Collapsed);
			TsmLog(L"HideRecommended: collapsed TopLevelSuggestionsRoot");
		}
		if (recHeader) recHeader.Visibility(Visibility::Collapsed);
		if (recMore) recMore.Visibility(Visibility::Collapsed);

		// 固定区补高（只做一次；失败也不影响折叠效果）
		if (!g_state.pinnedGrown && recHeight > 1)
		{
			auto pinned = FindFirstByName(treeRoot, kPinnedList);
			if (pinned)
			{
				double baseH = pinned.Height();
				if (!(baseH > 0)) baseH = pinned.ActualHeight();
				if (baseH > 0)
				{
					g_state.pinnedSavedHeight = baseH;
					g_state.pinnedGrown = true;
					pinned.Height(baseH + recHeight);
					TsmLog(L"HideRecommended: grew StartMenuPinnedList");
				}
			}
		}
	}

	void RestoreRecommended(DependencyObject const& treeRoot)
	{
		if (!treeRoot) return;
		if (auto recRoot = FindFirstByName(treeRoot, kRecRoot))
			recRoot.Visibility(Visibility::Visible);
		if (auto recHeader = FindFirstByName(treeRoot, kRecHeader))
			recHeader.Visibility(Visibility::Visible);
		if (auto recMore = FindFirstByName(treeRoot, kRecMore))
			recMore.Visibility(Visibility::Visible);
		if (g_state.pinnedGrown)
		{
			if (auto pinned = FindFirstByName(treeRoot, kPinnedList))
				pinned.Height(g_state.pinnedSavedHeight);
			g_state.pinnedGrown = false;
		}
	}

	// 设置面板（25H2 挂在底栏 RootPanel；22H2 走旧路径）
	void InstallSettingsPanel(DependencyObject const& treeRoot);

	// 铅笔定位：做成固定夹图标队列里等宽的一格，紧挨最左侧图标，
	// 间距取图标之间的标准间距。全部按 XAML 的 DIP 计算，分辨率 / 缩放比例无关；
	// 图标增减后可重复调用（轮询里会重新摆位）。
	// 铅笔摆位：纯布局，不做任何测量 / 坐标变换（上一版 0xC0000005 的嫌疑路径已全部移除）。
	// RootPanel 列定义实测为 [Star, 12px, Auto, Auto]，用户头像 col0、图标队列 col2、电源 col3。
	// 铅笔跨 col0+col1、靠右、右边距 0 → 右边缘恒等于图标队列左边缘，
	// 队列增减图标（设置 / 下载 / 文件资源管理器…）时由 Grid 自动跟随。
	void PositionGearBesidePlaces(DependencyObject const& sceneRoot)
	{
		if (!sceneRoot) return;
		if (GetVal(L"GearAutoPlace", 1) != 1) return;   // 默认开；设为 0 退回旧的固定位
		auto bt = FindFirstByName(sceneRoot, kGearButton).try_as<Button>();
		if (!bt) return;

		try
		{
			auto grid = VisualTreeHelper::GetParent(bt).try_as<Grid>();
			if (!grid || grid.Name() != L"RootPanel") return;
			if (grid.ColumnDefinitions().Size() < 3) return;   // 布局形状不符就不动

			Grid::SetRow(bt, 0);
			Grid::SetColumn(bt, 0);
			Grid::SetColumnSpan(bt, 2);
			bt.HorizontalAlignment(HorizontalAlignment::Right);
			bt.VerticalAlignment(VerticalAlignment::Center);
			bt.Width(40);                      // 与固定夹图标格等宽，微调看实际效果
			bt.Margin(Thickness{ 0, 0, 0, 0 });

			static bool logged = false;
			if (!logged)
			{
				logged = true;
				TsmLog(L"gear placed: layout-only (col0 span2, right-aligned)");
			}
		}
		catch (...)
		{
			TsmLog(L"PositionGearBesidePlaces failed");
		}
	}

	// 一次性布局诊断：只读属性，不做测量 / 变换
	void DumpRootPanelLayoutOnce(DependencyObject const& sceneRoot)
	{
		static bool done = false;
		if (done || !sceneRoot) return;
		auto grid = FindFirstByName(sceneRoot, kRootPanel).try_as<Grid>();
		if (!grid) return;
		done = true;
		try
		{
			wchar_t m[320];
			auto cols = grid.ColumnDefinitions();
			auto rows = grid.RowDefinitions();
			auto kids = grid.Children();
			swprintf_s(m, L"layout: RootPanel cols=%u rows=%u children=%u hAlign=%d actualW=%.1f",
				cols.Size(), rows.Size(), kids.Size(),
				(int)grid.HorizontalAlignment(), grid.ActualWidth());
			TsmLog(m);

			for (uint32_t i = 0; i < cols.Size(); i++)
			{
				auto w = cols.GetAt(i).Width();
				swprintf_s(m, L"layout: col[%u] width=%.1f unit=%d (0=Auto 1=Pixel 2=Star)",
					i, w.Value, (int)w.GridUnitType);
				TsmLog(m);
			}

			for (uint32_t i = 0; i < kids.Size(); i++)
			{
				auto fe = kids.GetAt(i).try_as<FrameworkElement>();
				if (!fe) continue;
				std::wstring cls;
				try { cls = winrt::get_class_name(fe).c_str(); } catch (...) {}
				auto mg = fe.Margin();
				swprintf_s(m, L"layout: child[%u] %s name=%s col=%d span=%d row=%d hAlign=%d width=%.1f actualW=%.1f margin=%.1f,%.1f,%.1f,%.1f",
					i, cls.c_str(), fe.Name().c_str(),
					Grid::GetColumn(fe), Grid::GetColumnSpan(fe), Grid::GetRow(fe),
					(int)fe.HorizontalAlignment(), fe.Width(), fe.ActualWidth(),
					mg.Left, mg.Top, mg.Right, mg.Bottom);
				TsmLog(m);

				if (auto lv = fe.try_as<ListViewBase>())
				{
					swprintf_s(m, L"layout: places list ItemsSource=%d", lv.ItemsSource() ? 1 : 0);
					TsmLog(m);
				}
			}
		}
		catch (...) { TsmLog(L"layout dump failed"); }
	}

	// 把设置套到单个元素上（Add 回调与轮询共用）
	void ApplyToElement(DependencyObject const& el, std::wstring_view name, std::wstring_view type)
	{
		if (!el) return;

		if (name == L"AcrylicBorder")
		{
			if (auto border = el.try_as<Border>())
			{
				g_acrylic = border;
				if (auto r = FindTreeRoot(el)) g_root = r;
				ApplyAcrylic(border);
				static bool logged = false;
				if (!logged) { logged = true; TsmLog(L"applied acrylic to AcrylicBorder"); }
			}
		}
		else if (name == L"AcrylicOverlay")
		{
			if (auto border = el.try_as<Border>())
			{
				g_overlay = border;
				ApplyHideBorder(border);
			}
		}
		else if (name == L"SearchBoxToggleButton" || name == L"StartMenuSearchBox" ||
			type == L"StartMenu.SearchBoxToggleButton" || type == L"StartDocked.SearchBoxToggleButton")
		{
			if (auto fe = el.try_as<FrameworkElement>())
			{
				g_search = fe;
				ApplyHideSearch(fe);
			}
		}
		else if (name == L"TopLevelSuggestionsRoot" || name == L"TopLevelSuggestionsContainerParent" ||
			name == L"SuggestionsParentContainer" || name == L"ShowMoreSuggestions" ||
			name == L"TopLevelSuggestionsListHeader")
		{
			// 折叠交给 ApplyHideRecommended 统一处理（含固定区补高）
			if (auto root = FindTreeRoot(el))
			{
				g_root = root;
				ApplyHideRecommended(root);
			}
		}
	}

	// 轮询走树：每个节点都过一遍 ApplyToElement
	void WalkAndApply(DependencyObject node, int depth, int& budget)
	{
		if (!node || budget <= 0 || depth > 24) return;
		budget--;

		try
		{
			std::wstring name;
			std::wstring type;
			try { type = winrt::get_class_name(node).c_str(); } catch (...) {}
			if (auto fe = node.try_as<FrameworkElement>())
			{
				try { name = fe.Name().c_str(); } catch (...) {}
			}
			if (!name.empty() || type.find(L"Start") != std::wstring::npos)
				ApplyToElement(node, name, type);

			if (!g_state.panelInstalled)
				InstallSettingsPanel(node);
		}
		catch (...) {}

		int count = 0;
		try { count = VisualTreeHelper::GetChildrenCount(node); }
		catch (...) { return; }
		for (int i = 0; i < count; i++)
		{
			DependencyObject child = nullptr;
			try { child = VisualTreeHelper::GetChild(node, i); }
			catch (...) { continue; }
			WalkAndApply(child, depth + 1, budget);
		}
	}
}

DWORD dwSize = sizeof(DWORD), dwOpacity = 0, dwLuminosity = 0, dwHide = 0, dwBorder = 0, dwRec = 0;

VisualTreeWatcher::VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
	: m_selfPtr(this, winrt::take_ownership_from_abi_t{}),
	m_XamlDiagnostics(site.as<IXamlDiagnostics>())
{
	this->AddRef();

	HANDLE thread = CreateThread(
		nullptr, 0,
		[](LPVOID lpParam) -> DWORD {
			auto watcher = reinterpret_cast<VisualTreeWatcher*>(lpParam);
			watcher->AdviseVisualTreeChange();
			return 0;
		},
		this, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	}

	// 24H2+ 兜底轮询（详见文件头注释）
	HANDLE poll = CreateThread(
		nullptr, 0,
		[](LPVOID lpParam) -> DWORD {
			reinterpret_cast<VisualTreeWatcher*>(lpParam)->PollUiLayer();
			return 0;
		},
		this, 0, nullptr);
	if (poll) {
		CloseHandle(poll);
	}
}

void VisualTreeWatcher::AdviseVisualTreeChange() {
	try {
		const auto treeService = m_XamlDiagnostics.as<IVisualTreeService3>();
		winrt::check_hresult(treeService->AdviseVisualTreeChange(this));
		TsmLog(L"AdviseVisualTreeChange OK");
	}
	catch (...) {
		TsmLog(L"AdviseVisualTreeChange FAILED");
	}
}

void VisualTreeWatcher::PollUiLayer()
{
	TsmLog(L"poll: thread started");
	// 前 60 秒每 1.5 秒一次，之后每 15 秒一次（改动设置后无需重启也能重新套用）
	for (int i = 0; i < 400; i++)
	{
		Sleep(i < 40 ? 1500 : 15000);
		try
		{
			com_ptr<IXamlDiagnostics> diag = m_XamlDiagnostics;

			::IInspectable* rawDisp = nullptr;
			HRESULT hrD = diag->GetDispatcher(&rawDisp);
			if (i < 3)
			{
				wchar_t m[120];
				swprintf_s(m, L"poll#%d GetDispatcher hr=0x%08X ptr=%p", i, hrD & 0xFFFFFFFF, rawDisp);
				TsmLog(m);
			}
			if (FAILED(hrD) || !rawDisp) continue;
			IInspectable dispObj{ nullptr };
			winrt::attach_abi(dispObj, rawDisp);
			auto dispatcher = dispObj.try_as<Windows::UI::Core::CoreDispatcher>();
			if (!dispatcher) continue;

			dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
				[diag, i]() {
					try
					{
						static int traced = 0;
						bool trace = (traced < 5);
						wchar_t m[220];
						::IInspectable* rawLayer = nullptr;
						HRESULT hrL = diag->GetUiLayer(&rawLayer);
						if (trace) { swprintf_s(m, L"poll#%d GetUiLayer hr=0x%08X ptr=%p", i, hrL & 0xFFFFFFFF, rawLayer); TsmLog(m); }
						if (FAILED(hrL) || !rawLayer) { if (trace) traced++; return; }
						IInspectable layer{ nullptr };
						winrt::attach_abi(layer, rawLayer);
						auto dep = layer.try_as<DependencyObject>();
						// GetUiLayer 返回 Window；VisualTreeHelper 对 Window 取子节点为 0，
						// 真正的树根是 Window.Content()
						int isWin = 0;
						if (auto win = layer.try_as<Windows::UI::Xaml::Window>())
						{
							isWin = 1;
							if (auto content = win.Content())
								dep = content;
						}
						if (!dep) { if (trace) { TsmLog(L"poll: no dep"); traced++; } return; }
						// GetUiLayer 返回的是一棵空 Grid（kids=0），真正的菜单树要用
						// Add 事件里缓存下来的 g_root（g_root 由 ApplyToElement/InstallSettingsPanel 填）
						if (g_root) dep = g_root;
						int kids = 0;
						try { kids = VisualTreeHelper::GetChildrenCount(dep); } catch (...) {}
						if (trace)
						{
							std::wstring cls;
							try { cls = winrt::get_class_name(dep).c_str(); } catch (...) {}
							swprintf_s(m, L"poll#%d root=%s isWin=%d kids=%d cached=%d", i, cls.c_str(), isWin, kids, g_root ? 1 : 0);
							TsmLog(m);
							traced++;
						}
						int budget = 4000;
						WalkAndApply(dep, 0, budget);
						// 固定夹图标增减后重新摆位（幂等）
						PositionGearBesidePlaces(dep);
						DumpRootPanelLayoutOnce(dep);
					}
					catch (...) { TsmLog(L"poll: lambda exception"); }
				});
		}
		catch (...) {}
	}
}

HRESULT VisualTreeWatcher::OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept
{
	return S_OK;
}

HRESULT VisualTreeWatcher::OnVisualTreeChange(ParentChildRelation relation, VisualElement element, VisualMutationType mutationType)
{
	if (mutationType == Add)
	{
		// 任何一路出问题都不能让异常穿出回调——0.6.9 就是死在这里
		try
		{
			const std::wstring_view type{ element.Type, SysStringLen(element.Type) };
			const std::wstring_view name{ element.Name, SysStringLen(element.Name) };

			IInspectable obj;
			if (FAILED(m_XamlDiagnostics->GetIInspectableFromHandle(element.Handle,
				reinterpret_cast<::IInspectable**>(winrt::put_abi(obj)))))
				return S_OK;

			auto el = obj.try_as<DependencyObject>();
			if (!el) return S_OK;

			ApplyToElement(el, name, type);

			// 设置面板：25H2 挂 RootPanel（22H2 时 RootPanel 存在于 RootGrid 子树里）
			if (!g_state.panelInstalled && (name == L"RootPanel" || name == L"RootGrid"))
				InstallSettingsPanel(el);
		}
		catch (...)
		{
			TsmLog(L"OnVisualTreeChange: swallowed exception");
		}
	}
	return S_OK;
}

namespace
{
	void InstallSettingsPanel(DependencyObject const& treeRoot)
	{
		if (g_state.panelInstalled || !treeRoot) return;

		// 从真正的树根开始找（treeRoot 可能只是某个局部节点，比如 RootPanel 自身）
		auto sceneRoot = FindTreeRoot(treeRoot);
		if (sceneRoot == nullptr) sceneRoot = treeRoot;

		// 挂载点：
		//  1) RootPanel（底栏 Grid）——首选，PositionGearBesidePlaces 的坐标系就是它，
		//     铅笔按固定夹图标队列的实际间距摆位；
		//  2) NavigationPanePlacesListView→Root 旧路径仅作回退（沿用原版偏移）。
		Panel host = nullptr;
		Panel rootPanelHost = nullptr;
		bool useLegacyPlacement = false;

		auto rootPanel = FindFirstByName(sceneRoot, kRootPanel);
		if (rootPanel != nullptr)
			rootPanelHost = rootPanel.try_as<Panel>();
		if (rootPanelHost == nullptr)
			rootPanelHost = treeRoot.try_as<Panel>();   // Add 事件路径：自身可能就是 RootPanel

		host = rootPanelHost;
		if (host == nullptr)
		{
			auto nvpane = FindFirstByName(sceneRoot, kNavPaneList);
			if (nvpane != nullptr)
			{
				auto legacyRoot = FindDescendantByName(nvpane, L"Root");
				if (legacyRoot != nullptr && VisualTreeHelper::GetChildrenCount(legacyRoot) > 0)
				{
					auto grid = VisualTreeHelper::GetChild(legacyRoot, 0).try_as<Grid>();
					if (grid != nullptr) { host = grid; useLegacyPlacement = true; }
				}
			}
		}
		if (host == nullptr) return;

		g_state.panelInstalled = true;
		g_root = sceneRoot;   // 处理器的回退树根（Flyout 里不能从 sender 往上找）

		// 安装时顺手预缓存（找不到也没关系，ApplyToElement / 轮询命中时会补上）
		if (auto b = FindFirstByName(sceneRoot, kAcrylicBorder).try_as<Border>()) g_acrylic = b;
		if (auto o = FindFirstByName(sceneRoot, kAcrylicOverlay).try_as<Border>()) g_overlay = o;
		if (auto se = FindFirstByName(sceneRoot, kSearchBox)) g_search = se;

		RegCreateKeyEx(HKEY_CURRENT_USER, L"SOFTWARE\\TranslucentSM", 0, NULL, REG_OPTION_NON_VOLATILE,
			KEY_ALL_ACCESS, NULL, &g_subKey, NULL);

		auto stackPanel = StackPanel();

		auto makeLabel = [](const wchar_t* text) {
			TextBlock tbx;
			tbx.FontFamily(Windows::UI::Xaml::Media::FontFamily(L"Segoe UI Variable"));
			tbx.FontSize(13.0);
			tbx.Text(hstring(text));
			return tbx;
		};

		// TintOpacity
		stackPanel.Children().Append(makeLabel(Tr().tintOpacityLabel));
		Slider slider;
		slider.Width(140);
		ToolTipService::SetToolTip(slider, box_value(Tr().tintOpacityTip));
		slider.Value(GetVal(L"TintOpacity", 30));
		stackPanel.Children().Append(slider);

		// TintLuminosityOpacity
		stackPanel.Children().Append(makeLabel(Tr().tintLumLabel));
		Slider slider2;
		slider2.Width(140);
		ToolTipService::SetToolTip(slider2, box_value(Tr().tintLumTip));
		slider2.Value(GetVal(L"TintLuminosityOpacity", 30));
		stackPanel.Children().Append(slider2);

		// 处理器全部走缓存元素（GetAcrylic / GetOverlay / GetSearch / g_root），不再用 sender 找树根
		slider.ValueChanged([](Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
			try {
				DWORD v = (DWORD)sender.as<Slider>().Value();
				SaveVal(L"TintOpacity", v);
				ApplyAcrylic(GetAcrylic());
			}
			catch (...) { TsmLog(L"handler TintOpacity: exception"); }
			});

		slider2.ValueChanged([](Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
			try {
				DWORD v = (DWORD)sender.as<Slider>().Value();
				SaveVal(L"TintLuminosityOpacity", v);
				ApplyAcrylic(GetAcrylic());
			}
			catch (...) { TsmLog(L"handler TintLuminosityOpacity: exception"); }
			});

		// 三个开关始终创建（以前按安装时是否找到元素决定，Add 回调早期常找不到 → 开关缺失）
		// 隐藏搜索框
		{
			auto checkBox = CheckBox();
			checkBox.Content(box_value(Tr().hideSearch));
			checkBox.IsChecked(GetVal(L"HideSearch", 0) == 1);
			stackPanel.Children().Append(checkBox);

			checkBox.Checked([](Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
				try { SaveVal(L"HideSearch", 1); ApplyHideSearch(GetSearch()); }
				catch (...) { TsmLog(L"handler HideSearch on: exception"); }
				});
			checkBox.Unchecked([](Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
				try { SaveVal(L"HideSearch", 0); RestoreSearch(GetSearch()); }
				catch (...) { TsmLog(L"handler HideSearch off: exception"); }
				});
		}

		// 隐藏白边
		{
			auto checkBox = CheckBox();
			checkBox.Content(box_value(Tr().hideBorder));
			checkBox.IsChecked(GetVal(L"HideBorder", 0) == 1);
			stackPanel.Children().Append(checkBox);

			checkBox.Checked([](Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
				try {
					SaveVal(L"HideBorder", 1);
					if (auto border = GetOverlay())
						if (auto solid = border.Background().try_as<SolidColorBrush>())
							solid.Opacity(0);
				}
				catch (...) { TsmLog(L"handler HideBorder on: exception"); }
				});
			checkBox.Unchecked([](Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
				try {
					SaveVal(L"HideBorder", 0);
					if (auto border = GetOverlay())
						if (auto solid = border.Background().try_as<SolidColorBrush>())
							solid.Opacity(1);
				}
				catch (...) { TsmLog(L"handler HideBorder off: exception"); }
				});
		}

		// 隐藏推荐区
		{
			auto checkBox = CheckBox();
			checkBox.Content(box_value(Tr().hideRecommended));
			checkBox.IsChecked(GetVal(L"HideRecommended", 0) == 1);
			stackPanel.Children().Append(checkBox);

			checkBox.Checked([](Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
				try { SaveVal(L"HideRecommended", 1); ApplyHideRecommended(g_root); }
				catch (...) { TsmLog(L"handler HideRecommended on: exception"); }
				});
			checkBox.Unchecked([](Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
				try { SaveVal(L"HideRecommended", 0); RestoreRecommended(g_root); }
				catch (...) { TsmLog(L"handler HideRecommended off: exception"); }
				});
		}

		// 设置按钮（命名后由 PositionGearBesidePlaces 统一定位）
		Button bt;
		bt.Name(L"TranslucentSMSettings");
		bt.BorderThickness(Thickness{ 0 });
		bt.Background(SolidColorBrush(Colors::Transparent()));
		bt.BorderBrush(SolidColorBrush(Colors::Transparent()));
		ToolTipService::SetToolTip(bt, box_value(Tr().gearTooltip));

		auto f = FontIcon();
		f.FontSize(16);
		f.Glyph(L"\uE104");
		f.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
		bt.Content(winrt::box_value(f));
		bt.Padding({ 11.2,11.2,11.2,11.2 });
		bt.Width(38);

		Flyout flyout;
		flyout.Content(stackPanel);
		bt.Flyout(flyout);

		host.Children().Append(bt);

		if (!useLegacyPlacement)
		{
			// 默认摆位：贴电源按钮左侧（已验证不崩）。
			// 自动摆位（按固定夹图标队列间距）由 PositionGearBesidePlaces 完成，
			// 需 GearAutoPlace=1，且只在轮询线程执行。
			bt.HorizontalAlignment(HorizontalAlignment::Right);
			bt.VerticalAlignment(VerticalAlignment::Center);
			bt.Margin({ 0,0,56,0 });
		}
		else
		{
			bt.Margin({ -40,0,0,0 });   // 旧容器内沿用原版偏移
		}

		wchar_t msg[128];
		swprintf_s(msg, L"settings panel installed (legacy=%d)", useLegacyPlacement ? 1 : 0);
		TsmLog(msg);
	}
}
