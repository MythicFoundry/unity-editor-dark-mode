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
#include <atomic>
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
static DWORD g_hookThreadId = 0;
static HWINEVENTHOOK g_windowEventHook = nullptr;
static HMODULE g_module = nullptr;
static DWORD g_processId = 0;
static UINT g_applyThemeMessage = 0;
static UINT g_removeThemeMessage = 0;
static SRWLOCK g_lifecycleLock = SRWLOCK_INIT;
static bool g_modulePinned = false;

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
using fnRefreshImmersiveColorPolicyState = void(WINAPI*)();
using fnFlushMenuThemes = void(WINAPI*)();

static INIT_ONCE g_darkModeApiInit = INIT_ONCE_STATIC_INIT;
static fnAllowDarkModeForWindow g_allowDarkModeForWindow = nullptr;
static fnSetPreferredAppMode g_setPreferredAppMode = nullptr;
static fnRefreshImmersiveColorPolicyState g_refreshImmersiveColorPolicyState = nullptr;
static fnFlushMenuThemes g_flushMenuThemes = nullptr;
static std::atomic_bool g_ready = false;
static thread_local bool g_applyingControlTheme = false;
static thread_local bool g_refreshingThemeState = false;
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

static void ReportWin32Failure(const wchar_t* operation, DWORD errorCode) {
    wchar_t message[512] = {};
    swprintf_s(
        message,
        L"[UnityEditorDarkMode] %s failed with Win32 error %lu.\n",
        operation,
        errorCode);
    OutputDebugStringW(message);
    fwprintf(stderr, L"%s", message);
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
        g_refreshImmersiveColorPolicyState = reinterpret_cast<fnRefreshImmersiveColorPolicyState>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(104)));
        g_flushMenuThemes = reinterpret_cast<fnFlushMenuThemes>(
            GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136)));
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

