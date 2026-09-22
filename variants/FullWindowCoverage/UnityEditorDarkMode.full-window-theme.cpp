// Including SDKDDKVer.h defines the highest available Windows platform.
#include <SDKDDKVer.h>

// Exclude rarely-used stuff from Windows headers
#define WIN32_LEAN_AND_MEAN

// Windows header files
#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>
#include <atlstr.h>

// COM header files
#include <ole2.h>

// Generic C++ stuff
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// for subclassing
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include <Uxtheme.h>
#pragma comment(lib, "uxtheme.lib")

#include <vsstyle.h>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

// inipp from https://github.com/mcmtroffaes/inipp
#include "inipp.h"

// window messages related to menu bar drawing
enum
{
    WM_UAHDESTROYWINDOW = 0x0090,
    WM_UAHDRAWMENU = 0x0091,
    WM_UAHDRAWMENUITEM = 0x0092,
    WM_UAHINITMENU = 0x0093,
    WM_UAHMEASUREMENUITEM = 0x0094,
    WM_UAHNCPAINTMENUPOPUP = 0x0095
};

// undocumented app mode enum for the private SetPreferredAppMode API
enum class PreferredAppMode
{
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Max
};

// describes the sizes of the menu bar or menu item
typedef union tagUAHMENUITEMMETRICS
{
    // cx appears to be 14 / 0xE less than rcItem's width!
    // cy 0x14 seems stable, i wonder if it is 4 less than rcItem's height which is always 24 atm
    struct {
        DWORD cx;
        DWORD cy;
    } rgsizeBar[2];
    struct {
        DWORD cx;
        DWORD cy;
    } rgsizePopup[4];
} UAHMENUITEMMETRICS;

// not really used in our case but part of the other structures
typedef struct tagUAHMENUPOPUPMETRICS
{
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2; // from kernel symbols, padded to full dword
} UAHMENUPOPUPMETRICS;

// hmenu is the main window menu; hdc is the context to draw in
typedef struct tagUAHMENU
{
    HMENU hmenu;
    HDC hdc;
    DWORD dwFlags; // no idea what these mean, in my testing it's either 0x00000a00 or sometimes 0x00000a10
} UAHMENU;

// menu items are always referred to by iPosition here
typedef struct tagUAHMENUITEM
{
    int iPosition; // 0-based position of menu item in menubar
    UAHMENUITEMMETRICS umim;
    UAHMENUPOPUPMETRICS umpm;
} UAHMENUITEM;

// the DRAWITEMSTRUCT contains the states of the menu items, as well as
// the position index of the item in the menu, which is duplicated in
// the UAHMENUITEM's iPosition as well
typedef struct UAHDRAWMENUITEM
{
    DRAWITEMSTRUCT dis; // itemID looks uninitialized
    UAHMENU um;
    UAHMENUITEM umi;
} UAHDRAWMENUITEM;

// the MEASUREITEMSTRUCT is intended to be filled with the size of the item
// height appears to be ignored, but width can be modified
typedef struct tagUAHMEASUREMENUITEM
{
    MEASUREITEMSTRUCT mis;
    UAHMENU um;
    UAHMENUITEM umi;
} UAHMEASUREMENUITEM;

// theme config struct
typedef struct {
    COLORREF menubar_textcolor;
    COLORREF menubar_textcolor_disabled;
    COLORREF menubar_bgcolor;
    COLORREF menubaritem_bgcolor;
    COLORREF menubaritem_bgcolor_hot;
    COLORREF menubaritem_bgcolor_selected;

    COLORREF dialog_textcolor;
    COLORREF dialog_textcolor_disabled;
    COLORREF dialog_bgcolor;
    COLORREF control_bgcolor;
    COLORREF control_bgcolor_hot;
    COLORREF control_bgcolor_pressed;
    COLORREF control_bordercolor;
    COLORREF progress_bgcolor;
    COLORREF progress_barcolor;
    bool log_unknown_windows;

    HBRUSH menubar_bgbrush;
    HBRUSH menubaritem_bgbrush;
    HBRUSH menubaritem_bgbrush_hot;
    HBRUSH menubaritem_bgbrush_selected;
    HBRUSH dialog_bgbrush;
    HBRUSH control_bgbrush;
    HBRUSH control_bgbrush_hot;
    HBRUSH control_bgbrush_pressed;
    HBRUSH control_borderbrush;
} theme_cfg;

// global variables
static thread_local HTHEME g_menuTheme = nullptr;
static HHOOK g_hook = nullptr;
static HWINEVENTHOOK g_windowEventHook = nullptr;
static HMODULE g_module = nullptr;
static DWORD g_processId = 0;
static UINT g_applyThemeMessage = 0;

static INIT_ONCE g_themeConfigInit = INIT_ONCE_STATIC_INIT;
static theme_cfg g_themeConfig = {};

static COLORREF ParseColor(const inipp::Ini<char>& ini, const char* key, COLORREF fallback) {
    const auto section = ini.sections.find("");
    if (section == ini.sections.end()) return fallback;

    const auto value = section->second.find(key);
    if (value == section->second.end()) return fallback;

    int r = 0;
    int g = 0;
    int b = 0;
    char comma1 = 0;
    char comma2 = 0;
    std::stringstream stream(value->second);
    if (!(stream >> r >> comma1 >> g >> comma2 >> b) || comma1 != ',' || comma2 != ',') {
        return fallback;
    }

    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    return RGB(r, g, b);
}

static bool ParseBool(const inipp::Ini<char>& ini, const char* key, bool fallback) {
    const auto section = ini.sections.find("");
    if (section == ini.sections.end()) return fallback;

    const auto value = section->second.find(key);
    if (value == section->second.end()) return fallback;

    std::string text = value->second;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (text == "true" || text == "yes" || text == "1" || text == "on") return true;
    if (text == "false" || text == "no" || text == "0" || text == "off") return false;
    return fallback;
}

