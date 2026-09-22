#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kWindowClass[] = L"UnityEditorDarkModeDialogHarness";
constexpr int kCloseButtonId = 1001;

void Trace(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
}

void ApplyDefaultFont(HWND window) {
    const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(window, [](HWND child, LPARAM fontValue) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontValue), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
}

LRESULT CALLBACK HarnessWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND:
            if (LOWORD(wParam) == kCloseButtonId) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND AddControl(
    HWND parent,
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id = 0,
    DWORD extendedStyle = 0) {
    fwprintf(stderr, L"Creating %s (id %d).\n", className, id);
    std::fflush(stderr);
    return CreateWindowExW(
        extendedStyle,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
}
}

int wmain() {
    Trace("Starting harness.");
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX commonControls = {
        sizeof(commonControls),
        ICC_PROGRESS_CLASS | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&commonControls);

    HMODULE darkModePlugin = LoadLibraryW(L"UnityEditorDarkMode.dll");
    if (!darkModePlugin) {
        std::fprintf(stderr, "Could not load UnityEditorDarkMode.dll (Win32 error %lu).\n", GetLastError());
        return 1;
    }
    Trace("Loaded plugin.");

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = HarnessWindowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&windowClass);
    Trace("Registered harness class.");

    const int width = 640;
    const int height = 520;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClass,
        L"Unity Editor Dark Mode - Native Dialog Harness",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!window) {
        std::fprintf(stderr, "Could not create harness window (Win32 error %lu).\n", GetLastError());
        FreeLibrary(darkModePlugin);
        return 2;
    }
    Trace("Created harness window.");

    AddControl(window, L"Static", L"Native dialog title", SS_LEFT, 24, 22, 260, 24);
    AddControl(window, L"Static", L"Labels, inputs, choices, lists, trees, tabs, and progress bars", SS_LEFT, 24, 50, 560, 22);
    AddControl(window, L"Edit", L"Editable text", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, 82, 280, 28, 2001, WS_EX_CLIENTEDGE);

    HWND combo = AddControl(window, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 324, 82, 280, 180, 2002);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"First option"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Second option"));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);

    AddControl(window, L"Button", L"Checkbox", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 126, 130, 24, 2003);
    AddControl(window, L"Button", L"Radio option", BS_AUTORADIOBUTTON | WS_TABSTOP, 170, 126, 140, 24, 2004);
    AddControl(window, L"Button", L"Disabled", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 324, 120, 130, 32, 2005);
    AddControl(window, L"Button", L"Primary action", BS_DEFPUSHBUTTON | WS_TABSTOP, 474, 120, 130, 32, 2006);

    HWND list = AddControl(window, L"ListBox", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 24, 172, 280, 128, 2007, WS_EX_CLIENTEDGE);
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"List item one"));
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"List item two"));
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"List item three"));
    SendMessageW(list, LB_SETCURSEL, 1, 0);

    HWND tree = AddControl(window, WC_TREEVIEWW, L"", TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | WS_BORDER | WS_TABSTOP, 324, 172, 280, 128, 2008, WS_EX_CLIENTEDGE);
    TVINSERTSTRUCTW rootItem = {};
    rootItem.hParent = TVI_ROOT;
    rootItem.hInsertAfter = TVI_LAST;
    rootItem.item.mask = TVIF_TEXT;
    rootItem.item.pszText = const_cast<wchar_t*>(L"Tree root");
    HTREEITEM root = TreeView_InsertItem(tree, &rootItem);
    TVINSERTSTRUCTW childItem = rootItem;
    childItem.hParent = root;
    childItem.item.pszText = const_cast<wchar_t*>(L"Tree child");
    TreeView_InsertItem(tree, &childItem);
    TreeView_Expand(tree, root, TVE_EXPAND);

    HWND tabs = AddControl(window, WC_TABCONTROLW, L"", WS_TABSTOP, 24, 320, 580, 38, 2009);
    TCITEMW tabItem = {};
    tabItem.mask = TCIF_TEXT;
    tabItem.pszText = const_cast<wchar_t*>(L"General");
    TabCtrl_InsertItem(tabs, 0, &tabItem);
    tabItem.pszText = const_cast<wchar_t*>(L"Advanced");
    TabCtrl_InsertItem(tabs, 1, &tabItem);

    HWND progress = AddControl(window, PROGRESS_CLASSW, L"", PBS_SMOOTH, 24, 380, 580, 24, 2010);
    SendMessageW(progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(progress, PBM_SETPOS, 62, 0);
    AddControl(window, L"Static", L"62% complete", SS_LEFT, 24, 412, 180, 22);
    AddControl(window, L"Button", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 484, 420, 120, 34, kCloseButtonId);

    ApplyDefaultFont(window);
    Trace("Applied fonts.");
    ShowWindow(window, SW_SHOW);
    Trace("Showed harness window.");
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    FreeLibrary(darkModePlugin);
    return static_cast<int>(message.wParam);
}
