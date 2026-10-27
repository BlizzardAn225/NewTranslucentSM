# TranslucentSM（25H2 适配版）

让 Windows 11 开始菜单变成半透明 / 毛玻璃效果的小工具。
原理：用 XAML Diagnostics 把一个 TAP DLL 注入 `StartMenuExperienceHost.exe`，修改开始菜单的 XAML（`AcrylicBorder` 的 `AcrylicBrush` 参数）。

> 基于 [rounk-ctrl/TranslucentSM](https://github.com/rounk-ctrl/TranslucentSM)（GPL-3.0）适配，
> 面向 **Windows 11 25H2（26200）**；改动摘要见下文『本版本变化』。
> 英文 [README.md](README.md)。

## 特别说明：

由于我的电脑不会频繁更新系统，难以及时适配新版Windows，故提供Agent分析包 `TranslucentSM-porting-kit.zip`，供Agent分析适配其他版本的Windows.

## 功能

| 功能 | 说明 |
| --- | --- |
| 开始菜单半透明 | 修改 `AcrylicBrush` 的 `TintOpacity` / `TintLuminosityOpacity` |
| 滑块 ×2 | 实时调节色调浓度与亮度层（见下方"参数含义"） |
| 开关 ×3 | 隐藏搜索栏、隐藏白色边框、隐藏推荐区 |
| 设置入口 | 开始菜单底栏的铅笔按钮（电源按钮左侧）→ 弹出设置面板 |
| 中英双语界面 | 跟随系统语言；注册表 `Language=zh/en` 可强制 |
| 开机自启 | 见下文 |

## 参数含义（重要）

- **色调浓度**：控制**色调**由全彩到全黑白。
  >`100%` = 色调层完全不透明，整个表面变成实色。这是 Windows `AcrylicBrush` 的标准语义。
- **不透明度**：模糊背景之上的灰白罩强度。默认 30%。

## 使用

1. 把 `StartTAP.dll`、`start.exe` 放同一个目录。
2. 运行 `start.exe`（等 StartMenuExperienceHost 起来，最多等 30 秒）。
3. 打开开始菜单即可看到效果；点底栏铅笔按钮调参数，改动立即生效并写入注册表。

设置保存在 `HKCU\SOFTWARE\TranslucentSM`：

| 键 | 类型 | 含义 |
| --- | --- | --- |
| `TintOpacity` | DWORD 0-100 | 色调 |
| `TintLuminosityOpacity` | DWORD 0-100 | 不透明度 |
| `HideSearch` / `HideBorder` / `HideRecommended` | DWORD 0/1 | 三个开关 |
| `Language` | 字符串 `zh` / `en` | 强制界面语言（不设=跟随系统） |
| `GearAutoPlace` | DWORD 0/1 | 设置按钮自动摆位（默认开；0=旧的固定位置） |

## 开机自启与文件清单

运行一次 `start.exe` 即完成安装：把 **自身**（改名 `TranslucentSM.exe`）和 **`StartTAP.dll`**
复制到 `shell:startup` 目录，Windows 登录时直接运行该 exe 完成注入（无 VBS、不闪窗口、运行完即退出）。
再次运行会刷新这两个副本（升级）。移动工具目录不影响已部署的自启。

**开机登录进入桌面后，几秒内即自动注入成功**（`TranslucentSM.exe` 会等 `StartMenuExperienceHost.exe` 起来，最多等 30 秒），无需任何手动操作；注入完成后它自行退出。

### 写入的文件

| 位置 | 内容 |
| --- | --- |
| `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\TranslucentSM.exe` | 自启动副本（登录时运行一次后退出） |
| `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\StartTAP.dll` | TAP 副本（供上面的 exe 注入使用） |
| `%TEMP%\TranslucentSM.log` | 调试日志 |
| 工具目录 | `start.exe`、`StartTAP.dll`（你自己放的那套） |

### 写入的注册表（仅当前用户）

`HKCU\SOFTWARE\TranslucentSM`：`TintOpacity`、`TintLuminosityOpacity`（DWORD 0-100），
`HideSearch` / `HideBorder` / `HideRecommended` / `GearAutoPlace`（DWORD 0/1），
`Language`（字符串 `zh`/`en`）。

### 运行时涉及的进程

| 进程 | 说明 |
| --- | --- |
| `StartMenuExperienceHost.exe` | 系统的开始菜单进程，`StartTAP.dll` 注入其中；**在任务管理器结束它 → 全部 XAML 修改立即还原**（系统会自动把它拉起，之后不再注入，效果消失） |
| `TranslucentSM.exe` | 登录时运行一次做注入，随后自动退出，不常驻 |

### 手动卸载

1. 任务管理器结束 `StartMenuExperienceHost.exe`（开始菜单自动重启，效果随之消失）。
2. 删除 `Startup` 目录下的 `TranslucentSM.exe` 和 `StartTAP.dll`。
3. 删除工具目录。
4. （可选）删除注册表 `HKCU\SOFTWARE\TranslucentSM` 与 `%TEMP%\TranslucentSM.log`。

想临时跳过自启部署：`start.exe /noautostart`。

## 构建

只需要 **MSVC（v143+）** 和 **Windows SDK（10.0.22621+）**，不需要 NuGet / VS 解决方案：

```
python build\build_adapted.py
```

或双击 `build\build.cmd`。产物在 `build_out\`：`StartTAP.dll`、`start.exe`（x64，静态 CRT，无第三方依赖）。
完整编译日志写到 `build_out\build.log`。

## 杀软提示

本工具会把 XAML-Diagnostics TAP DLL 注入 `StartMenuExperienceHost.exe`，
Defender / SmartScreen 可能误报。源码全部开放，不放心可以自行用上面的命令编译。

## 许可

GPL-3.0（见 [LICENSE](LICENSE)）。改动部分同样以 GPL-3.0 发布。
