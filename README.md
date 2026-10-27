# TranslucentSM
A lightweight utility that makes the Windows Start Menu translucent/transparent.<br>
This app utilizes XAML Diagnostics to inject a dll into a process and modifies the XAML.

**中文：[README.zh-CN.md](README.zh-CN.md)** · The settings UI is bilingual (English/Chinese,
follows your Windows display language; force it with the `Language` registry value).

> Based on [rounk-ctrl/TranslucentSM](https://github.com/rounk-ctrl/TranslucentSM) (GPL-3.0),
> adapted for Windows 11 24H2/25H2. Change summary is listed below.

## Special note:

Since my computer does not update the system frequently, it is difficult for me to adapt to new versions of Windows in a timely manner. Therefore, I am providing the Agent analysis package `TranslucentSM-porting-kit.zip` for Agents to analyze and adapt to other versions of Windows.

## Features

| Feature | Description |
| --- | --- |
| Translucent Start menu | Modifies `TintOpacity` / `TintLuminosityOpacity` of `AcrylicBrush` |
| Sliders ×2 | Real-time adjustment of tint intensity and luminosity layer (see "Parameter meanings" below) |
| Toggles ×3 | Hide search bar, hide white border, hide recommended section |
| Settings entry | Pencil button in the Start menu bottom bar (left of the power button) → opens settings panel |
| Bilingual Chinese/English UI | Follows system language; registry `Language=zh/en` can force it |
| Auto-start on boot | See below |

## Parameter meanings (important)

- **Tint intensity**: Controls the **tint** from full color to full black-and-white.
  > `100%` = the tint layer is fully opaque, and the entire surface becomes a solid color. This is the standard semantics of Windows `AcrylicBrush`.
- **Opacity**: The strength of the gray-white overlay above the blurred background. Default 30%.

## Usage

1. Put `StartTAP.dll` and `start.exe` in the same directory.
2. Run `start.exe` (wait for StartMenuExperienceHost to start, up to 30 seconds).
3. Open the Start menu to see the effect; click the pencil button in the bottom bar to adjust parameters. Changes take effect immediately and are written to the registry.

Settings are stored in `HKCU\SOFTWARE\TranslucentSM`:

| Key | Type | Meaning |
| --- | --- | --- |
| `TintOpacity` | DWORD 0-100 | Tint |
| `TintLuminosityOpacity` | DWORD 0-100 | Opacity |
| `HideSearch` / `HideBorder` / `HideRecommended` | DWORD 0/1 | Three toggles |
| `Language` | String `zh` / `en` | Force UI language (unset = follow system) |
| `GearAutoPlace` | DWORD 0/1 | Auto-placement of settings button (default on; 0 = old fixed position) |

## Auto-start on boot and file list

Running `start.exe` once completes installation: it copies **itself** (renamed to `TranslucentSM.exe`) and **`StartTAP.dll`**
to the `shell:startup` directory. At Windows login, the exe runs directly to perform injection (no VBS, no window flash, exits after running).
Running it again refreshes these two copies (upgrade). Moving the tool directory does not affect the already deployed auto-start.

**After logging in to the desktop at boot, injection succeeds automatically within a few seconds** (`TranslucentSM.exe` waits for `StartMenuExperienceHost.exe` to start, up to 30 seconds), with no manual operation required; after injection completes, it exits by itself.

### Files written

| Location | Content |
| --- | --- |
| `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\TranslucentSM.exe` | Auto-start copy (runs once at login, then exits) |
| `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\StartTAP.dll` | TAP copy (used for injection by the exe above) |
| `%TEMP%\TranslucentSM.log` | Debug log |
| Tool directory | `start.exe`, `StartTAP.dll` (the set you placed yourself) |

### Registry written (current user only)

`HKCU\SOFTWARE\TranslucentSM`: `TintOpacity`, `TintLuminosityOpacity` (DWORD 0-100),
`HideSearch` / `HideBorder` / `HideRecommended` / `GearAutoPlace` (DWORD 0/1),
`Language` (string `zh`/`en`).

### Processes involved at runtime

| Process | Description |
| --- | --- |
| `StartMenuExperienceHost.exe` | The system Start menu process, into which `StartTAP.dll` is injected; **ending it in Task Manager → all XAML modifications are immediately restored** (the system will automatically restart it, after which injection no longer occurs and the effect disappears) |
| `TranslucentSM.exe` | Runs once at login to perform injection, then exits automatically and does not stay resident |

### Manual uninstall

1. End `StartMenuExperienceHost.exe` in Task Manager (the Start menu restarts automatically and the effect disappears).
2. Delete `TranslucentSM.exe` and `StartTAP.dll` in the `Startup` directory.
3. Delete the tool directory.
4. (Optional) Delete the registry `HKCU\SOFTWARE\TranslucentSM` and `%TEMP%\TranslucentSM.log`.

To temporarily skip auto-start deployment: `start.exe /noautostart`.

## Build

Only **MSVC (v143+)** and **Windows SDK (10.0.22621+)** are required; NuGet / VS solution is not needed:

```
python build\build_adapted.py
```

Or double-click `build\build.cmd`. Outputs are in `build_out\`: `StartTAP.dll`, `start.exe` (x64, static CRT, no third-party dependencies).
The full build log is written to `build_out\build.log`.

## Antivirus notice

This tool injects the XAML-Diagnostics TAP DLL into `StartMenuExperienceHost.exe`,
so Defender / SmartScreen may produce false positives. The source code is fully open; if you are concerned, you can compile it yourself using the commands above.

## License

GPL-3.0 (see [LICENSE](LICENSE)). Modified parts are also released under GPL-3.0.