bool IsWndClass(HWND hWnd, const TCHAR* classname) {
    TCHAR buf[512];
    GetClassName(hWnd, buf, 512);
    return _wcsicmp(classname, buf) == 0;
}

bool IsUnityWndClass(HWND hWnd) {
    return IsWndClass(hWnd, L"UnityContainerWndClass");
}

void GetAllWindowsByProcessID(DWORD dwProcessID, std::vector<HWND>& vhWnds) {
    HWND hCurWnd = nullptr;
    do
    {
        hCurWnd = FindWindowEx(nullptr, hCurWnd, nullptr, nullptr);
        if (hCurWnd != nullptr)
        {
            DWORD processID = 0;
            GetWindowThreadProcessId(hCurWnd, &processID);
            if (processID == dwProcessID)
            {
                vhWnds.push_back(hCurWnd);
            }
        }
    } while (hCurWnd != nullptr);
}

static BOOL CALLBACK InitializeThemeConfig(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE hm = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(InitializeThemeConfig),
        &hm);
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(hm, path, MAX_PATH);
    CStringW inifn(path);
    inifn.Append(L".ini");

    if (!std::filesystem::exists(inifn.GetString())) {
        std::ofstream configFile;
        configFile.open(inifn.GetString());
        configFile << "menubar_textcolor = 200,200,200" << std::endl;
        configFile << "menubar_textcolor_disabled = 160,160,160" << std::endl;
        configFile << "menubar_bgcolor = 48,48,48" << std::endl;
        configFile << "menubaritem_bgcolor = 48,48,48" << std::endl;
        configFile << "menubaritem_bgcolor_hot = 62,62,62" << std::endl;
        configFile << "menubaritem_bgcolor_selected = 62,62,62" << std::endl;
        configFile << "dialog_textcolor = 210,210,210" << std::endl;
        configFile << "dialog_textcolor_disabled = 145,145,145" << std::endl;
        configFile << "dialog_bgcolor = 48,48,48" << std::endl;
        configFile << "control_bgcolor = 58,58,58" << std::endl;
        configFile << "control_bgcolor_hot = 72,72,72" << std::endl;
        configFile << "control_bgcolor_pressed = 42,42,42" << std::endl;
        configFile << "control_bordercolor = 96,96,96" << std::endl;
        configFile << "progress_bgcolor = 64,64,64" << std::endl;
        configFile << "progress_barcolor = 58,121,187" << std::endl;
        configFile << "log_unknown_windows = false" << std::endl;
        configFile.close();
    }

    inipp::Ini<char> ini;
    std::ifstream is(inifn.GetString());
    ini.parse(is);
    is.close();

    g_themeConfig.menubar_textcolor = ParseColor(ini, "menubar_textcolor", RGB(200, 200, 200));
    g_themeConfig.menubar_textcolor_disabled = ParseColor(ini, "menubar_textcolor_disabled", RGB(160, 160, 160));
    g_themeConfig.menubar_bgcolor = ParseColor(ini, "menubar_bgcolor", RGB(48, 48, 48));
    g_themeConfig.menubaritem_bgcolor = ParseColor(ini, "menubaritem_bgcolor", RGB(48, 48, 48));
    g_themeConfig.menubaritem_bgcolor_hot = ParseColor(ini, "menubaritem_bgcolor_hot", RGB(62, 62, 62));
    g_themeConfig.menubaritem_bgcolor_selected = ParseColor(ini, "menubaritem_bgcolor_selected", RGB(62, 62, 62));

    g_themeConfig.dialog_textcolor = ParseColor(ini, "dialog_textcolor", RGB(210, 210, 210));
    g_themeConfig.dialog_textcolor_disabled = ParseColor(ini, "dialog_textcolor_disabled", RGB(145, 145, 145));
    g_themeConfig.dialog_bgcolor = ParseColor(ini, "dialog_bgcolor", g_themeConfig.menubar_bgcolor);
    g_themeConfig.control_bgcolor = ParseColor(ini, "control_bgcolor", RGB(58, 58, 58));
    g_themeConfig.control_bgcolor_hot = ParseColor(ini, "control_bgcolor_hot", RGB(72, 72, 72));
    g_themeConfig.control_bgcolor_pressed = ParseColor(ini, "control_bgcolor_pressed", RGB(42, 42, 42));
    g_themeConfig.control_bordercolor = ParseColor(ini, "control_bordercolor", RGB(96, 96, 96));
    g_themeConfig.progress_bgcolor = ParseColor(ini, "progress_bgcolor", RGB(64, 64, 64));
    g_themeConfig.progress_barcolor = ParseColor(ini, "progress_barcolor", RGB(58, 121, 187));
    g_themeConfig.log_unknown_windows = ParseBool(ini, "log_unknown_windows", false);

    g_themeConfig.menubar_bgbrush = CreateSolidBrush(g_themeConfig.menubar_bgcolor);
    g_themeConfig.menubaritem_bgbrush = CreateSolidBrush(g_themeConfig.menubaritem_bgcolor);
    g_themeConfig.menubaritem_bgbrush_hot = CreateSolidBrush(g_themeConfig.menubaritem_bgcolor_hot);
    g_themeConfig.menubaritem_bgbrush_selected = CreateSolidBrush(g_themeConfig.menubaritem_bgcolor_selected);
    g_themeConfig.dialog_bgbrush = CreateSolidBrush(g_themeConfig.dialog_bgcolor);
    g_themeConfig.control_bgbrush = CreateSolidBrush(g_themeConfig.control_bgcolor);
    g_themeConfig.control_bgbrush_hot = CreateSolidBrush(g_themeConfig.control_bgcolor_hot);
    g_themeConfig.control_bgbrush_pressed = CreateSolidBrush(g_themeConfig.control_bgcolor_pressed);
    g_themeConfig.control_borderbrush = CreateSolidBrush(g_themeConfig.control_bordercolor);

    return TRUE;
}