static void RefreshDarkModeState() {
    InitOnceExecuteOnce(&g_darkModeApiInit, InitializeDarkModeApi, nullptr, nullptr);

    if (g_refreshImmersiveColorPolicyState) {
        g_refreshImmersiveColorPolicyState();
    }
    if (g_setPreferredAppMode) {
        g_setPreferredAppMode(PreferredAppMode::ForceDark);
    }
    if (g_flushMenuThemes) {
        g_flushMenuThemes();
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

static bool IsPopupMenuWindow(HWND hWnd) {
    // Popup menus already have a dedicated dark-mode path in this plugin.
    // Letting the general top-level-window hook subclass #32768 changes their
    // established Unity appearance and can interfere with native menu drawing.
    return IsWndClass(hWnd, L"#32768");
}

static bool IsKnownContainerClass(HWND hWnd) {
    return IsUnityWndClass(hWnd) ||
        IsWndClass(hWnd, L"#32770") ||
        IsWndClass(hWnd, L"NativeHWNDHost") ||
        IsWndClass(hWnd, L"WorkerW");
}

static bool HasAncestorClass(HWND hWnd, const wchar_t* className) {
    HWND ancestor = GetParent(hWnd);
    while (ancestor) {
        if (IsWndClass(ancestor, className)) return true;
        ancestor = GetParent(ancestor);
    }
    return false;
}

static bool IsFileDialogItemsView(HWND hWnd) {
    return IsWndClass(hWnd, L"SHELLDLL_DefView") ||
        (IsWndClass(hWnd, L"DirectUIHWND") &&
            HasAncestorClass(hWnd, L"SHELLDLL_DefView"));
}

static constexpr wchar_t kCommonFileDialogProperty[] =
    L"MythicFoundry.UnityEditorDarkMode.CommonFileDialog";

static BOOL CALLBACK FindShellViewWindow(HWND hWnd, LPARAM parameter) {
    if (!IsWndClass(hWnd, L"SHELLDLL_DefView")) return TRUE;

    *reinterpret_cast<HWND*>(parameter) = hWnd;
    return FALSE;
}

static bool ContainsShellView(HWND hWnd) {
    HWND shellView = nullptr;
    EnumChildWindows(hWnd, FindShellViewWindow, reinterpret_cast<LPARAM>(&shellView));
    return shellView != nullptr;
}

static void MarkCommonFileDialog(HWND hWnd) {
    HWND root = GetAncestor(hWnd, GA_ROOT);
    if (!root) root = hWnd;
    if (!IsWndClass(root, L"#32770") || GetPropW(root, kCommonFileDialogProperty)) return;

    if (ContainsShellView(root)) {
        SetPropW(root, kCommonFileDialogProperty, reinterpret_cast<HANDLE>(1));
    }
}

static bool IsCommonFileDialogWindow(HWND hWnd) {
    HWND root = GetAncestor(hWnd, GA_ROOT);
    if (!root) root = hWnd;
    return GetPropW(root, kCommonFileDialogProperty) != nullptr;
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
        IsWndClass(hWnd, L"TravelBand") ||
        IsWndClass(hWnd, L"UpBand") ||
        IsWndClass(hWnd, L"Address Band Root") ||
        IsWndClass(hWnd, L"Breadcrumb Parent") ||
        IsWndClass(hWnd, L"UniversalSearchBand") ||
        IsWndClass(hWnd, L"SearchEditBoxWrapperClass") ||
        IsWndClass(hWnd, L"SeparatorBand") ||
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
    return !IsPopupMenuWindow(hWnd) &&
        (IsTopLevelWindow(hWnd) ||
            IsKnownContainerClass(hWnd) ||
            IsKnownControlClass(hWnd) ||
            IsCommonFileDialogWindow(hWnd));
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
static void RemoveWindowTree(HWND hWnd);

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

static LONG_PTR GetButtonType(HWND hWnd) {
    return GetWindowLongPtrW(hWnd, GWL_STYLE) & BS_TYPEMASK;
}

static bool IsCheckOrRadioButton(HWND hWnd) {
    if (!IsWndClass(hWnd, L"Button")) return false;

    const LONG_PTR buttonType = GetButtonType(hWnd);
    return buttonType == BS_CHECKBOX ||
        buttonType == BS_AUTOCHECKBOX ||
        buttonType == BS_3STATE ||
        buttonType == BS_AUTO3STATE ||
        buttonType == BS_RADIOBUTTON ||
        buttonType == BS_AUTORADIOBUTTON;
}

static bool IsGroupBox(HWND hWnd) {
    return IsWndClass(hWnd, L"Button") && GetButtonType(hWnd) == BS_GROUPBOX;
}

static bool IsTextStatic(HWND hWnd) {
    if (!IsWndClass(hWnd, L"Static")) return false;

    const LONG_PTR staticType = GetWindowLongPtrW(hWnd, GWL_STYLE) & SS_TYPEMASK;
    return staticType == SS_LEFT ||
        staticType == SS_CENTER ||
        staticType == SS_RIGHT ||
        staticType == SS_LEFTNOWORDWRAP ||
        staticType == SS_SIMPLE;
}

static bool IsCustomPaintedControl(HWND hWnd) {
    return IsCheckOrRadioButton(hWnd) ||
        IsGroupBox(hWnd) ||
        IsTextStatic(hWnd) ||
        IsWndClass(hWnd, L"msctls_trackbar32") ||
        IsWndClass(hWnd, L"msctls_hotkey32");
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
    else if (IsWndClass(hWnd, L"Edit")) {
        // The common file dialog creates ordinary Edit controls for its search,
        // path, filename, and inline label-edit surfaces. DarkMode_CFD supplies
        // the matching dark selection and focus treatment for those controls.
        const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
        SetWindowTheme(
            hWnd,
            (style & ES_MULTILINE) ? L"DarkMode_Explorer" : L"DarkMode_CFD",
            nullptr);
    }
    else if (IsWndClass(hWnd, L"ComboBox") || IsWndClass(hWnd, L"ComboBoxEx32")) {
        SetWindowTheme(hWnd, L"DarkMode_CFD", nullptr);
    }
    else if (IsWndClass(hWnd, L"DirectUIHWND") && IsFileDialogItemsView(hWnd)) {
        // Keep the file-list selection visible. Applying ItemsView only to the
        // DirectUI item surface avoids changing the Explorer-themed scrollbar
        // and navigation chrome owned by its parent shell view.
        SetWindowTheme(hWnd, L"DarkMode_ItemsView", nullptr);
    }
    else if (IsWndClass(hWnd, L"SysTreeView32")) {
        SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);
    }
    else if (IsWndClass(hWnd, L"SysListView32")) {
        SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);
    }
    else if (IsWndClass(hWnd, L"Static")) {
        // Disable themed painting for every static control. Text labels are
        // custom-painted below, while owner-drawn/image/frame statics must stay
        // available to their owning window instead of inheriting a light
        // DarkMode_Explorer surface.
        SetWindowTheme(hWnd, L"", L"");
    }
    else if (IsWndClass(hWnd, L"msctls_trackbar32") ||
        IsWndClass(hWnd, L"msctls_hotkey32") ||
        IsWndClass(hWnd, L"SysTabControl32")) {
        SetWindowTheme(hWnd, L"", L"");
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
        else if (IsCheckOrRadioButton(hWnd)) {
            // Retain the Explorer theme for native hit testing while the
            // subclass owns painting. Empty themes can break checkbox and radio
            // interaction on some Windows common-control versions.
            SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);
        }
        else if (IsGroupBox(hWnd)) {
            SetWindowTheme(hWnd, L"", L"");
        }
        else {
            SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);
        }
    }
    else if (IsKnownControlClass(hWnd) ||
        IsKnownContainerClass(hWnd) ||
        IsTopLevelWindow(hWnd) ||
        IsCommonFileDialogWindow(hWnd)) {
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
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
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
    if (!hWnd || !IsWindow(hWnd) || IsPopupMenuWindow(hWnd)) return;
    g_hookStage = 0x30;
    MarkCommonFileDialog(hWnd);
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
        else if (message && message->message == g_removeThemeMessage) {
            RemoveWindowTree(message->hwnd);
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
    if (!hWnd || !IsWindow(hWnd) || !IsCurrentProcessWindow(hWnd) || IsPopupMenuWindow(hWnd)) return;

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

static BOOL CALLBACK RemoveChildSubclass(HWND hWnd, LPARAM) {
    RemoveWindowSubclass(hWnd, CallWndSubClassProc, 0);
    return TRUE;
}

static void RemoveWindowTree(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !IsCurrentProcessWindow(hWnd)) return;

    EnumChildWindows(hWnd, RemoveChildSubclass, 0);
    RemoveWindowSubclass(hWnd, CallWndSubClassProc, 0);
    if (IsTopLevelWindow(hWnd)) {
        RemovePropW(hWnd, kCommonFileDialogProperty);
    }
}

static void RemoveWindowOnOwningThread(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !IsCurrentProcessWindow(hWnd) || IsPopupMenuWindow(hWnd)) return;

    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(hWnd, &processId);
    if (processId != g_processId) return;

    if (threadId == GetCurrentThreadId()) {
        RemoveWindowTree(hWnd);
        return;
    }

    HHOOK callHook = SetWindowsHookExW(WH_CALLWNDPROC, ApplyThemeCallWndProc, g_module, threadId);
    if (!callHook) return;

    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(
        hWnd,
        g_removeThemeMessage,
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
    if (!g_ready ||
        event != EVENT_OBJECT_SHOW ||
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

static bool EnsureWindowEventHook() {
    if (g_windowEventHook) return true;

    g_windowEventHook = SetWinEventHook(
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_SHOW,
        g_module,
        WindowEventProc,
        g_processId,
        0,
        WINEVENT_INCONTEXT);
    if (!g_windowEventHook) {
        ReportWin32Failure(L"SetWinEventHook", GetLastError());
        return false;
    }

    return true;
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

static HDC BeginControlPaint(HWND hWnd, HDC suppliedDeviceContext, PAINTSTRUCT& paint) {
    return suppliedDeviceContext ? suppliedDeviceContext : BeginPaint(hWnd, &paint);
}

static void EndControlPaint(HWND hWnd, HDC suppliedDeviceContext, PAINTSTRUCT& paint) {
    if (!suppliedDeviceContext) EndPaint(hWnd, &paint);
}

static void SelectControlFont(HWND hWnd, HDC deviceContext, HGDIOBJ& oldFont) {
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
    oldFont = font ? SelectObject(deviceContext, font) : nullptr;
}

static void PaintCheckOrRadioButton(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    const theme_cfg* theme = LoadThemeConfig();
    PAINTSTRUCT paint = {};
    HDC deviceContext = BeginControlPaint(hWnd, suppliedDeviceContext, paint);
    if (!deviceContext) return;

    RECT bounds = {};
    GetClientRect(hWnd, &bounds);
    FillRect(deviceContext, &bounds, theme->dialog_bgbrush);

    const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    const LONG_PTR buttonType = style & BS_TYPEMASK;
    const bool radio = buttonType == BS_RADIOBUTTON || buttonType == BS_AUTORADIOBUTTON;
    const LRESULT state = SendMessageW(hWnd, BM_GETSTATE, 0, 0);
    const bool disabled = !IsWindowEnabled(hWnd);
    const bool pressed = (state & BST_PUSHED) != 0;
    const bool hot = (state & BST_HOT) != 0;
    const bool checked = (state & BST_CHECKED) != 0;
    const bool indeterminate = (state & BST_INDETERMINATE) != 0;

    const int glyphSize = std::max(12, GetSystemMetrics(SM_CXMENUCHECK));
    RECT glyphBounds = {};
    glyphBounds.top = bounds.top + std::max(0L, (bounds.bottom - bounds.top - glyphSize) / 2);
    glyphBounds.bottom = glyphBounds.top + glyphSize;
    if ((style & BS_LEFTTEXT) != 0) {
        glyphBounds.right = bounds.right - 2;
        glyphBounds.left = glyphBounds.right - glyphSize;
    }
    else {
        glyphBounds.left = bounds.left + 2;
        glyphBounds.right = glyphBounds.left + glyphSize;
    }

    HBRUSH glyphBrush = theme->control_bgbrush;
    if (pressed) glyphBrush = theme->control_bgbrush_pressed;
    else if (hot) glyphBrush = theme->control_bgbrush_hot;

    HPEN borderPen = CreatePen(PS_SOLID, 1, theme->control_bordercolor);
    HGDIOBJ oldPen = SelectObject(deviceContext, borderPen);
    HGDIOBJ oldBrush = SelectObject(deviceContext, glyphBrush);
    if (radio) {
        Ellipse(
            deviceContext,
            glyphBounds.left,
            glyphBounds.top,
            glyphBounds.right,
            glyphBounds.bottom);
    }
    else {
        Rectangle(
            deviceContext,
            glyphBounds.left,
            glyphBounds.top,
            glyphBounds.right,
            glyphBounds.bottom);
    }

    const COLORREF markColor = disabled
        ? theme->dialog_textcolor_disabled
        : theme->dialog_textcolor;
    HPEN markPen = CreatePen(PS_SOLID, 2, markColor);
    SelectObject(deviceContext, markPen);
    if (radio && checked) {
        HBRUSH markBrush = CreateSolidBrush(markColor);
        SelectObject(deviceContext, markBrush);
        RECT dotBounds = glyphBounds;
        InflateRect(&dotBounds, -4, -4);
        Ellipse(deviceContext, dotBounds.left, dotBounds.top, dotBounds.right, dotBounds.bottom);
        SelectObject(deviceContext, glyphBrush);
        DeleteObject(markBrush);
    }
    else if (indeterminate) {
        RECT markBounds = glyphBounds;
        InflateRect(&markBounds, -3, -4);
        HBRUSH markBrush = CreateSolidBrush(markColor);
        FillRect(deviceContext, &markBounds, markBrush);
        DeleteObject(markBrush);
    }
    else if (checked) {
        MoveToEx(deviceContext, glyphBounds.left + 3, glyphBounds.top + glyphSize / 2, nullptr);
        LineTo(deviceContext, glyphBounds.left + glyphSize / 2 - 1, glyphBounds.bottom - 4);
        LineTo(deviceContext, glyphBounds.right - 3, glyphBounds.top + 3);
    }
    SelectObject(deviceContext, oldBrush);
    SelectObject(deviceContext, oldPen);
    DeleteObject(markPen);
    DeleteObject(borderPen);

    wchar_t text[512] = {};
    GetWindowTextW(hWnd, text, static_cast<int>(_countof(text)));
    RECT textBounds = bounds;
    if ((style & BS_LEFTTEXT) != 0) {
        textBounds.right = glyphBounds.left - 6;
    }
    else {
        textBounds.left = glyphBounds.right + 6;
    }

    HGDIOBJ oldFont = nullptr;
    SelectControlFont(hWnd, deviceContext, oldFont);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, markColor);
    DrawTextW(
        deviceContext,
        text,
        -1,
        &textBounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (GetFocus() == hWnd) {
        RECT focusBounds = textBounds;
        InflateRect(&focusBounds, -1, -3);
        DrawFocusRect(deviceContext, &focusBounds);
    }
    if (oldFont) SelectObject(deviceContext, oldFont);

    EndControlPaint(hWnd, suppliedDeviceContext, paint);
}

static void PaintGroupBox(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    const theme_cfg* theme = LoadThemeConfig();
    PAINTSTRUCT paint = {};
    HDC deviceContext = BeginControlPaint(hWnd, suppliedDeviceContext, paint);
    if (!deviceContext) return;

    RECT bounds = {};
    GetClientRect(hWnd, &bounds);
    FillRect(deviceContext, &bounds, theme->dialog_bgbrush);

    wchar_t text[512] = {};
    GetWindowTextW(hWnd, text, static_cast<int>(_countof(text)));
    HGDIOBJ oldFont = nullptr;
    SelectControlFont(hWnd, deviceContext, oldFont);
    SIZE textSize = {};
    GetTextExtentPoint32W(deviceContext, text, static_cast<int>(wcslen(text)), &textSize);

    const int labelLeft = bounds.left + 9;
    const int labelRight = labelLeft + textSize.cx + 5;
    const int borderTop = bounds.top + std::max(5L, textSize.cy / 2);
    HPEN borderPen = CreatePen(PS_SOLID, 1, theme->control_bordercolor);
    HGDIOBJ oldPen = SelectObject(deviceContext, borderPen);
    MoveToEx(deviceContext, bounds.left, borderTop, nullptr);
    LineTo(deviceContext, labelLeft - 3, borderTop);
    MoveToEx(deviceContext, labelRight, borderTop, nullptr);
    LineTo(deviceContext, bounds.right - 1, borderTop);
    LineTo(deviceContext, bounds.right - 1, bounds.bottom - 1);
    LineTo(deviceContext, bounds.left, bounds.bottom - 1);
    LineTo(deviceContext, bounds.left, borderTop);
    SelectObject(deviceContext, oldPen);
    DeleteObject(borderPen);

    RECT textBounds = { labelLeft, bounds.top, labelRight, bounds.top + textSize.cy + 2 };
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(
        deviceContext,
        IsWindowEnabled(hWnd)
            ? theme->dialog_textcolor
            : theme->dialog_textcolor_disabled);
    DrawTextW(deviceContext, text, -1, &textBounds, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont) SelectObject(deviceContext, oldFont);

    EndControlPaint(hWnd, suppliedDeviceContext, paint);
}

static void PaintTrackbar(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    const theme_cfg* theme = LoadThemeConfig();
    PAINTSTRUCT paint = {};
    HDC deviceContext = BeginControlPaint(hWnd, suppliedDeviceContext, paint);
    if (!deviceContext) return;

    RECT bounds = {};
    RECT channelBounds = {};
    RECT thumbBounds = {};
    GetClientRect(hWnd, &bounds);
    SendMessageW(hWnd, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channelBounds));
    SendMessageW(hWnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumbBounds));
    FillRect(deviceContext, &bounds, theme->dialog_bgbrush);

    const bool vertical = (GetWindowLongPtrW(hWnd, GWL_STYLE) & TBS_VERT) != 0;
    RECT trackBounds = channelBounds;
    if (vertical) {
        const int center = (channelBounds.left + channelBounds.right) / 2;
        trackBounds.left = center - 2;
        trackBounds.right = center + 2;
    }
    else {
        const int center = (channelBounds.top + channelBounds.bottom) / 2;
        trackBounds.top = center - 2;
        trackBounds.bottom = center + 2;
    }
    FillRect(deviceContext, &trackBounds, theme->control_borderbrush);

    FillRect(
        deviceContext,
        &thumbBounds,
        IsWindowEnabled(hWnd)
            ? theme->control_bgbrush_hot
            : theme->control_bgbrush);
    FrameRect(deviceContext, &thumbBounds, theme->control_borderbrush);
    if (GetFocus() == hWnd) {
        RECT focusBounds = bounds;
        InflateRect(&focusBounds, -1, -1);
        DrawFocusRect(deviceContext, &focusBounds);
    }

    EndControlPaint(hWnd, suppliedDeviceContext, paint);
}

static std::wstring GetHotkeyText(HWND hWnd) {
    const WORD hotkey = static_cast<WORD>(SendMessageW(hWnd, HKM_GETHOTKEY, 0, 0));
    const BYTE virtualKey = LOBYTE(hotkey);
    const BYTE modifiers = HIBYTE(hotkey);
    if (!virtualKey) return {};

    std::wstring text;
    if ((modifiers & HOTKEYF_CONTROL) != 0) text += L"Ctrl + ";
    if ((modifiers & HOTKEYF_SHIFT) != 0) text += L"Shift + ";
    if ((modifiers & HOTKEYF_ALT) != 0) text += L"Alt + ";
    if ((modifiers & HOTKEYF_EXT) != 0) text += L"Ext + ";

    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    LPARAM keyNameParameter = static_cast<LPARAM>(scanCode << 16);
    if ((modifiers & HOTKEYF_EXT) != 0) keyNameParameter |= 1 << 24;
    wchar_t keyName[128] = {};
    if (GetKeyNameTextW(static_cast<LONG>(keyNameParameter), keyName, static_cast<int>(_countof(keyName))) > 0) {
        text += keyName;
    }
    else {
        text += static_cast<wchar_t>(virtualKey);
    }
    return text;
}

static void PaintHotkeyControl(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    const theme_cfg* theme = LoadThemeConfig();
    PAINTSTRUCT paint = {};
    HDC deviceContext = BeginControlPaint(hWnd, suppliedDeviceContext, paint);
    if (!deviceContext) return;

    RECT bounds = {};
    GetClientRect(hWnd, &bounds);
    FillRect(deviceContext, &bounds, theme->control_bgbrush);
    FrameRect(deviceContext, &bounds, theme->control_borderbrush);

    const std::wstring text = GetHotkeyText(hWnd);
    HGDIOBJ oldFont = nullptr;
    SelectControlFont(hWnd, deviceContext, oldFont);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(
        deviceContext,
        IsWindowEnabled(hWnd)
            ? theme->dialog_textcolor
            : theme->dialog_textcolor_disabled);
    RECT textBounds = bounds;
    textBounds.left += 5;
    textBounds.right -= 5;
    DrawTextW(
        deviceContext,
        text.c_str(),
        -1,
        &textBounds,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (GetFocus() == hWnd) {
        RECT focusBounds = bounds;
        InflateRect(&focusBounds, -3, -3);
        DrawFocusRect(deviceContext, &focusBounds);
    }
    if (oldFont) SelectObject(deviceContext, oldFont);

    EndControlPaint(hWnd, suppliedDeviceContext, paint);
}

static void PaintStaticText(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    const theme_cfg* theme = LoadThemeConfig();
    PAINTSTRUCT paint = {};
    HDC deviceContext = BeginControlPaint(hWnd, suppliedDeviceContext, paint);
    if (!deviceContext) return;

    RECT bounds = {};
    GetClientRect(hWnd, &bounds);
    FillRect(deviceContext, &bounds, theme->dialog_bgbrush);

    wchar_t text[1024] = {};
    GetWindowTextW(hWnd, text, static_cast<int>(_countof(text)));
    HGDIOBJ oldFont = nullptr;
    SelectControlFont(hWnd, deviceContext, oldFont);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(
        deviceContext,
        IsWindowEnabled(hWnd)
            ? theme->dialog_textcolor
            : theme->dialog_textcolor_disabled);

    const LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    const LONG_PTR staticType = style & SS_TYPEMASK;
    UINT format = DT_LEFT;
    if (staticType == SS_CENTER) format = DT_CENTER;
    else if (staticType == SS_RIGHT) format = DT_RIGHT;
    if ((style & SS_NOPREFIX) != 0) format |= DT_NOPREFIX;
    if (staticType == SS_LEFTNOWORDWRAP || staticType == SS_SIMPLE) {
        format |= DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    }
    else {
        format |= DT_WORDBREAK | DT_EDITCONTROL;
        if ((style & SS_CENTERIMAGE) != 0) format |= DT_VCENTER;
    }
    DrawTextW(deviceContext, text, -1, &bounds, format);
    if (oldFont) SelectObject(deviceContext, oldFont);

    EndControlPaint(hWnd, suppliedDeviceContext, paint);
}

static bool PaintSpecializedControl(HWND hWnd, HDC suppliedDeviceContext = nullptr) {
    if (IsCheckOrRadioButton(hWnd)) {
        PaintCheckOrRadioButton(hWnd, suppliedDeviceContext);
    }
    else if (IsGroupBox(hWnd)) {
        PaintGroupBox(hWnd, suppliedDeviceContext);
    }
    else if (IsWndClass(hWnd, L"msctls_trackbar32")) {
        PaintTrackbar(hWnd, suppliedDeviceContext);
    }
    else if (IsWndClass(hWnd, L"msctls_hotkey32")) {
        PaintHotkeyControl(hWnd, suppliedDeviceContext);
    }
    else if (IsTextStatic(hWnd)) {
        PaintStaticText(hWnd, suppliedDeviceContext);
    }
    else {
        return false;
    }
    return true;
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
        case WM_CTLCOLORBTN:
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
            if (IsCustomPaintedControl(hWnd)) {
                RECT bounds = {};
                GetClientRect(hWnd, &bounds);
                FillRect(
                    reinterpret_cast<HDC>(wParam),
                    &bounds,
                    IsWndClass(hWnd, L"msctls_hotkey32")
                        ? LoadThemeConfig()->control_bgbrush
                        : LoadThemeConfig()->dialog_bgbrush);
                return TRUE;
            }
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
                // This runs synchronously on the owning UI thread. It is late
                // enough for native controls to be initialized, but early
                // enough to avoid a light first paint while Unity is blocked
                // in managed callbacks or a domain reload.
                ApplyControlTheme(hWnd);
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
            if (PaintSpecializedControl(hWnd)) {
                return 0;
            }
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
            if (PaintSpecializedControl(hWnd, reinterpret_cast<HDC>(wParam))) {
                return 0;
            }
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
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MOUSEMOVE:
        {
            if (IsWndClass(hWnd, L"msctls_trackbar32")) {
                const LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                InvalidateRect(hWnd, nullptr, TRUE);
                return result;
            }
            break;
        }
        case WM_SETTEXT:
        case WM_ENABLE:
        case BM_SETCHECK:
        case HKM_SETHOTKEY:
        {
            if (IsCustomPaintedControl(hWnd)) {
                const LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);
                InvalidateRect(hWnd, nullptr, TRUE);
                return result;
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
            if (IsTopLevelWindow(hWnd) && !g_refreshingThemeState) {
                g_refreshingThemeState = true;
                RefreshDarkModeState();
                ThemeWindowTree(hWnd);
                g_refreshingThemeState = false;
            }
            else {
                ApplyControlTheme(hWnd);
            }
            break;
        }
        case WM_NCDESTROY:
        {
            if (IsTopLevelWindow(hWnd)) {
                RemovePropW(hWnd, kCommonFileDialogProperty);
            }
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
    if (!IsCurrentProcessWindow(hWnd) || IsPopupMenuWindow(hWnd)) return;

    EnableDarkMode(hWnd);
    if (GetWindowThreadProcessId(hWnd, nullptr) == GetCurrentThreadId()) {
        SetWindowSubclass(hWnd, CallWndSubClassProc, 0, 0);
        EnumChildWindows(hWnd, AttachExistingChildWindow, 0);
    }
}

static bool PinNativeModule() {
    if (g_modulePinned) return true;

    HMODULE pinnedModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(PinNativeModule),
            &pinnedModule)) {
        ReportWin32Failure(L"GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN)", GetLastError());
        return false;
    }

    g_modulePinned = true;
    return true;
}

extern "C" BOOL APIENTRY UnityEditorDarkMode_Initialize() {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (!g_module) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return FALSE;
    }

    g_processId = GetCurrentProcessId();

    if (!g_applyThemeMessage) {
        g_applyThemeMessage = RegisterWindowMessageW(L"UnityEditorDarkMode.ApplyTheme");
    }
    if (!g_removeThemeMessage) {
        g_removeThemeMessage = RegisterWindowMessageW(L"UnityEditorDarkMode.RemoveTheme");
    }
    if (!g_applyThemeMessage || !g_removeThemeMessage || !PinNativeModule()) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return FALSE;
    }

    g_ready = false;
    RefreshDarkModeState();

    std::vector<HWND> windowHandles;
    GetAllWindowsByProcessID(g_processId, windowHandles);

    HWND unityWindow = nullptr;
    DWORD unityWindowThreadId = 0;
    for (const HWND& hWnd : windowHandles) {
        if (IsUnityWndClass(hWnd)) {
            unityWindow = hWnd;
            unityWindowThreadId = GetWindowThreadProcessId(hWnd, nullptr);
        }
        ThemeWindowOnOwningThread(hWnd);
    }

    const DWORD targetThreadId = unityWindowThreadId ? unityWindowThreadId : GetCurrentThreadId();
    if (g_hook && g_hookThreadId != targetThreadId) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        g_hookThreadId = 0;
    }

    if (!g_hook) {
        g_hook = SetWindowsHookExW(WH_CBT, CBTProc, nullptr, targetThreadId);
        if (g_hook) {
            g_hookThreadId = targetThreadId;
        }
    }

    const bool windowEventHookReady = EnsureWindowEventHook();

    BOOL darkModeEnabled = FALSE;
    HRESULT darkModeResult = unityWindow
        ? DwmGetWindowAttribute(
            unityWindow,
            static_cast<DWMWINDOWATTRIBUTE>(20),
            &darkModeEnabled,
            sizeof(darkModeEnabled))
        : E_HANDLE;
    if (FAILED(darkModeResult) && unityWindow) {
        darkModeResult = DwmGetWindowAttribute(
            unityWindow,
            static_cast<DWMWINDOWATTRIBUTE>(19),
            &darkModeEnabled,
            sizeof(darkModeEnabled));
    }

    g_ready = windowEventHookReady && SUCCEEDED(darkModeResult) && darkModeEnabled;
    const BOOL result = g_ready ? TRUE : FALSE;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return result;
}

