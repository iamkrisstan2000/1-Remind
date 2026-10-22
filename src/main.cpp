// ============================================================================
//  1-Remind!  (V1.0.0)
//  Created by KnightWin
//
//  Copyright (C) 2026 KnightWin
//  SPDX-License-Identifier: GPL-3.0-or-later
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//  GNU General Public License for more details.
//
//  Design goals
//  ------------
//  * Native Win32 / C++ — no Electron, no .NET, no runtime to install.
//    One self-contained .exe, a few hundred KB, typically ~4-6 MB of RAM
//    while hidden in the tray, and effectively 0% CPU (it wakes twice a
//    second for a few microseconds to advance a counter).
//  * Fluent look on Windows 11: rounded window corners, immersive dark
//    title bar, Fluent window translucency (8% in Dark mode, 3% in Light
//    mode), system accent colour, Segoe UI Variable, card layout,
//    Win11-style toggle switches and segmented controls — all drawn with
//    plain GDI (no GDI+/Direct2D dependency to keep it lean).
//  * The hour counter measures *active PC time*: it uses
//    QueryUnbiasedInterruptTime (which does not advance while the PC is
//    asleep/hibernating) and pauses while the workstation is locked.
//  * Minimize hides to the system tray (gone from the taskbar).
//    Closing the window (X / Alt+F4) or choosing Exit from the tray menu
//    completely terminates the app process immediately.
//
//  Settings live in 1-Remind.ini next to the .exe (falling back to
//  %APPDATA%\1-Remind if that folder is not writable), keeping it portable.
// ============================================================================

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00          // target Windows 10/11 APIs
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#ifdef _MSC_VER                      // so the same file builds with plain `cl`
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const wchar_t* kAppName       = L"1-Remind!";
static const wchar_t* kAppVersion    = L"V1.0.0";
static const wchar_t* kClassName     = L"OneRemind.MainWindow.v1";
static const wchar_t* kMutexName     = L"OneRemind.SingleInstance.v1";
static const wchar_t* kRunValue      = L"1-Remind";      // HKCU\...\Run value
static const wchar_t* kLegacyRunVal  = L"HourlyChime";   // clean up older test build
static const int      kAppIconId     = 101;

// Fluent translucency levels requested:
//   Dark mode:  8% translucency => 92% opacity => round(255 * 0.92) = 235
//   Light mode: 3% translucency => 97% opacity => round(255 * 0.97) = 247
static const BYTE kAlphaDark  = 235;
static const BYTE kAlphaLight = 247;

static const UINT WM_TRAYICON = WM_APP + 1;
static const UINT WM_SHOWME   = WM_APP + 2;
static const UINT_PTR TIMER_ID = 1;
static const UINT TIMER_MS = 500;                       // 2 ticks / second
static const ULONGLONG HOUR_MS = 60ull * 60ull * 1000ull;

enum TrayCmd { ID_TRAY_OPEN = 1001, ID_TRAY_TEST, ID_TRAY_RESET, ID_TRAY_EXIT };

// DWM attribute ids (numeric so we don't depend on a very new SDK header)
static const DWORD DWMWA_DARK_MODE_NUM   = 20;   // DWMWA_USE_IMMERSIVE_DARK_MODE
static const DWORD DWMWA_CORNER_NUM      = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
static const DWORD DWMWA_BACKDROP_NUM    = 38;   // DWMWA_SYSTEMBACKDROP_TYPE
static const int   DWMWCP_ROUND_NUM      = 2;    // DWMWCP_ROUND
static const int   DWMSBT_MAINWINDOW_NUM = 2;    // Mica

#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#endif
#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#endif

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct Settings {
    int  theme     = 0;      // 0 = System, 1 = Light, 2 = Dark
    int  style     = 0;      // 0 = Chime,  1 = Speech
    bool startup   = true;   // default ON
    bool minimized = false;  // Run minimized (start hidden in tray)
    bool toast     = true;   // default ON
};

// ---------------------------------------------------------------------------
// Globals (this is a one-window app; a handful of globals keep it simple)
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst  = nullptr;
static HWND      g_hwnd   = nullptr;
static HANDLE    g_mutex  = nullptr;
static Settings  g_set;
static std::wstring g_iniPath;

static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded   = false;
static UINT g_msgTaskbarCreated = 0;

static bool      g_dark        = false;   // effective theme right now
static UINT      g_dpi         = 96;
static HFONT     g_fTitle      = nullptr;
static HFONT     g_fBig        = nullptr;
static HFONT     g_fBody       = nullptr;
static HFONT     g_fBodySemi   = nullptr;
static HFONT     g_fSmall      = nullptr;
static HFONT     g_fVersion    = nullptr;
static HFONT     g_fSection    = nullptr;
static HFONT     g_fBadgeOne   = nullptr;

static ULONGLONG g_elapsedMs    = 0;      // active-use time this session
static ULONGLONG g_lastTickMs   = 0;      // last "unbiased" timestamp seen
static ULONGLONG g_lastAlertHr  = 0;      // last hour we alerted for
static bool      g_locked       = false;
static bool      g_suspended    = false;
static bool      g_hintShown    = false;
static bool      g_startedHidden = false;
static long      g_lastTipMinute = -1;

static ISpVoice* g_voice = nullptr;       // created lazily (saves RAM)

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }   // DPI scale

static COLORREF Blend(COLORREF a, COLORREF b, int pctB) {   // pctB = 0..100 of b
    int r = GetRValue(a) + (GetRValue(b) - GetRValue(a)) * pctB / 100;
    int g = GetGValue(a) + (GetGValue(b) - GetGValue(a)) * pctB / 100;
    int bl = GetBValue(a) + (GetBValue(b) - GetBValue(a)) * pctB / 100;
    return RGB(r, g, bl);
}