const theme_cfg* LoadThemeConfig() {
    InitOnceExecuteOnce(&g_themeConfigInit, InitializeThemeConfig, nullptr, nullptr);
    return &g_themeConfig;
}

// https://stackoverflow.com/questions/39261826/change-the-color-of-the-title-bar-caption-of-a-win32-application
// https://gist.github.com/rounk-ctrl/b04e5622e30e0d62956870d5c22b7017
// https://github.com/microsoft/WindowsAppSDK/issues/41
// https://gist.github.com/ericoporto/1745f4b912e22f9eabfce2c7166d979b
using fnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND hWnd, BOOL allow);
using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode appMode);

static INIT_ONCE g_darkModeApiInit = INIT_ONCE_STATIC_INIT;
static fnAllowDarkModeForWindow g_allowDarkModeForWindow = nullptr;
static fnSetPreferredAppMode g_setPreferredAppMode = nullptr;
static bool g_ready = false;
static thread_local bool g_applyingControlTheme = false;
static thread_local unsigned int g_hookStage = 0;

static LONG ReportHookException(DWORD exceptionCode, const wchar_t* callbackName) {
    wchar_t message[512] = {};
    swprintf_s(
        message,
        L"[UnityEditorDarkMode] Suppressed exception 0x%08lX in %s (stage 0x%08X).\n",
        exceptionCode,
        callbackName,
        g_hookStage);
    OutputDebugStringW(message);
    fwprintf(stderr, L"%s", message);
    return EXCEPTION_EXECUTE_HANDLER;
}

static BOOL CALLBACK InitializeDarkModeApi(PINIT_ONCE, PVOID, PVOID*) {
    HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
    if (!hUxtheme) {
        hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    if (hUxtheme) {
        // Undocumented APIs available since Windows 10 1903. Resolve dynamically
        // so the plugin remains loadable when a Windows build does not expose them.
        g_allowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133)));
        g_setPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135)));
        if (g_setPreferredAppMode) {
            g_setPreferredAppMode(PreferredAppMode::ForceDark);
        }
    }

    return TRUE;
}

void EnableDarkMode(HWND hWnd) {
    InitOnceExecuteOnce(&g_darkModeApiInit, InitializeDarkModeApi, nullptr, nullptr);

    if (!hWnd) return;

    if (g_allowDarkModeForWindow) {
        g_allowDarkModeForWindow(hWnd, TRUE);
    }

    const BOOL useDarkMode = TRUE;
    HRESULT result = DwmSetWindowAttribute(
        hWnd,
        static_cast<DWMWINDOWATTRIBUTE>(20), // DWMWA_USE_IMMERSIVE_DARK_MODE on current Windows SDKs
        &useDarkMode,
        sizeof(useDarkMode));
    if (FAILED(result)) {
        DwmSetWindowAttribute(
            hWnd,
            static_cast<DWMWINDOWATTRIBUTE>(19), // Windows 10 1809 compatibility value
            &useDarkMode,
            sizeof(useDarkMode));
    }
}

static std::wstring GetWindowClassNameString(HWND hWnd) {
    wchar_t className[256] = {};
    GetClassNameW(hWnd, className, static_cast<int>(_countof(className)));
    return className;
}

static bool IsClassPrefix(HWND hWnd, const wchar_t* prefix) {
    const std::wstring className = GetWindowClassNameString(hWnd);
    const size_t prefixLength = wcslen(prefix);
    return className.length() >= prefixLength &&
        _wcsnicmp(className.c_str(), prefix, prefixLength) == 0;
}

static bool IsCurrentProcessWindow(HWND hWnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hWnd, &processId);
    return processId == g_processId;
}

static bool IsTopLevelWindow(HWND hWnd) {
    return (GetWindowLongPtrW(hWnd, GWL_STYLE) & WS_CHILD) == 0;
}

static bool IsKnownContainerClass(HWND hWnd) {
    return IsUnityWndClass(hWnd) ||
        IsWndClass(hWnd, L"#32770") ||
        IsWndClass(hWnd, L"NativeHWNDHost");
}

static bool IsKnownControlClass(HWND hWnd) {
    return IsWndClass(hWnd, L"Button") ||
        IsWndClass(hWnd, L"Static") ||
        IsWndClass(hWnd, L"Edit") ||
        IsWndClass(hWnd, L"ScrollBar") ||
        IsWndClass(hWnd, L"ComboBox") ||
        IsWndClass(hWnd, L"ComboBoxEx32") ||
        IsWndClass(hWnd, L"ComboLBox") ||
        IsWndClass(hWnd, L"ListBox") ||
        IsWndClass(hWnd, L"SysListView32") ||
        IsWndClass(hWnd, L"SysTreeView32") ||
        IsWndClass(hWnd, L"SysTabControl32") ||
        IsWndClass(hWnd, L"SysHeader32") ||
        IsWndClass(hWnd, L"SysLink") ||
        IsWndClass(hWnd, L"SysDateTimePick32") ||
        IsWndClass(hWnd, L"SysMonthCal32") ||
        IsWndClass(hWnd, L"ToolbarWindow32") ||
        IsWndClass(hWnd, L"ReBarWindow32") ||
        IsWndClass(hWnd, L"msctls_progress32") ||
        IsWndClass(hWnd, L"msctls_trackbar32") ||
        IsWndClass(hWnd, L"msctls_updown32") ||
        IsWndClass(hWnd, L"msctls_hotkey32") ||
        IsWndClass(hWnd, L"msctls_statusbar32") ||
        IsWndClass(hWnd, L"tooltips_class32") ||
        IsWndClass(hWnd, L"DirectUIHWND") ||
        IsWndClass(hWnd, L"DUIViewWndClassName") ||
        IsWndClass(hWnd, L"SHELLDLL_DefView") ||
        IsClassPrefix(hWnd, L"RichEdit") ||
        IsClassPrefix(hWnd, L"RICHEDIT");
}

