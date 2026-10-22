# 1-Remind!

A lightweight, native Windows 11 hourly reminder app that alerts you every time **1 hour of active PC use** has passed — with a soft two-tone **chime** or a spoken **“1 hour has passed”**, plus an optional Windows notification toast.

Created by **KnightWin** · Version **V1.0.0** · Licensed under **GNU GPLv3**

---

## Screenshots 

<img width="1917" height="1074" alt="Screenshot 2026-10-22 151944" src="https://github.com/user-attachments/assets/8031a2d0-ae67-4e8b-b963-0b7146fefb29" />
<img width="1918" height="1079" alt="Screenshot 2026-10-22 151840" src="https://github.com/user-attachments/assets/c4162811-1ae2-43a4-9a47-4104072f06ff" />



## Features

- **Tiny & Native:** Single self-contained C++/Win32 executable (`~307 KB`), zero external dependencies (no .NET, no Electron, no VC++ runtime required), uses only a few MB of RAM and ~0% CPU.
- **Smart Active-Time Tracking:** Counts actual PC usage via `QueryUnbiasedInterruptTime` — sleep/hibernate time is ignored and the timer automatically pauses while your PC is locked (`Win + L`).
- **Windows 11 Fluent UI:** Rounded corners, dark/light title bar, system accent color support, Segoe UI Variable typography, and subtle Fluent translucency (**8% in Dark mode, 3% in Light mode**).
- **Tray-Friendly & Clean Exit:**
  - **Minimize (`—`)** hides the app cleanly to the system tray (removes it from the taskbar).
  - **Close (`✕` / `Alt + F4`)** or **Tray → Exit** completely terminates the process immediately.
- **5 Simple Settings** (saved portably to `1-Remind.ini` next to the `.exe`):
  - **Appearance:** `System` / `Light` / `Dark`
  - **Alert sound:** `Chime` / `Speech`
  - **Run at startup:** Launch automatically when signing in to Windows
  - **Run minimized:** Start hidden in the system tray
  - **Notification toast:** Show an hourly Windows notification

---

## Quick Start

1. Download **`1-Remind!.exe`** from `dist/` (or the Releases page) and run it from any folder.
2. *(First launch only)* If Windows SmartScreen appears because the binary is unsigned, click **More info → Run anyway**.

---

## Building from Source

The entire application lives in `src/main.cpp` with Win32 resources in `src/resource.rc` and `src/app.manifest`.

### Option 1: Visual Studio (Windows)
Open an **x64 Native Tools Command Prompt for VS** and run:
```bat
build-msvc.bat
```

### Option 2: MinGW-w64 (MSYS2 on Windows, or Linux cross-compile)
```bash
./build-mingw.sh
```

Both scripts output the standalone binary to `dist/1-Remind!.exe`.
To regenerate the multi-size `.ico` from scratch, run `python3 tools/make_icon.py` (requires Pillow).

---

## Support 

If you like the app and want to support, it would be much appreciated! 
[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/N7D7292WUD)



## License

Copyright (C) 2026 **KnightWin**

This project is licensed under the **GNU General Public License v3.0** — see the [LICENSE](LICENSE) file for details.
