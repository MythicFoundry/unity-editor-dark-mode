#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cwchar>
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {
constexpr wchar_t kWindowClass[] = L"UnityContainerWndClass";
constexpr int kCloseButtonId = 1001;
constexpr int kWorkerButtonId = 1002;
constexpr int kWorkerProgressId = 1003;
constexpr int kForceDarkAppMode = 2;
constexpr int kForceLightAppMode = 3;
constexpr wchar_t kWorkerDialogFlag[] = L"--verify-worker-dialog";

struct WorkerDialogState {
    HWND owner = nullptr;
    HWND button = nullptr;
    int attempts = 0;
    bool passed = false;
    DWORD errorCode = ERROR_SUCCESS;
};

using SetPreferredAppMode = int(WINAPI*)(int);

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

bool HasImmersiveDarkMode(HWND window) {
    BOOL darkModeEnabled = FALSE;
    HRESULT result = DwmGetWindowAttribute(
        window,
        static_cast<DWMWINDOWATTRIBUTE>(20),
        &darkModeEnabled,
        sizeof(darkModeEnabled));
    if (FAILED(result)) {
        result = DwmGetWindowAttribute(
            window,
            static_cast<DWMWINDOWATTRIBUTE>(19),
            &darkModeEnabled,
            sizeof(darkModeEnabled));
    }
    return SUCCEEDED(result) && darkModeEnabled;
}

INT_PTR CALLBACK WorkerDialogProc(HWND dialog, UINT message, WPARAM, LPARAM lParam) {
    WorkerDialogState* state = reinterpret_cast<WorkerDialogState*>(
        GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
        case WM_INITDIALOG:
        {
            state = reinterpret_cast<WorkerDialogState*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, lParam);
            SetWindowPos(
                dialog,
                nullptr,
                -32000,
                -32000,
                440,
                150,
                SWP_NOACTIVATE | SWP_NOZORDER);
            AddControl(dialog, L"Static", L"Building Player", SS_LEFT, 18, 18, 260, 22);
            AddControl(
                dialog,
                PROGRESS_CLASSW,
                L"",
                PBS_SMOOTH,
                18,
                52,
                400,
                20,
                kWorkerProgressId);
            state->button = AddControl(
                dialog,
                L"Button",
                L"Cancel",
                BS_PUSHBUTTON,
                318,
                86,
                100,
                30,
                kWorkerButtonId);
            SetTimer(dialog, 1, 20, nullptr);
            return TRUE;
        }
        case WM_TIMER:
        {
            if (!state) return FALSE;

            const LONG_PTR buttonStyle = GetWindowLongPtrW(state->button, GWL_STYLE);
            state->passed = HasImmersiveDarkMode(dialog) &&
                (buttonStyle & BS_TYPEMASK) == BS_OWNERDRAW;
            ++state->attempts;
            if (state->passed || state->attempts >= 100) {
                KillTimer(dialog, 1);
                EndDialog(dialog, state->passed ? IDOK : IDCANCEL);
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}

DWORD WINAPI WorkerDialogThread(void* parameter) {
    WorkerDialogState* state = static_cast<WorkerDialogState*>(parameter);
    alignas(DWORD) struct {
        DLGTEMPLATE dialog;
        WORD menu;
        WORD windowClass;
        wchar_t title;
    } dialogTemplate = {};
    dialogTemplate.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    dialogTemplate.dialog.dwExtendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    dialogTemplate.dialog.cdit = 0;
    dialogTemplate.dialog.cx = 220;
    dialogTemplate.dialog.cy = 75;

    const INT_PTR result = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        &dialogTemplate.dialog,
        state->owner,
        WorkerDialogProc,
        reinterpret_cast<LPARAM>(state));
    if (result == -1) {
        state->errorCode = GetLastError();
        return 1;
    }
    return state->passed ? 0 : 2;
}

bool VerifyWorkerThreadDialog(HWND owner) {
    WorkerDialogState state = {};
    state.owner = owner;
    HANDLE thread = CreateThread(nullptr, 0, WorkerDialogThread, &state, 0, nullptr);
    if (!thread) {
        std::fprintf(stderr, "Could not create worker dialog thread (Win32 error %lu).\n", GetLastError());
        return false;
    }

    DWORD waitResult = WAIT_TIMEOUT;
    while (waitResult != WAIT_OBJECT_0) {
        waitResult = MsgWaitForMultipleObjects(1, &thread, FALSE, 5000, QS_ALLINPUT);
        if (waitResult == WAIT_OBJECT_0 + 1) {
            MSG message = {};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        else if (waitResult != WAIT_OBJECT_0) {
            std::fprintf(stderr, "Worker dialog test timed out or failed while waiting (result %lu).\n", waitResult);
            CloseHandle(thread);
            return false;
        }
    }

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    if (!state.passed || exitCode != 0) {
        std::fprintf(
            stderr,
            "Worker-thread #32770 dialog was not fully themed (thread result %lu, Win32 error %lu).\n",
            exitCode,
            state.errorCode);
        return false;
    }

    Trace("Verified worker-thread #32770 dialog dark title bar and child control theming.");
    return true;
}
}

int wmain(int argumentCount, wchar_t* arguments[]) {
    Trace("Starting harness.");
    const bool verifyWorkerDialog = argumentCount == 2 &&
        wcscmp(arguments[1], kWorkerDialogFlag) == 0;
    if (argumentCount > 1 && !verifyWorkerDialog) {
        std::fprintf(stderr, "Unknown harness argument.\n");
        return 1;
    }

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
    using InitializeDarkMode = BOOL(APIENTRY*)();
    const auto initializeDarkMode = reinterpret_cast<InitializeDarkMode>(
        GetProcAddress(darkModePlugin, "UnityEditorDarkMode_Initialize"));
    if (!initializeDarkMode) {
        std::fprintf(stderr, "Could not find UnityEditorDarkMode_Initialize (Win32 error %lu).\n", GetLastError());
        FreeLibrary(darkModePlugin);
        return 2;
    }

    HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll");
    auto setPreferredAppMode = uxtheme
        ? reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)))
        : nullptr;
    if (setPreferredAppMode) {
        setPreferredAppMode(kForceLightAppMode); // Simulate an explicit Unity override.
        Trace("Reset process app mode to ForceLight before creating the window.");
    }

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

    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, 3001, L"Open");
    AppendMenuW(fileMenu, MF_STRING, 3002, L"Save");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"File");
    SetMenu(window, menuBar);

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
    if (!verifyWorkerDialog) {
        ShowWindow(window, SW_SHOW);
        Trace("Showed harness window.");
        UpdateWindow(window);
    }
    if (!initializeDarkMode()) {
        Trace("UnityEditorDarkMode_Initialize could not attach to the harness window.");
        DestroyWindow(window);
        FreeLibrary(darkModePlugin);
        return 3;
    }
    Trace("Initialized plugin after creating the harness window.");
    if (setPreferredAppMode) {
        const int previousAppMode = setPreferredAppMode(kForceDarkAppMode);
        if (previousAppMode != kForceDarkAppMode) {
            std::fprintf(
                stderr,
                "Late initialization did not restore ForceDark app mode (previous mode %d).\n",
                previousAppMode);
            DestroyWindow(window);
            FreeLibrary(darkModePlugin);
            return 4;
        }
        Trace("Verified late initialization restored ForceDark app mode.");
    }

    if (verifyWorkerDialog) {
        const bool workerDialogPassed = VerifyWorkerThreadDialog(window);
        DestroyWindow(window);
        FreeLibrary(darkModePlugin);
        return workerDialogPassed ? 0 : 5;
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    FreeLibrary(darkModePlugin);
    return static_cast<int>(message.wParam);
}