static bool ShouldThemeWindow(HWND hWnd) {
    return IsTopLevelWindow(hWnd) || IsKnownContainerClass(hWnd) || IsKnownControlClass(hWnd);
}

static void LogUnknownWindow(HWND hWnd) {
    if (!g_ready || !LoadThemeConfig()->log_unknown_windows || IsTopLevelWindow(hWnd)) return;

    const std::wstring className = GetWindowClassNameString(hWnd);
    wchar_t message[512] = {};
    swprintf_s(
        message,
        L"[UnityEditorDarkMode] Unhandled Unity child window class: %s\n",
        className.c_str());
    OutputDebugStringW(message);
}

LRESULT CALLBACK CallWndSubClassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
static void ThemeWindowTree(HWND hWnd);

static void RefreshControlColors(HWND hWnd) {
    const theme_cfg* theme = LoadThemeConfig();

    if (IsWndClass(hWnd, L"msctls_progress32")) {
        SendMessageW(hWnd, PBM_SETBKCOLOR, 0, theme->progress_bgcolor);
        SendMessageW(hWnd, PBM_SETBARCOLOR, 0, theme->progress_barcolor);
    }
    else if (IsWndClass(hWnd, L"tooltips_class32")) {
        SendMessageW(hWnd, TTM_SETTIPBKCOLOR, theme->control_bgcolor, 0);
        SendMessageW(hWnd, TTM_SETTIPTEXTCOLOR, theme->dialog_textcolor, 0);
    }
    else if (IsWndClass(hWnd, L"SysTreeView32")) {
        TreeView_SetBkColor(hWnd, theme->control_bgcolor);
        TreeView_SetTextColor(hWnd, theme->dialog_textcolor);
    }
    else if (IsWndClass(hWnd, L"SysListView32")) {
        ListView_SetBkColor(hWnd, theme->control_bgcolor);
        ListView_SetTextBkColor(hWnd, theme->control_bgcolor);
        ListView_SetTextColor(hWnd, theme->dialog_textcolor);
    }
}

static void ApplyControlTheme(HWND hWnd) {
    if (g_applyingControlTheme) return;
    g_applyingControlTheme = true;

    g_hookStage = 0x20;
    EnableDarkMode(hWnd);

    g_hookStage = 0x21;
    if (IsWndClass(hWnd, L"msctls_progress32")) {
        // Native progress bars ignore PBM_SET*COLOR while visual styles are active.
        SetWindowTheme(hWnd, L"", L"");
    }
    else if (IsWndClass(hWnd, L"tooltips_class32")) {
        SetWindowTheme(hWnd, L"", L"");
    }
    else if (IsWndClass(hWnd, L"SysTreeView32")) {
        SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);
    }
    else if (IsWndClass(hWnd, L"SysListView32")) {
        SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);
    }
    else if (IsWndClass(hWnd, L"Button")) {
        const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        const LONG_PTR buttonType = style & BS_TYPEMASK;
        if (buttonType == BS_PUSHBUTTON ||
            buttonType == BS_DEFPUSHBUTTON ||
            buttonType == BS_OWNERDRAW) {
            if (buttonType != BS_OWNERDRAW) {
                SetWindowLongPtrW(hWnd, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW);
            }
            SetWindowTheme(hWnd, L"", L"");
        }
        else {
            // Windows 10's themed check/radio renderer can keep black text even
            // after dark mode is enabled. Classic rendering honors the parent's
            // WM_CTLCOLORBTN colors while retaining native input behavior.
            SetWindowTheme(hWnd, L"", L"");
        }
    }
    else if (IsWndClass(hWnd, L"ComboBox") || IsWndClass(hWnd, L"ComboBoxEx32")) {
        // Classic combo rendering honors WM_CTLCOLOREDIT / LISTBOX / STATIC.
        SetWindowTheme(hWnd, L"", L"");
    }
    else if (IsKnownControlClass(hWnd) || IsKnownContainerClass(hWnd) || IsTopLevelWindow(hWnd)) {
        SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);
    }

    g_hookStage = 0x22;
    RefreshControlColors(hWnd);
    g_applyingControlTheme = false;
}