extern "C" BOOL APIENTRY UnityEditorDarkMode_Shutdown() {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    g_ready = false;

    if (g_windowEventHook) {
        UnhookWinEvent(g_windowEventHook);
        g_windowEventHook = nullptr;
    }
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        g_hookThreadId = 0;
    }

    if (g_processId && g_removeThemeMessage) {
        std::vector<HWND> windowHandles;
        GetAllWindowsByProcessID(g_processId, windowHandles);
        for (const HWND& hWnd : windowHandles) {
            RemoveWindowOnOwningThread(hWnd);
        }
    }

    if (g_menuTheme) {
        CloseThemeData(g_menuTheme);
        g_menuTheme = nullptr;
    }

    ReleaseSRWLockExclusive(&g_lifecycleLock);
    return TRUE;
}

// DLL entry
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason)
    {
        case DLL_PROCESS_ATTACH: {
            g_module = hModule;
            DisableThreadLibraryCalls(hModule);

            // Loader-lock-safe startup defers all work described below to UnityEditorDarkMode_Initialize.
            // Enable process-wide menu dark mode before attaching window hooks.

            // Catch windows created before Unity loaded this preloaded native plugin.

            // CBT gives synchronous coverage for Unity's main GUI thread. The
            // process-scoped in-context WinEvent hook supplements it for dialogs
            // created by other Unity-owned UI threads. EVENT_OBJECT_SHOW runs on
            // the owner before the first stable paint, even while Unity blocks
            // the main thread for managed callbacks or a domain reload.
            break;
        }
        case DLL_PROCESS_DETACH: {
            // When FreeLibrary unloads the plugin before process exit, remove
            // callbacks that would otherwise point into the unloaded module.
            // Explicit shutdown removes callbacks on their owner threads. The module is pinned after initialization.
            (void)reserved;
            break;
        }
        default: break;
    }

    return TRUE;
}