struct Palette {
    COLORREF bg, card, cardBorder, text, subtext, accent, accentHover,
             accentText, segBg, segBorder, segHover, divider, track,
             btnSecondary, toggleOffBorder, toggleOffKnob, statusDot;
};

static COLORREF SystemAccent() {
    // HKCU\Software\Microsoft\Windows\DWM\AccentColor is stored as ABGR.
    DWORD val = 0, size = sizeof(val);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM",
                     L"AccentColor", RRF_RT_REG_DWORD, nullptr, &val, &size) == ERROR_SUCCESS) {
        return RGB(val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF);
    }
    return RGB(0, 103, 192);   // Windows default blue
}

static Palette Pal() {
    Palette p{};
    COLORREF rawAccent = SystemAccent();
    COLORREF accent = rawAccent;
    if (g_dark) {
        // Lighten the accent a touch in dark mode so it stays readable,
        // just like WinUI does (dark accent is a lighter tint).
        int lum = (GetRValue(accent) * 299 + GetGValue(accent) * 587 + GetBValue(accent) * 114) / 1000;
        if (lum < 150) accent = Blend(accent, RGB(255, 255, 255), 35);
        // Subtle Fluent Mica-like accent tint on the dark surface
        p.bg   = Blend(RGB(30, 30, 30), rawAccent, 4);
        p.card = Blend(RGB(42, 42, 42), rawAccent, 3);
        p.cardBorder = RGB(58, 58, 58);    p.text = RGB(255, 255, 255);
        p.subtext = RGB(166, 166, 166);    p.segBg = RGB(27, 27, 27);
        p.segBorder = RGB(60, 60, 60);     p.segHover = RGB(48, 48, 48);
        p.divider = RGB(52, 52, 52);       p.track = RGB(60, 60, 60);
        p.btnSecondary = RGB(50, 50, 50);  p.toggleOffBorder = RGB(138, 138, 138);
        p.toggleOffKnob = RGB(163, 163, 163);
        p.statusDot = RGB(108, 203, 95);
        p.accentHover = Blend(accent, RGB(255, 255, 255), 12);
    } else {
        // Subtle Fluent Mica-like accent tint on the light surface
        p.bg   = Blend(RGB(243, 243, 243), rawAccent, 2);
        p.card = RGB(255, 255, 255);
        p.cardBorder = RGB(224, 224, 224); p.text = RGB(27, 27, 27);
        p.subtext = RGB(93, 93, 93);       p.segBg = RGB(234, 234, 234);
        p.segBorder = RGB(206, 206, 206);  p.segHover = RGB(219, 219, 219);
        p.divider = RGB(229, 229, 229);    p.track = RGB(217, 217, 217);
        p.btnSecondary = RGB(251, 251, 251);
        p.toggleOffBorder = RGB(118, 118, 118);
        p.toggleOffKnob = RGB(93, 93, 93);
        p.statusDot = RGB(16, 124, 16);
        p.accentHover = Blend(accent, RGB(255, 255, 255), 10);
    }
    p.accent = accent;
    // Text on the accent colour: white unless the accent is very light.
    int lum = (GetRValue(accent) * 299 + GetGValue(accent) * 587 + GetBValue(accent) * 114) / 1000;
    p.accentText = (lum > 190) ? RGB(0, 0, 0) : RGB(255, 255, 255);
    return p;
}

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return buf;
}

static std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(0, pos + 1);
}

// Portable settings file: next to the exe if writable, else %APPDATA%.
static std::wstring ResolveIniPath() {
    std::wstring dir = ExeDir();
    std::wstring probe = dir + L".1r_write_test.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        DeleteFileW(probe.c_str());
        return dir + L"1-Remind.ini";
    }
    wchar_t appData[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData) == S_OK) {
        std::wstring d = std::wstring(appData) + L"\\1-Remind";
        CreateDirectoryW(d.c_str(), nullptr);
        return d + L"\\1-Remind.ini";
    }
    return dir + L"1-Remind.ini";
}

static void SaveSettings() {
    wchar_t v[16]{};
    swprintf_s(v, L"%d", g_set.theme);
    WritePrivateProfileStringW(L"Settings", L"Theme", v, g_iniPath.c_str());
    swprintf_s(v, L"%d", g_set.style);
    WritePrivateProfileStringW(L"Settings", L"NotificationStyle", v, g_iniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"RunAtStartup", g_set.startup ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"RunMinimized", g_set.minimized ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"NotificationToast", g_set.toast ? L"1" : L"0", g_iniPath.c_str());
}

static void LoadSettings() {
    bool firstRun = (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES);
    // If upgrading from HourlyChime.ini in the same folder, migrate settings seamlessly
    std::wstring readFrom = g_iniPath;
    if (firstRun) {
        std::wstring legacy = ExeDir() + L"HourlyChime.ini";
        if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
            readFrom = legacy;
    }
    g_set.theme     = GetPrivateProfileIntW(L"Settings", L"Theme", 0, readFrom.c_str());
    g_set.style     = GetPrivateProfileIntW(L"Settings", L"NotificationStyle", 0, readFrom.c_str());
    g_set.startup   = GetPrivateProfileIntW(L"Settings", L"RunAtStartup", 1, readFrom.c_str()) != 0;
    g_set.minimized = GetPrivateProfileIntW(L"Settings", L"RunMinimized", 0, readFrom.c_str()) != 0;
    g_set.toast     = GetPrivateProfileIntW(L"Settings", L"NotificationToast", 1, readFrom.c_str()) != 0;
    if (g_set.theme < 0 || g_set.theme > 2) g_set.theme = 0;
    if (g_set.style < 0 || g_set.style > 1) g_set.style = 0;
    if (firstRun) SaveSettings();
}

