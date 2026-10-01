# MemStat

A tiny always-on-top Windows widget that shows memory usage at a glance:

- **System RAM**: used / total
- **Each GPU**: dedicated VRAM used / total, plus shared memory in use

It's a single ~12 KB executable written in plain C against the Win32 API. It has no C runtime, no installer and no dependencies beyond what ships with Windows.

## Features

- Updates once per second
- Color-coded bars: blue under 75%, amber from 75%, red from 90%
- Supports multiple GPUs (up to 8), including integrated and discrete adapters
- Per-monitor DPI aware, so it stays crisp when you move it between displays
- Dark, borderless window with rounded corners on Windows 11
- Settings are saved between runs

## Usage

Run `MemStat.exe`. The widget opens in the top-right corner of the primary screen. Only one copy runs at a time.

| Action | Result |
| --- | --- |
| Left-click and drag | Move the widget |
| Drag the left or right edge | Change the width (the height always fits the content) |
| Right-click | Open the menu |

The right-click menu has these options:

- **Always on top**: keep the widget above other windows
- **Transparency**: Off, or 10% to 60%
- **Start with Windows**: run MemStat when you sign in
- **Exit**

## Requirements

- Windows 10 (version 1709 or later) or Windows 11
- Per-GPU memory figures come from the `GPU Adapter Memory` performance counters, which need a WDDM 2.x driver

## Building

You need [MinGW-w64](https://www.mingw-w64.org/) with `gcc` on your `PATH`. Then run:

```bash
build.cmd
```

This produces `MemStat.exe` next to the source. The build uses `-nostdlib` with a custom entry point (`WinMainCRTStartup`), so the binary links only against system DLLs: kernel32, user32, gdi32, advapi32, dwmapi, dxgi and pdh.

## How it works

- **RAM**: `GlobalMemoryStatusEx`
- **GPU list and VRAM size**: DXGI (`IDXGIFactory1::EnumAdapters1`). Software adapters are skipped.
- **GPU usage**: PDH counters `\GPU Adapter Memory(*)\Dedicated Usage` and `\Shared Usage`, matched to each adapter by its LUID

## Settings

Settings are stored in the registry under `HKEY_CURRENT_USER\Software\MemStat`:

| Value | Meaning |
| --- | --- |
| `X`, `Y` | Window position |
| `Width` | Window width at 96 DPI |
| `Topmost` | Always on top (1/0) |
| `Transparency` | Transparency percentage |

"Start with Windows" adds a `MemStat` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

To reset everything, exit MemStat and delete the `HKCU\Software\MemStat` key.

## License

[MIT](LICENSE)