static void ThemeWindow(HWND hWnd) {
    g_hookStage = 0x10;
    if (!hWnd || !IsWindow(hWnd) || !IsCurrentProcessWindow(hWnd)) return;

    g_hookStage = 0x11;
    if (!ShouldThemeWindow(hWnd)) {
        LogUnknownWindow(hWnd);
        return;
    }

    g_hookStage = 0x12;
    ApplyControlTheme(hWnd);
    g_hookStage = 0x13;
    SetWindowSubclass(hWnd, CallWndSubClassProc, 0, 0);
    if (IsWindowVisible(hWnd)) {
        g_hookStage = 0x14;
        RedrawWindow(
            hWnd,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

static void AttachWindowSubclass(HWND hWnd) {
    if (!hWnd || !IsCurrentProcessWindow(hWnd) || !ShouldThemeWindow(hWnd)) return;

    // HCBT_CREATEWND runs while the native control is only partially created.
    // Attaching the subclass here is safe, but applying visual styles, sending
    // control messages, or redrawing is not. EVENT_OBJECT_SHOW performs the
    // full theme pass after creation has progressed.
    EnableDarkMode(hWnd);
    SetWindowSubclass(hWnd, CallWndSubClassProc, 0, 0);
}

static BOOL CALLBACK ThemeChildWindow(HWND hWnd, LPARAM) {
    __try {
        ThemeWindow(hWnd);
    }
    __except (ReportHookException(GetExceptionCode(), L"ThemeChildWindow")) {
    }
    return TRUE;
}

static void ThemeWindowTree(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return;
    g_hookStage = 0x30;
    ThemeWindow(hWnd);
    g_hookStage = 0x31;
    EnumChildWindows(hWnd, ThemeChildWindow, 0);
}

static LRESULT ApplyThemeCallWndProcImpl(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        const CWPSTRUCT* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message && message->message == g_applyThemeMessage) {
            ThemeWindowTree(message->hwnd);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static LRESULT CALLBACK ApplyThemeCallWndProc(int nCode, WPARAM wParam, LPARAM lParam) {
    __try {
        g_hookStage = 0x20000000 | static_cast<unsigned int>(nCode & 0xffff);
        return ApplyThemeCallWndProcImpl(nCode, wParam, lParam);
    }
    __except (ReportHookException(GetExceptionCode(), L"ApplyThemeCallWndProc")) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
}

static void ThemeWindowOnOwningThread(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !IsCurrentProcessWindow(hWnd)) return;

    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(hWnd, &processId);
    if (processId != g_processId) return;

    if (threadId == GetCurrentThreadId()) {
        ThemeWindowTree(hWnd);
        return;
    }

    HHOOK callHook = SetWindowsHookExW(WH_CALLWNDPROC, ApplyThemeCallWndProc, g_module, threadId);
    if (!callHook) {
        EnableDarkMode(hWnd);
        return;
    }

    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(
        hWnd,
        g_applyThemeMessage,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        200,
        &ignored);
    UnhookWindowsHookEx(callHook);
}

static void WindowEventProcImpl(
    HWINEVENTHOOK,
    DWORD event,
    HWND hWnd,
    LONG idObject,
    LONG idChild,
    DWORD,
    DWORD) {
    if (event != EVENT_OBJECT_SHOW ||
        !hWnd ||
        idObject != OBJID_WINDOW ||
        idChild != CHILDID_SELF ||
        !IsCurrentProcessWindow(hWnd)) {
        return;
    }

    HWND root = GetAncestor(hWnd, GA_ROOT);
    ThemeWindowOnOwningThread(root ? root : hWnd);
}

static void CALLBACK WindowEventProc(
    HWINEVENTHOOK hook,
    DWORD event,
    HWND hWnd,
    LONG idObject,
    LONG idChild,
    DWORD eventThread,
    DWORD eventTime) {
    __try {
        g_hookStage = 0x30000000 | (event & 0xffff);
        WindowEventProcImpl(hook, event, hWnd, idObject, idChild, eventThread, eventTime);
    }
    __except (ReportHookException(GetExceptionCode(), L"WindowEventProc")) {
    }
}

static LRESULT CBTProcImpl(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HCBT_CREATEWND) {
        AttachWindowSubclass(reinterpret_cast<HWND>(wParam));
    }
    else if (nCode == HCBT_DESTROYWND) {
        RemoveWindowSubclass(reinterpret_cast<HWND>(wParam), CallWndSubClassProc, 0);
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam) {
    __try {
        g_hookStage = 0x40000000 | static_cast<unsigned int>(nCode & 0xffff);
        return CBTProcImpl(nCode, wParam, lParam);
    }
    __except (ReportHookException(GetExceptionCode(), L"CBTProc")) {
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }
}

void UAHDrawMenuNCBottomLine(HWND hWnd) {
    MENUBARINFO mbi = { sizeof(mbi) };

    if (!GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi))
    {
        return;
    }

    RECT rcClient = { 0 };
    GetClientRect(hWnd, &rcClient);
    MapWindowPoints(hWnd, nullptr, (POINT*)&rcClient, 2);

    RECT rcWindow = { 0 };
    GetWindowRect(hWnd, &rcWindow);
    OffsetRect(&rcClient, -rcWindow.left, -rcWindow.top);
    // the rcBar is offset by the window rect
    RECT rcAnnoyingLine = rcClient;
    rcAnnoyingLine.bottom = rcAnnoyingLine.top;
    rcAnnoyingLine.top--;

    HDC hdc = GetWindowDC(hWnd);
    FillRect(hdc, &rcAnnoyingLine, LoadThemeConfig()->menubar_bgbrush);
    ReleaseDC(hWnd, hdc);
}

void PaintOwnerDrawButton(const DRAWITEMSTRUCT& dis) {
    const theme_cfg* theme = LoadThemeConfig();
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;
    const bool hot = (dis.itemState & ODS_HOTLIGHT) != 0;

    HBRUSH background = theme->control_bgbrush;
    if (pressed) background = theme->control_bgbrush_pressed;
    else if (hot) background = theme->control_bgbrush_hot;

    RECT bounds = dis.rcItem;
    FillRect(dis.hDC, &bounds, background);
    FrameRect(dis.hDC, &bounds, theme->control_borderbrush);

    if ((dis.itemState & ODS_DEFAULT) != 0) {
        RECT innerBorder = bounds;
        InflateRect(&innerBorder, -1, -1);
        FrameRect(dis.hDC, &innerBorder, theme->control_borderbrush);
    }

    wchar_t text[512] = {};
    GetWindowTextW(dis.hwndItem, text, static_cast<int>(_countof(text)));

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(dis.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dis.hDC, font) : nullptr;
    SetBkMode(dis.hDC, TRANSPARENT);
    SetTextColor(
        dis.hDC,
        disabled ? theme->dialog_textcolor_disabled : theme->dialog_textcolor);

    RECT textBounds = bounds;
    if (pressed) OffsetRect(&textBounds, 1, 1);
    DrawTextW(
        dis.hDC,
        text,
        -1,
        &textBounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if ((dis.itemState & ODS_FOCUS) != 0) {
        RECT focusBounds = bounds;
        InflateRect(&focusBounds, -3, -3);
        DrawFocusRect(dis.hDC, &focusBounds);
    }

    if (oldFont) SelectObject(dis.hDC, oldFont);
}

static void PaintTabControl(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    const theme_cfg* theme = LoadThemeConfig();
    PAINTSTRUCT paint = {};
    HDC deviceContext = suppliedDeviceContext
        ? suppliedDeviceContext
        : BeginPaint(hWnd, &paint);
    if (!deviceContext) return;

    RECT clientBounds = {};
    GetClientRect(hWnd, &clientBounds);
    FillRect(deviceContext, &clientBounds, theme->control_bgbrush);

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(deviceContext, font) : nullptr;
    SetBkMode(deviceContext, TRANSPARENT);

    const int itemCount = TabCtrl_GetItemCount(hWnd);
    const int selectedItem = TabCtrl_GetCurSel(hWnd);
    for (int index = 0; index < itemCount; ++index) {
        RECT itemBounds = {};
        if (!TabCtrl_GetItemRect(hWnd, index, &itemBounds)) continue;

        FillRect(
            deviceContext,
            &itemBounds,
            index == selectedItem
                ? theme->control_bgbrush_hot
                : theme->control_bgbrush);
        FrameRect(deviceContext, &itemBounds, theme->control_borderbrush);

        wchar_t text[256] = {};
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = static_cast<int>(_countof(text));
        if (TabCtrl_GetItem(hWnd, index, &item)) {
            SetTextColor(
                deviceContext,
                IsWindowEnabled(hWnd)
                    ? theme->dialog_textcolor
                    : theme->dialog_textcolor_disabled);
            DrawTextW(
                deviceContext,
                text,
                -1,
                &itemBounds,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    if (oldFont) SelectObject(deviceContext, oldFont);
    if (!suppliedDeviceContext) EndPaint(hWnd, &paint);
}

static bool IsDropDownListCombo(HWND hWnd) {
    return IsWndClass(hWnd, L"ComboBox") &&
        (GetWindowLongPtrW(hWnd, GWL_STYLE) & 0x3) == CBS_DROPDOWNLIST;
}

static void PaintDropDownListCombo(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    const theme_cfg* theme = LoadThemeConfig();
    PAINTSTRUCT paint = {};
    HDC deviceContext = suppliedDeviceContext
        ? suppliedDeviceContext
        : BeginPaint(hWnd, &paint);
    if (!deviceContext) return;

    RECT bounds = {};
    GetClientRect(hWnd, &bounds);
    FillRect(deviceContext, &bounds, theme->control_bgbrush);
    FrameRect(deviceContext, &bounds, theme->control_borderbrush);

    const int arrowWidth = std::max(18, GetSystemMetrics(SM_CXVSCROLL));
    RECT arrowBounds = bounds;
    arrowBounds.left = std::max(bounds.left, bounds.right - arrowWidth);
    InflateRect(&arrowBounds, -1, -1);
    FillRect(deviceContext, &arrowBounds, theme->control_bgbrush_hot);

    RECT separator = arrowBounds;
    separator.right = separator.left + 1;
    FillRect(deviceContext, &separator, theme->control_borderbrush);

    const COLORREF textColor = IsWindowEnabled(hWnd)
        ? theme->dialog_textcolor
        : theme->dialog_textcolor_disabled;
    const int centerX = (arrowBounds.left + arrowBounds.right) / 2;
    const int centerY = (arrowBounds.top + arrowBounds.bottom) / 2;
    POINT arrow[] = {
        { centerX - 4, centerY - 2 },
        { centerX + 4, centerY - 2 },
        { centerX, centerY + 3 }
    };
    HBRUSH arrowBrush = CreateSolidBrush(textColor);
    HGDIOBJ oldBrush = SelectObject(deviceContext, arrowBrush);
    HGDIOBJ oldPen = SelectObject(deviceContext, GetStockObject(NULL_PEN));
    Polygon(deviceContext, arrow, static_cast<int>(_countof(arrow)));
    SelectObject(deviceContext, oldPen);
    SelectObject(deviceContext, oldBrush);
    DeleteObject(arrowBrush);

    wchar_t text[512] = {};
    GetWindowTextW(hWnd, text, static_cast<int>(_countof(text)));

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(deviceContext, font) : nullptr;
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, textColor);
    RECT textBounds = bounds;
    textBounds.left += 6;
    textBounds.right = arrowBounds.left - 4;
    DrawTextW(
        deviceContext,
        text,
        -1,
        &textBounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(deviceContext, oldFont);

    if (GetFocus() == hWnd) {
        RECT focusBounds = bounds;
        focusBounds.right = arrowBounds.left;
        InflateRect(&focusBounds, -3, -3);
        DrawFocusRect(deviceContext, &focusBounds);
    }

    if (!suppliedDeviceContext) EndPaint(hWnd, &paint);
}

static LRESULT CallWndSubClassProcImpl(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass) {
    switch (uMsg) {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC:
        {
            const theme_cfg* theme = LoadThemeConfig();
            HDC deviceContext = reinterpret_cast<HDC>(wParam);
            HWND child = reinterpret_cast<HWND>(lParam);
            SetTextColor(
                deviceContext,
                child && !IsWindowEnabled(child)
                    ? theme->dialog_textcolor_disabled
                    : theme->dialog_textcolor);
            SetBkColor(deviceContext, theme->dialog_bgcolor);
            SetBkMode(deviceContext, TRANSPARENT);
            return reinterpret_cast<LRESULT>(theme->dialog_bgbrush);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSCROLLBAR:
        {
            const theme_cfg* theme = LoadThemeConfig();
            HDC deviceContext = reinterpret_cast<HDC>(wParam);
            HWND child = reinterpret_cast<HWND>(lParam);
            SetTextColor(
                deviceContext,
                child && !IsWindowEnabled(child)
                    ? theme->dialog_textcolor_disabled
                    : theme->dialog_textcolor);
            SetBkColor(deviceContext, theme->control_bgcolor);
            SetBkMode(deviceContext, OPAQUE);
            return reinterpret_cast<LRESULT>(theme->control_bgbrush);
        }
        case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item && item->CtlType == ODT_BUTTON) {
                PaintOwnerDrawButton(*item);
                return TRUE;
            }
            break;
        }
        case WM_ERASEBKGND:
        {
            if (IsWndClass(hWnd, L"SysTabControl32") || IsDropDownListCombo(hWnd)) {
                RECT bounds = {};
                GetClientRect(hWnd, &bounds);
                FillRect(
                    reinterpret_cast<HDC>(wParam),
                    &bounds,
                    LoadThemeConfig()->control_bgbrush);
                return TRUE;
            }
            if (IsTopLevelWindow(hWnd) || IsKnownContainerClass(hWnd)) {
                RECT bounds = {};
                GetClientRect(hWnd, &bounds);
                FillRect(
                    reinterpret_cast<HDC>(wParam),
                    &bounds,
                    LoadThemeConfig()->dialog_bgbrush);
                return TRUE;
            }
            break;
        }
        case WM_NCACTIVATE:
        {
            if (IsTopLevelWindow(hWnd) || IsKnownContainerClass(hWnd)) {
                LRESULT lr = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                UAHDrawMenuNCBottomLine(hWnd);
                return lr;
            }
            break;
        }
        case WM_INITDIALOG:
        {
            ThemeWindowTree(hWnd);
            break;
        }
        case WM_PARENTNOTIFY:
        {
            if (LOWORD(wParam) == WM_CREATE) {
                ThemeWindow(reinterpret_cast<HWND>(lParam));
            }
            break;
        }
        case WM_SHOWWINDOW:
        {
            if (wParam) {
                EnableDarkMode(hWnd);
                EnumChildWindows(hWnd, ThemeChildWindow, 0);
            }
            break;
        }
        case WM_NCPAINT:
        {
            if (IsTopLevelWindow(hWnd) || IsKnownContainerClass(hWnd)) {
                LRESULT lr = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                UAHDrawMenuNCBottomLine(hWnd);
                return lr;
            }
            break;
        }
        case WM_PAINT:
        {
            if (IsWndClass(hWnd, L"SysTabControl32")) {
                PaintTabControl(hWnd);
                return 0;
            }
            if (IsDropDownListCombo(hWnd)) {
                PaintDropDownListCombo(hWnd);
                return 0;
            }
            // Some common controls reset their palette after internal theme changes.
            if (IsWndClass(hWnd, L"msctls_progress32") ||
                IsWndClass(hWnd, L"tooltips_class32") ||
                IsWndClass(hWnd, L"SysTreeView32") ||
                IsWndClass(hWnd, L"SysListView32")) {
                RefreshControlColors(hWnd);
            }
            break;
        }
        case WM_PRINTCLIENT:
        {
            if (IsWndClass(hWnd, L"SysTabControl32")) {
                PaintTabControl(hWnd, reinterpret_cast<HDC>(wParam));
                return 0;
            }
            if (IsDropDownListCombo(hWnd)) {
                PaintDropDownListCombo(hWnd, reinterpret_cast<HDC>(wParam));
                return 0;
            }
            break;
        }
        case WM_STYLECHANGING:
        case WM_STYLECHANGED:
        {
            if (IsUnityWndClass(hWnd)) {
                // prevent propagation to prevent menu bar from going back to the standard one...?!? FIXME
                return true;
            }
            break;
        }
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        {
            if (g_menuTheme) {
                CloseThemeData(g_menuTheme);
                g_menuTheme = nullptr;
            }
            ApplyControlTheme(hWnd);
            break;
        }
        case WM_NCDESTROY:
        {
            RemoveWindowSubclass(hWnd, CallWndSubClassProc, uIdSubclass);
            break;
        }
        // https://stackoverflow.com/questions/77985210/how-to-set-menu-bar-color-in-win32
        case WM_UAHDRAWMENU:
        {
            if (IsTopLevelWindow(hWnd) || IsKnownContainerClass(hWnd)) {
                UAHMENU* pUDM = (UAHMENU*)lParam;
                RECT rc = { 0 };
                {
                    MENUBARINFO mbi = { sizeof(mbi) };
                    GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi);

                    RECT rcWindow;
                    GetWindowRect(hWnd, &rcWindow);
                    // the rcBar is offset by the window rect
                    rc = mbi.rcBar;
                    OffsetRect(&rc, -rcWindow.left, -rcWindow.top);
                }
                FillRect(pUDM->hdc, &rc, LoadThemeConfig()->menubar_bgbrush);
                return true;
            }
            break;
        }
        case WM_UAHDRAWMENUITEM:
        {
            if (IsTopLevelWindow(hWnd) || IsKnownContainerClass(hWnd)) {
                UAHDRAWMENUITEM* pUDMI = (UAHDRAWMENUITEM*)lParam;

                const HBRUSH* pbrBackground = &LoadThemeConfig()->menubaritem_bgbrush;
                // get the menu item string
                wchar_t menuString[256] = { 0 };
                MENUITEMINFO mii = { sizeof(mii), MIIM_STRING };
                {
                    mii.dwTypeData = menuString;
                    mii.cch = (sizeof(menuString) / 2) - 1;

                    GetMenuItemInfo(pUDMI->um.hmenu, pUDMI->umi.iPosition, TRUE, &mii);
                }
                // get the item state for drawing
                DWORD dwFlags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
                int iTextStateID = 0;

                if ((pUDMI->dis.itemState & ODS_INACTIVE) | (pUDMI->dis.itemState & ODS_DEFAULT)) {
                    // normal display
                    iTextStateID = MPI_NORMAL;
                }
                if (pUDMI->dis.itemState & ODS_HOTLIGHT) {
                    // hot tracking
                    iTextStateID = MPI_HOT;
                    pbrBackground = &LoadThemeConfig()->menubaritem_bgbrush_hot;
                }
                if (pUDMI->dis.itemState & ODS_SELECTED) {
                    // clicked -- MENU_POPUPITEM has no state for this, though MENU_BARITEM does
                    iTextStateID = MPI_HOT;
                    pbrBackground = &LoadThemeConfig()->menubaritem_bgbrush_selected;
                }
                if ((pUDMI->dis.itemState & ODS_GRAYED) || (pUDMI->dis.itemState & ODS_DISABLED)) {
                    // disabled / grey text
                    iTextStateID = MPI_DISABLED;
                }
                if (pUDMI->dis.itemState & ODS_NOACCEL) {
                    dwFlags |= DT_HIDEPREFIX;
                }

                if (!g_menuTheme) {
                    g_menuTheme = OpenThemeData(hWnd, L"Menu");
                }

                DTTOPTS opts = { sizeof(opts), DTT_TEXTCOLOR, iTextStateID != MPI_DISABLED ? LoadThemeConfig()->menubar_textcolor : LoadThemeConfig()->menubar_textcolor_disabled };
                FillRect(pUDMI->um.hdc, &pUDMI->dis.rcItem, *pbrBackground);
                DrawThemeTextEx(g_menuTheme, pUDMI->um.hdc, MENU_BARITEM, MBI_NORMAL, menuString, mii.cch, dwFlags, &pUDMI->dis.rcItem, &opts);
                return true;
            }
            break;
        }
        case WM_UAHMEASUREMENUITEM:
        {
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }
        default:
            break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK CallWndSubClassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR) {
    __try {
        g_hookStage = 0x50000000 | (uMsg & 0xffff);
        return CallWndSubClassProcImpl(hWnd, uMsg, wParam, lParam, uIdSubclass);
    }
    __except (ReportHookException(GetExceptionCode(), L"CallWndSubClassProc")) {
        // Do not retry DefSubclassProc here: the original exception may have
        // come from the downstream control procedure itself.
        return 0;
    }
}

static BOOL CALLBACK AttachExistingChildWindow(HWND hWnd, LPARAM) {
    EnableDarkMode(hWnd);
    if (ShouldThemeWindow(hWnd) && GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId()) {
        SetWindowSubclass(hWnd, CallWndSubClassProc, 0, 0);
    }
    return TRUE;
}

static void AttachExistingWindow(HWND hWnd) {
    if (!IsCurrentProcessWindow(hWnd)) return;

    EnableDarkMode(hWnd);
    if (GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId()) {
        SetWindowSubclass(hWnd, CallWndSubClassProc, 0, 0);
        EnumChildWindows(hWnd, AttachExistingChildWindow, 0);
    }
}

static BOOL CALLBACK RemoveChildSubclass(HWND hWnd, LPARAM) {
    RemoveWindowSubclass(hWnd, CallWndSubClassProc, 0);
    return TRUE;
}

// DLL entry
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason)
    {
        case DLL_PROCESS_ATTACH: {
            g_module = hModule;
            g_processId = GetCurrentProcessId();
            DisableThreadLibraryCalls(hModule);
            g_applyThemeMessage = RegisterWindowMessageW(L"UnityEditorDarkMode.ApplyTheme");

            // Enable process-wide menu dark mode before attaching window hooks.
            EnableDarkMode(nullptr);

            // Catch windows created before Unity loaded this preloaded native plugin.
            std::vector<HWND> windowHandles;
            GetAllWindowsByProcessID(g_processId, windowHandles);

            for (const HWND& hWnd : windowHandles) {
                AttachExistingWindow(hWnd);
            }

            // CBT gives synchronous coverage for Unity's main GUI thread. The
            // process-scoped WinEvent hook supplements it for dialogs created by
            // other Unity-owned UI threads and themes them on their owning thread.
            g_hook = SetWindowsHookExW(WH_CBT, CBTProc, nullptr, GetCurrentThreadId());
            g_windowEventHook = SetWinEventHook(
                EVENT_OBJECT_SHOW,
                EVENT_OBJECT_SHOW,
                nullptr,
                WindowEventProc,
                g_processId,
                0,
                WINEVENT_OUTOFCONTEXT);

            g_ready = true;
            break;
        }
        case DLL_PROCESS_DETACH: {
            g_ready = false;

            if (g_windowEventHook) {
                UnhookWinEvent(g_windowEventHook);
                g_windowEventHook = nullptr;
            }
            if (g_hook) {
                UnhookWindowsHookEx(g_hook);
                g_hook = nullptr;
            }

            // When FreeLibrary unloads the plugin before process exit, remove
            // callbacks that would otherwise point into the unloaded module.
            if (!reserved) {
                std::vector<HWND> windowHandles;
                GetAllWindowsByProcessID(g_processId, windowHandles);
                for (const HWND& hWnd : windowHandles) {
                    if (GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId()) {
                        EnumChildWindows(hWnd, RemoveChildSubclass, 0);
                        RemoveWindowSubclass(hWnd, CallWndSubClassProc, 0);
                    }
                }
            }

            if (g_menuTheme) {
                CloseThemeData(g_menuTheme);
                g_menuTheme = nullptr;
            }
            break;
        }
        default: break;
    }

    return TRUE;
}