static void ApplyStartup(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    // Remove legacy value name if present from earlier test builds
    RegDeleteValueW(key, kLegacyRunVal);
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        if (g_set.minimized) cmd += L" --minimized";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

static bool SystemIsDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

static void ResolveTheme() {
    g_dark = (g_set.theme == 2) || (g_set.theme == 0 && SystemIsDark());
}

// Let Win32 context menus follow dark mode too (undocumented but stable
// uxtheme ordinals, used by File Explorer itself; harmless if absent).
static void ApplyMenuTheme() {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    typedef int (WINAPI *SetAppModeFn)(int);
    typedef void (WINAPI *FlushFn)();
    auto setMode = (SetAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush   = (FlushFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(g_dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
    FreeLibrary(ux);
}

static void ApplyWindowTheme() {
    if (!g_hwnd) return;
    BOOL dark = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_DARK_MODE_NUM, &dark, sizeof(dark));
    int corner = DWMWCP_ROUND_NUM;          // Windows 11 rounded corners
    DwmSetWindowAttribute(g_hwnd, DWMWA_CORNER_NUM, &corner, sizeof(corner));
    int backdrop = DWMSBT_MAINWINDOW_NUM;   // Mica (ignored pre-Win11)
    DwmSetWindowAttribute(g_hwnd, DWMWA_BACKDROP_NUM, &backdrop, sizeof(backdrop));

    // Apply Fluent translucency:
    //   Dark mode  = 8% translucency (92% opacity -> alpha 235)
    //   Light mode = 3% translucency (97% opacity -> alpha 247)
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED))
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    BYTE alpha = g_dark ? kAlphaDark : kAlphaLight;
    SetLayeredWindowAttributes(g_hwnd, 0, alpha, LWA_ALPHA);

    ApplyMenuTheme();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static UINT DpiForWindow(HWND hwnd) {
    typedef UINT (WINAPI *Fn)(HWND);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && hwnd) return fn(hwnd);
    HDC dc = GetDC(nullptr);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

static HFONT MakeFont(int pt, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt, (int)g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void CreateFonts() {
    HFONT old[] = { g_fTitle, g_fBig, g_fBody, g_fBodySemi, g_fSmall, g_fVersion, g_fSection, g_fBadgeOne };
    g_fTitle    = MakeFont(15, FW_SEMIBOLD, L"Segoe UI Variable Display");
    g_fBig      = MakeFont(32, FW_SEMIBOLD, L"Segoe UI Variable Display");
    g_fSection  = MakeFont(12, FW_SEMIBOLD, L"Segoe UI Variable Text");
    g_fBody     = MakeFont(10, FW_NORMAL,   L"Segoe UI Variable Text");
    g_fBodySemi = MakeFont(10, FW_SEMIBOLD, L"Segoe UI Variable Text");
    g_fSmall    = MakeFont( 9, FW_NORMAL,   L"Segoe UI Variable Text");
    g_fVersion  = MakeFont( 8, FW_NORMAL,   L"Segoe UI Variable Text");
    g_fBadgeOne = MakeFont( 8, FW_BOLD,     L"Segoe UI Variable Display");
    for (HFONT f : old) if (f) DeleteObject(f);
}

// "Unbiased" time ignores sleep/hibernate — exactly what we want to count.
static ULONGLONG NowActiveMs() {
    ULONGLONG t = 0;
    if (QueryUnbiasedInterruptTime(&t)) return t / 10000ull;   // 100ns -> ms
    return GetTickCount64();
}

static std::wstring FormatHMS(ULONGLONG ms) {
    ULONGLONG s = ms / 1000;
    wchar_t b[32]{};
    swprintf_s(b, L"%02llu:%02llu:%02llu", s / 3600, (s % 3600) / 60, s % 60);
    return b;
}

// ---------------------------------------------------------------------------
// Sound: a gentle two-tone chime, synthesised once into a static WAV buffer
// (no sound file to ship). Speech: SAPI via ISpVoice, created lazily.
// ---------------------------------------------------------------------------
static const std::vector<BYTE>& ChimeData() {
    static std::vector<BYTE> wav;
    if (!wav.empty()) return wav;

    const int sr = 44100;
    const double seconds = 2.2;
    const int n = (int)(sr * seconds);
    const double pi = 3.14159265358979323846;
    const double f1 = 1318.51;         // E6
    const double f2 = 1046.50;         // C6  ("ding-dong")
    const double t2start = 0.28;

    std::vector<short> samples((size_t)n);
    for (int i = 0; i < n; ++i) {
        double t = (double)i / sr;
        double attack1 = (t < 0.005) ? t / 0.005 : 1.0;
        double v = (std::sin(2 * pi * f1 * t) * 0.55 + std::sin(2 * pi * f1 * 2 * t) * 0.14)
                   * std::exp(-3.0 * t) * attack1;
        if (t >= t2start) {
            double t2 = t - t2start;
            double attack2 = (t2 < 0.005) ? t2 / 0.005 : 1.0;
            v += (std::sin(2 * pi * f2 * t2) * 0.55 + std::sin(2 * pi * f2 * 2 * t2) * 0.14)
                 * std::exp(-3.0 * t2) * attack2;
        }
        if (v > 1.0) v = 1.0; else if (v < -1.0) v = -1.0;
        samples[(size_t)i] = (short)(v * 0.65 * 32767.0);
    }

    // Minimal 44-byte PCM WAV header.
    wav.resize(44 + samples.size() * 2);
    BYTE* p = wav.data();
    auto w32 = [&](size_t off, DWORD v) { memcpy(p + off, &v, 4); };
    auto w16 = [&](size_t off, WORD v)  { memcpy(p + off, &v, 2); };
    memcpy(p + 0, "RIFF", 4);  w32(4, (DWORD)(36 + samples.size() * 2));
    memcpy(p + 8, "WAVE", 4);  memcpy(p + 12, "fmt ", 4);
    w32(16, 16); w16(20, 1); w16(22, 1);                       // PCM, mono
    w32(24, (DWORD)sr); w32(28, (DWORD)sr * 2); w16(32, 2); w16(34, 16);
    memcpy(p + 36, "data", 4); w32(40, (DWORD)(samples.size() * 2));
    memcpy(p + 44, samples.data(), samples.size() * 2);
    return wav;
}

static void PlayChime() {
    const std::vector<BYTE>& d = ChimeData();   // static -> safe for SND_ASYNC
    PlaySoundW((LPCWSTR)d.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static bool Speak(const std::wstring& text) {
    if (!g_voice) {
        if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                    IID_ISpVoice, (void**)&g_voice)) || !g_voice)
            return false;
    }
    HRESULT hr = g_voice->Speak(text.c_str(),
                                SVSFlagsAsync | SVSFPurgeBeforeSpeak, nullptr);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Tray icon + toast (Shell_NotifyIcon balloon; Windows 11 renders these as
// proper notification toasts / in Notification Center for desktop apps)
// ---------------------------------------------------------------------------
static void UpdateTrayTip(bool force = false) {
    ULONGLONG mins = (g_elapsedMs / 60000ull);
    if (!force && (long)mins == g_lastTipMinute) return;
    g_lastTipMinute = (long)mins;
    ULONGLONG remainMin = (HOUR_MS - (g_elapsedMs % HOUR_MS)) / 60000ull;
    wchar_t tip[128]{};
    swprintf_s(tip, L"%s \u2014 %s elapsed \u2022 next alert in %llum",
               kAppName, FormatHMS(g_elapsedMs).c_str(), remainMin + (remainMin == 0 ? 1 : 0));
    wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
    g_nid.uFlags = NIF_TIP | NIF_GUID;   // keep identifying the icon by GUID
    if (g_trayAdded) Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    // {8A1E4C20-9B3D-4E6F-A542-7C9D8E1F03B6} — stable tray identity for 1-Remind!
    g_nid.guidItem = { 0x8a1e4c20, 0x9b3d, 0x4e6f, { 0xa5, 0x42, 0x7c, 0x9d, 0x8e, 0x1f, 0x03, 0xb6 } };
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_GUID | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(kAppIconId),
                                    IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, kAppName, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    g_trayAdded = true;
    g_lastTipMinute = -1;
    UpdateTrayTip(true);
}

static void RemoveTrayIcon() {
    if (g_trayAdded) {
        g_nid.uFlags = NIF_GUID;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    }
    g_trayAdded = false;
}

static void ShowToast(const std::wstring& title, const std::wstring& body) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO | NIF_GUID;
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;      // we supply our own sound
    n.uTimeout = 8000;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, body.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------------------
// Complete, immediate process shutdown on exit
// ---------------------------------------------------------------------------
static void ShutdownAndKillApp() {
    if (g_hwnd) {
        KillTimer(g_hwnd, TIMER_ID);
        WTSUnRegisterSessionNotification(g_hwnd);
    }
    RemoveTrayIcon();
    PlaySoundW(nullptr, nullptr, 0);               // halt any async chime audio
    if (g_mutex) {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
    // Hard-kill the current process after removing the tray icon and releasing
    // the single-instance mutex so no SAPI/COM/winmm worker thread can ever
    // keep the process lingering in Task Manager.
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

// ---------------------------------------------------------------------------
// The hourly alert itself
// ---------------------------------------------------------------------------
static void FireAlert(ULONGLONG hours, bool isTest) {
    std::wstring text;
    if (isTest) {
        text = L"Test alert \u2014 1 hour has passed.";
    } else if (hours == 1) {
        text = L"1 hour has passed.";
    } else {
        wchar_t b[64]{};
        swprintf_s(b, L"%llu hours have passed.", hours);
        text = b;
    }

    if (g_set.toast) {
        std::wstring body = text;
        if (!isTest) {
            wchar_t b[96]{};
            swprintf_s(b, L"%s Total PC use this session: %s",
                       text.c_str(), FormatHMS(g_elapsedMs).c_str());
            body = b;
        }
        ShowToast(kAppName, body);
    }

    if (g_set.style == 1) {                    // Speech
        if (!Speak(text)) PlayChime();         // fall back gracefully
    } else {                                   // Chime
        PlayChime();
    }
}

// ---------------------------------------------------------------------------
// Layout + custom painting (Fluent-style cards, segments, toggles)
// ---------------------------------------------------------------------------
enum Hit {
    HIT_NONE = 0, HIT_TEST, HIT_RESET,
    HIT_THEME_0, HIT_THEME_1, HIT_THEME_2,
    HIT_STYLE_0, HIT_STYLE_1,
    HIT_STARTUP, HIT_MINIMIZED, HIT_TOAST
};
static Hit g_hover = HIT_NONE;
static Hit g_pressed = HIT_NONE;

struct Layout {
    RECT badge, timerCard, progress, btnTest, btnReset;
    RECT settingsCard;
    RECT segThemeBox, segTheme[3];
    RECT segStyleBox, segStyle[2];
    RECT togStartup, togMinimized, togToast;
    RECT rowTheme, rowStyle, rowStartup, rowMinimized, rowToast;
};

static Layout ComputeLayout(const RECT& rc) {
    Layout L{};
    int pad = S(20);
    int W = rc.right - rc.left;
    L.badge = { pad, S(18), pad + S(40), S(58) };

    // Timer card
    L.timerCard = { pad, S(84), W - pad, S(84) + S(182) };
    int ix0 = L.timerCard.left + S(18), ix1 = L.timerCard.right - S(18);
    L.progress = { ix0, L.timerCard.top + S(94), ix1, L.timerCard.top + S(100) };
    int by = L.timerCard.top + S(136);
    L.btnTest  = { ix0, by, ix0 + S(124), by + S(32) };
    L.btnReset = { ix0 + S(134), by, ix0 + S(134) + S(96), by + S(32) };

    // Settings card: 5 rows of 56px
    int sy = S(306), rowH = S(56);
    L.settingsCard = { pad, sy, W - pad, sy + rowH * 5 };
    L.rowTheme     = { pad, sy,            W - pad, sy + rowH };
    L.rowStyle     = { pad, sy + rowH,     W - pad, sy + rowH * 2 };
    L.rowStartup   = { pad, sy + rowH * 2, W - pad, sy + rowH * 3 };
    L.rowMinimized = { pad, sy + rowH * 3, W - pad, sy + rowH * 4 };
    L.rowToast     = { pad, sy + rowH * 4, W - pad, sy + rowH * 5 };

    int rx1 = W - pad - S(16);
    int segW = S(188), segH = S(30);
    auto segBoxFor = [&](const RECT& row) {
        int cy = (row.top + row.bottom) / 2;
        return RECT{ rx1 - segW, cy - segH / 2, rx1, cy + segH / 2 };
    };
    L.segThemeBox = segBoxFor(L.rowTheme);
    for (int i = 0; i < 3; ++i) {
        int w = (L.segThemeBox.right - L.segThemeBox.left) / 3;
        L.segTheme[i] = { L.segThemeBox.left + w * i, L.segThemeBox.top,
                          (i == 2) ? L.segThemeBox.right : L.segThemeBox.left + w * (i + 1),
                          L.segThemeBox.bottom };
    }
    L.segStyleBox = segBoxFor(L.rowStyle);
    for (int i = 0; i < 2; ++i) {
        int w = (L.segStyleBox.right - L.segStyleBox.left) / 2;
        L.segStyle[i] = { L.segStyleBox.left + w * i, L.segStyleBox.top,
                          (i == 1) ? L.segStyleBox.right : L.segStyleBox.left + w * (i + 1),
                          L.segStyleBox.bottom };
    }
    auto toggleFor = [&](const RECT& row) {
        int cy = (row.top + row.bottom) / 2;
        return RECT{ rx1 - S(40), cy - S(10), rx1, cy + S(10) };
    };
    L.togStartup   = toggleFor(L.rowStartup);
    L.togMinimized = toggleFor(L.rowMinimized);
    L.togToast     = toggleFor(L.rowToast);
    return L;
}

static Hit HitTest(const Layout& L, POINT pt) {
    auto in = [&](const RECT& r) { return PtInRect(&r, pt) != FALSE; };
    if (in(L.btnTest))  return HIT_TEST;
    if (in(L.btnReset)) return HIT_RESET;
    for (int i = 0; i < 3; ++i) if (in(L.segTheme[i])) return (Hit)(HIT_THEME_0 + i);
    for (int i = 0; i < 2; ++i) if (in(L.segStyle[i])) return (Hit)(HIT_STYLE_0 + i);
    RECT ts = L.togStartup, tm = L.togMinimized, tt = L.togToast;
    InflateRect(&ts, S(34), S(8));
    InflateRect(&tm, S(34), S(8));
    InflateRect(&tt, S(34), S(8)); // incl. On/Off label
    if (in(ts)) return HIT_STARTUP;
    if (in(tm)) return HIT_MINIMIZED;
    if (in(tt)) return HIT_TOAST;
    return HIT_NONE;
}

// --- little drawing helpers -------------------------------------------------
static void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}
static void RoundFrame(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}
static void FillEllipse(HDC dc, const RECT& r, COLORREF fill) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}
static void Text(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, of);
}

static void DrawSegmented(HDC dc, const RECT& box, const RECT* segs, int count,
                          const wchar_t** labels, int selected, Hit firstHit,
                          const Palette& p) {
    RoundFrame(dc, box, S(6), p.segBg, p.segBorder);
    for (int i = 0; i < count; ++i) {
        RECT r = segs[i];
        Hit h = (Hit)(firstHit + i);
        if (i == selected) {
            RECT sel = r; InflateRect(&sel, -S(3), -S(3));
            FillRound(dc, sel, S(5), (g_pressed == h) ? p.accentHover : p.accent);
            Text(dc, labels[i], r, g_fBodySemi, p.accentText,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            if (g_hover == h) {
                RECT hov = r; InflateRect(&hov, -S(3), -S(3));
                FillRound(dc, hov, S(5), p.segHover);
            }
            Text(dc, labels[i], r, g_fBody, p.text,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

static void DrawToggle(HDC dc, const RECT& r, bool on, const Palette& p) {
    if (on) {
        FillRound(dc, r, S(10), p.accent);
        int d = S(12), cy = (r.top + r.bottom) / 2;
        RECT knob = { r.right - S(4) - d, cy - d / 2, r.right - S(4), cy + d / 2 };
        FillEllipse(dc, knob, RGB(255, 255, 255));
    } else {
        RoundFrame(dc, r, S(10), p.segBg, p.toggleOffBorder);
        int d = S(10), cy = (r.top + r.bottom) / 2;
        RECT knob = { r.left + S(5), cy - d / 2, r.left + S(5) + d, cy + d / 2 };
        FillEllipse(dc, knob, p.toggleOffKnob);
    }
    RECT lbl = { r.left - S(38), r.top - S(4), r.left - S(8), r.bottom + S(4) };
    Text(dc, on ? L"On" : L"Off", lbl, g_fBody, p.text,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// Header badge: renders the embedded multi-size Fluent hourglass+1 icon
// directly if available, with a crisp GDI hourglass+1 fallback.
static void DrawBadge(HDC dc, const RECT& r, const Palette& p) {
    int w = r.right - r.left, h = r.bottom - r.top;
    HICON hIco = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(kAppIconId),
                                   IMAGE_ICON, w, h, LR_SHARED);
    if (hIco && DrawIconEx(dc, r.left, r.top, hIco, w, h, 0, nullptr, DI_NORMAL))
        return;

    // Fallback GDI hourglass + "1" badge
    FillRound(dc, r, S(10), p.accent);
    int cx = (r.left + r.right) / 2 - S(2), cy = (r.top + r.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, (std::max)(2, S(2)), p.accentText);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, cx - S(8), cy - S(10), nullptr); LineTo(dc, cx + S(8), cy - S(10));
    MoveToEx(dc, cx - S(8), cy + S(10), nullptr); LineTo(dc, cx + S(8), cy + S(10));
    POINT hg[6] = {
        { cx - S(7), cy - S(10) }, { cx + S(7), cy - S(10) },
        { cx + S(1), cy },         { cx + S(7), cy + S(10) },
        { cx - S(7), cy + S(10) }, { cx - S(1), cy }
    };
    Polygon(dc, hg, 6);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
    RECT b = { r.right - S(16), r.bottom - S(16), r.right - S(2), r.bottom - S(2) };
    RoundFrame(dc, b, S(7), Blend(p.accent, RGB(0, 0, 0), 35), p.accentText);
    Text(dc, L"1", b, g_fBadgeOne, p.accentText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void Paint(HWND hwnd, HDC hdcIn) {
    RECT rc; GetClientRect(hwnd, &rc);
    // Double-buffered to avoid any flicker.
    HDC dc = CreateCompatibleDC(hdcIn);
    HBITMAP bmp = CreateCompatibleBitmap(hdcIn, rc.right - rc.left, rc.bottom - rc.top);
    HGDIOBJ obmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    Palette p = Pal();
    Layout L = ComputeLayout(rc);
    int pad = S(20);
    int W = rc.right - rc.left;

    HBRUSH bg = CreateSolidBrush(p.bg);
    FillRect(dc, &rc, bg); DeleteObject(bg);

    // ---- header -------------------------------------------------------------
    DrawBadge(dc, L.badge, p);
    RECT tr = { L.badge.right + S(12), S(18), W - pad, S(44) };
    Text(dc, kAppName, tr, g_fTitle, p.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    tr = { L.badge.right + S(12), S(42), W - pad, S(62) };
    Text(dc, L"A gentle alert for every hour of PC use", tr, g_fSmall, p.subtext,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // ---- timer card ----------------------------------------------------------
    RoundFrame(dc, L.timerCard, S(8), p.card, p.cardBorder);
    int ix0 = L.timerCard.left + S(18), ix1 = L.timerCard.right - S(18);
    RECT r = { ix0, L.timerCard.top + S(14), ix1, L.timerCard.top + S(32) };
    Text(dc, L"ELAPSED THIS SESSION", r, g_fSmall, p.subtext,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    r = { ix0, L.timerCard.top + S(32), ix1, L.timerCard.top + S(84) };
    Text(dc, FormatHMS(g_elapsedMs).c_str(), r, g_fBig, p.text,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // progress towards the next hour
    ULONGLONG within = g_elapsedMs % HOUR_MS;
    FillRound(dc, L.progress, S(3), p.track);
    int pw = L.progress.right - L.progress.left;
    int fill = (int)((within * (ULONGLONG)pw) / HOUR_MS);
    if (fill > 0) {
        HRGN clip = CreateRoundRectRgn(L.progress.left, L.progress.top,
                                       L.progress.right + 1, L.progress.bottom + 1, S(6), S(6));
        SelectClipRgn(dc, clip);
        RECT fr = { L.progress.left, L.progress.top, L.progress.left + fill, L.progress.bottom };
        HBRUSH ab = CreateSolidBrush(p.accent);
        FillRect(dc, &fr, ab); DeleteObject(ab);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    }
    ULONGLONG remain = HOUR_MS - within;
    wchar_t buf[96]{};
    swprintf_s(buf, L"Next alert in %02llu:%02llu",
               (remain / 1000) / 60, (remain / 1000) % 60);
    r = { ix0, L.timerCard.top + S(106), ix1, L.timerCard.top + S(126) };
    Text(dc, buf, r, g_fSmall, p.subtext, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    swprintf_s(buf, L"%llu%% of this hour", (within * 100) / HOUR_MS);
    Text(dc, buf, r, g_fSmall, p.subtext, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // buttons
    bool testHov = (g_hover == HIT_TEST), testDown = (g_pressed == HIT_TEST);
    FillRound(dc, L.btnTest, S(6), testDown ? Blend(p.accent, RGB(0, 0, 0), 8)
                                   : (testHov ? p.accentHover : p.accent));
    Text(dc, L"Test alert", L.btnTest, g_fBodySemi, p.accentText,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    bool rstHov = (g_hover == HIT_RESET), rstDown = (g_pressed == HIT_RESET);
    RoundFrame(dc, L.btnReset, S(6), rstDown ? p.segHover : (rstHov ? p.segBg : p.btnSecondary), p.cardBorder);
    Text(dc, L"Reset", L.btnReset, g_fBody, p.text,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // running / paused status (right of the buttons)
    bool paused = g_locked || g_suspended;
    const wchar_t* status = paused ? L"Paused" : L"Running";
    int sy2 = (L.btnTest.top + L.btnTest.bottom) / 2;
    RECT sr = { L.btnReset.right + S(14), L.btnTest.top, ix1, L.btnTest.bottom };
    RECT dot = { sr.left, sy2 - S(4), sr.left + S(8), sy2 + S(4) };
    FillEllipse(dc, dot, paused ? p.subtext : p.statusDot);
    sr.left += S(14);
    Text(dc, status, sr, g_fSmall, p.subtext, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // ---- settings ------------------------------------------------------------
    r = { pad, S(278), W - pad, S(302) };
    Text(dc, L"Settings", r, g_fSection, p.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RoundFrame(dc, L.settingsCard, S(8), p.card, p.cardBorder);
    int lx = pad + S(16);
    auto rowLabel = [&](const RECT& row, const wchar_t* label, const wchar_t* desc) {
        if (desc) {
            RECT a = { lx, row.top + S(8), row.right - S(110), row.top + S(28) };
            Text(dc, label, a, g_fBodySemi, p.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RECT b = { lx, row.top + S(28), row.right - S(110), row.top + S(47) };
            Text(dc, desc, b, g_fSmall, p.subtext, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else {
            RECT a = { lx, row.top, row.right - S(210), row.bottom };
            Text(dc, label, a, g_fBodySemi, p.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    };
    auto divider = [&](const RECT& row) {
        HPEN pen = CreatePen(PS_SOLID, 1, p.divider);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, lx, row.bottom, nullptr);
        LineTo(dc, L.settingsCard.right, row.bottom);
        SelectObject(dc, op); DeleteObject(pen);
    };

    rowLabel(L.rowTheme, L"Appearance", nullptr);
    static const wchar_t* themeLabels[] = { L"System", L"Light", L"Dark" };
    DrawSegmented(dc, L.segThemeBox, L.segTheme, 3, themeLabels, g_set.theme, HIT_THEME_0, p);
    divider(L.rowTheme);

    rowLabel(L.rowStyle, L"Alert sound", nullptr);
    static const wchar_t* styleLabels[] = { L"Chime", L"Speech" };
    DrawSegmented(dc, L.segStyleBox, L.segStyle, 2, styleLabels, g_set.style, HIT_STYLE_0, p);
    divider(L.rowStyle);

    rowLabel(L.rowStartup, L"Run at startup", L"Launch automatically when you sign in");
    DrawToggle(dc, L.togStartup, g_set.startup, p);
    divider(L.rowStartup);

    rowLabel(L.rowMinimized, L"Run minimized", L"Start hidden in the system tray");
    DrawToggle(dc, L.togMinimized, g_set.minimized, p);
    divider(L.rowMinimized);

    rowLabel(L.rowToast, L"Notification toast", L"Show a Windows notification each hour");
    DrawToggle(dc, L.togToast, g_set.toast, p);

    // ---- footer ----------------------------------------------------------------
    r = { pad, S(590), W - pad, S(608) };
    Text(dc, L"Minimize hides to tray \u2022 Close or tray Exit quits completely",
         r, g_fSmall, p.subtext, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    r = { pad, S(609), W - pad, S(626) };
    Text(dc, kAppVersion,
         r, g_fVersion, Blend(p.subtext, p.bg, 25), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    BitBlt(hdcIn, 0, 0, rc.right - rc.left, rc.bottom - rc.top, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------
// Window show/hide behaviour
// ---------------------------------------------------------------------------
static void RestoreFromTray() {
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void HideToTray() {
    ShowWindow(g_hwnd, SW_HIDE);      // hidden window => no taskbar button
    UpdateTrayTip(true);
    if (!g_hintShown && !g_startedHidden) {
        g_hintShown = true;
        ShowToast(kAppName, L"Minimized to the system tray. "
                            L"Click the tray icon to reopen.");
    }
}

static void ShowTrayMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN,  L"Open 1-Remind!");
    AppendMenuW(menu, MF_STRING, ID_TRAY_TEST,  L"Test alert");
    AppendMenuW(menu, MF_STRING, ID_TRAY_RESET, L"Reset timer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT,  L"Exit");
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);
    SetForegroundWindow(g_hwnd);       // required so the menu dismisses properly
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

static void DoAction(Hit h) {
    switch (h) {
    case HIT_TEST:
        FireAlert(1, true);
        break;
    case HIT_RESET:
        g_elapsedMs = 0;
        g_lastAlertHr = 0;
        UpdateTrayTip(true);
        break;
    case HIT_THEME_0: case HIT_THEME_1: case HIT_THEME_2:
        g_set.theme = (int)h - (int)HIT_THEME_0;
        SaveSettings();
        ResolveTheme();
        ApplyWindowTheme();
        break;
    case HIT_STYLE_0: case HIT_STYLE_1:
        g_set.style = (int)h - (int)HIT_STYLE_0;
        SaveSettings();
        break;
    case HIT_STARTUP:
        g_set.startup = !g_set.startup;
        ApplyStartup(g_set.startup);
        SaveSettings();
        break;
    case HIT_MINIMIZED:
        g_set.minimized = !g_set.minimized;
        if (g_set.startup) ApplyStartup(true);
        SaveSettings();
        break;
    case HIT_TOAST:
        g_set.toast = !g_set.toast;
        SaveSettings();
        break;
    default: break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void OnTick() {
    ULONGLONG now = NowActiveMs();
    ULONGLONG delta = (now >= g_lastTickMs) ? (now - g_lastTickMs) : 0;
    g_lastTickMs = now;
    if (delta > 60ull * 1000ull) delta = TIMER_MS;   // clock jump safety
    if (!g_locked && !g_suspended) g_elapsedMs += delta;

    ULONGLONG hours = g_elapsedMs / HOUR_MS;
    if (hours >= 1 && hours > g_lastAlertHr) {
        g_lastAlertHr = hours;
        FireAlert(hours, false);
    }
    if (IsWindowVisible(g_hwnd)) InvalidateRect(g_hwnd, nullptr, FALSE);
    UpdateTrayTip();
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && msg != 0) {   // Explorer restarted
        g_trayAdded = false;
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_dpi = DpiForWindow(hwnd);
        CreateFonts();
        ApplyWindowTheme();
        AddTrayIcon();
        g_lastTickMs = NowActiveMs();
        SetTimer(hwnd, TIMER_ID, TIMER_MS, nullptr);
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ID) OnTick();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT rc; GetClientRect(hwnd, &rc);
        Hit h = HitTest(ComputeLayout(rc), pt);
        if (h != g_hover) { g_hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(nullptr, h != HIT_NONE ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = HIT_NONE;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT rc; GetClientRect(hwnd, &rc);
        g_pressed = HitTest(ComputeLayout(rc), pt);
        if (g_pressed != HIT_NONE) { SetCapture(hwnd); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT rc; GetClientRect(hwnd, &rc);
        Hit h = HitTest(ComputeLayout(rc), pt);
        Hit was = g_pressed;
        g_pressed = HIT_NONE;
        ReleaseCapture();
        if (h != HIT_NONE && h == was) DoAction(h);
        else InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SYSCOMMAND:
        // Minimize hides to the system tray; Close (SC_CLOSE) exits completely.
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            HideToTray();
            return 0;
        }
        break;

    case WM_CLOSE:
        ShutdownAndKillApp();
        return 0;

    case WM_DESTROY:
        ShutdownAndKillApp();
        return 0;

    case WM_ENDSESSION:
        if (wp) ShutdownAndKillApp();
        return 0;

    case WM_TRAYICON: {
        // NOTIFYICON_VERSION_4 protocol: LOWORD(lParam) is the event and
        // wParam carries the anchor point. Act on NIN_SELECT / WM_CONTEXTMENU
        // only — the raw button messages arrive alongside them, and handling
        // both would open the menu twice for a single click.
        UINT evt = LOWORD(lp);
        if (evt == NIN_SELECT || evt == NIN_KEYSELECT || evt == NIN_BALLOONUSERCLICK) {
            RestoreFromTray();
        } else if (evt == WM_CONTEXTMENU) {
            POINT pt = { (int)(short)LOWORD(wp), (int)(short)HIWORD(wp) };
            if (pt.x <= 0 && pt.y <= 0) GetCursorPos(&pt);
            ShowTrayMenu(pt);
        }
        return 0;
    }

    case WM_SHOWME:
        RestoreFromTray();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TRAY_OPEN:  RestoreFromTray(); break;
        case ID_TRAY_TEST:  FireAlert(1, true); break;
        case ID_TRAY_RESET: g_elapsedMs = 0; g_lastAlertHr = 0; UpdateTrayTip(true); break;
        case ID_TRAY_EXIT:  ShutdownAndKillApp(); break;
        }
        return 0;

    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_locked = true;
        else if (wp == WTS_SESSION_UNLOCK) g_locked = false;
        g_lastTickMs = NowActiveMs();
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) g_suspended = true;
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) g_suspended = false;
        g_lastTickMs = NowActiveMs();
        return TRUE;

    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            ResolveTheme();
            ApplyWindowTheme();
        }
        return 0;

    case WM_DWMCOLORIZATIONCOLORCHANGED:
        InvalidateRect(hwnd, nullptr, FALSE);   // accent colour changed
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        CreateFonts();
        RECT* sug = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;

    // Per-monitor-V2 DPI awareness (the manifest says the same).
    typedef BOOL (WINAPI *DpiFn)(HANDLE);
    if (auto fn = (DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                        "SetProcessDpiAwarenessContext"))
        fn((HANDLE)(INT_PTR)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

    // Single instance: a second launch just opens the running copy.
    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClassName, kAppName))
            PostMessageW(existing, WM_SHOWME, 0, 0);
        return 0;
    }

    bool cmdMinimized = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i)
            if (lstrcmpiW(argv[i], L"--minimized") == 0) cmdMinimized = true;
        LocalFree(argv);
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_iniPath = ResolveIniPath();
    LoadSettings();
    ApplyStartup(g_set.startup);   // refresh path & clean up legacy Run value
    ResolveTheme();

    bool startHidden = cmdMinimized || g_set.minimized;
    g_startedHidden = startHidden;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(kAppIconId),
                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(kAppIconId),
                                   IMAGE_ICON, 16, 16, LR_SHARED);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g_dpi = DpiForWindow(nullptr);
    RECT wr = { 0, 0, S(420), S(634) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD exStyle = WS_EX_LAYERED;
    typedef BOOL (WINAPI *AdjFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto adj = (AdjFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                     "AdjustWindowRectExForDpi");
    if (adj) adj(&wr, style, FALSE, exStyle, g_dpi);
    else AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    int sx = (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2;
    int syy = (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2;

    g_hwnd = CreateWindowExW(exStyle, kClassName, kAppName, style,
                             sx, syy, wr.right - wr.left, wr.bottom - wr.top,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) { CoUninitialize(); return 1; }

    if (!startHidden) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownAndKillApp();
    return 0;
}
