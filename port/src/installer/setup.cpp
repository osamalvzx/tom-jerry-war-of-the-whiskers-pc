#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

static HWND g_hPathEdit = NULL;
static HWND g_hStatusText = NULL;
static HWND g_hInstallBtn = NULL;
static HWND g_hLaunchBtn = NULL;
static HWND g_hOpenModsBtn = NULL;
static HWND g_hShortcutCheck = NULL;
static char g_gamePath[MAX_PATH] = "";
static HINSTANCE g_hInstance = NULL;

static bool CheckValidGameDir(const char* dir) {
    char xbe[MAX_PATH], exe[MAX_PATH];
    _snprintf_s(xbe, sizeof(xbe), _TRUNCATE, "%s\\default.xbe", dir);
    _snprintf_s(exe, sizeof(exe), _TRUNCATE, "%s\\Tom and Jerry - War of the Whiskers.exe", dir);
    return (GetFileAttributesA(xbe) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesA(exe) != INVALID_FILE_ATTRIBUTES);
}

static void AutoDetectGameDir(char* out, size_t cap) {
    char cur[MAX_PATH];
    GetModuleFileNameA(NULL, cur, MAX_PATH);
    char* p = strrchr(cur, '\\');
    if (p) {
        *p = '\0';
        if (CheckValidGameDir(cur)) {
            strncpy_s(out, cap, cur, _TRUNCATE);
            return;
        }
    }

    const char* candidates[] = {
        "C:\\Users\\goldl\\Games\\Tom and Jerry War of the Whiskers",
        "C:\\Games\\Tom and Jerry War of the Whiskers",
        "D:\\Games\\Tom and Jerry War of the Whiskers",
        "E:\\Games\\Tom and Jerry War of the Whiskers",
        "C:\\Program Files (x86)\\Tom and Jerry War of the Whiskers",
        "C:\\Program Files\\Tom and Jerry War of the Whiskers"
    };
    for (const char* c : candidates) {
        if (CheckValidGameDir(c)) {
            strncpy_s(out, cap, c, _TRUNCATE);
            return;
        }
    }

    strncpy_s(out, cap, "C:\\Games\\Tom and Jerry War of the Whiskers", _TRUNCATE);
}

static void CreateShortcut(const char* targetExe, const char* workingDir, const char* shortcutName) {
    CoInitialize(NULL);
    IShellLinkA* psl = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkA, (void**)&psl))) {
        psl->SetPath(targetExe);
        psl->SetWorkingDirectory(workingDir);
        psl->SetDescription("Tom and Jerry: War of the Whiskers (Mods Enhanced)");

        IPersistFile* ppf = NULL;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
            char desktop[MAX_PATH];
            SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop);
            char linkPath[MAX_PATH];
            _snprintf_s(linkPath, sizeof(linkPath), _TRUNCATE, "%s\\%s.lnk", desktop, shortcutName);
            WCHAR wLinkPath[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, linkPath, -1, wLinkPath, MAX_PATH);
            ppf->Save(wLinkPath, TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
}

static bool ExtractPayloadTo(const char* destDir) {
    HRSRC hRes = FindResourceA(g_hInstance, MAKEINTRESOURCEA(100), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hMem = LoadResource(g_hInstance, hRes);
    if (!hMem) return false;
    void* pData = LockResource(hMem);
    DWORD dwSize = SizeofResource(g_hInstance, hRes);
    if (!pData || dwSize == 0) return false;

    char tempPath[MAX_PATH], tempZip[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);
    _snprintf_s(tempZip, sizeof(tempZip), _TRUNCATE, "%stj_mods_payload.zip", tempPath);

    FILE* fp = NULL;
    fopen_s(&fp, tempZip, "wb");
    if (!fp) return false;
    fwrite(pData, 1, dwSize, fp);
    fclose(fp);

    char cmd[1024];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\"",
                tempZip, destDir);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    DeleteFileA(tempZip);
    return true;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hFontBold = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        HWND hTitle = CreateWindowA("STATIC", "Tom and Jerry: War of the Whiskers\nMODS CONFIG & Enhancements Setup",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 15, 460, 45, hWnd, NULL, NULL, NULL);
        SendMessageA(hTitle, WM_SETFONT, (WPARAM)hFontBold, TRUE);

        HWND hDesc = CreateWindowA("STATIC", "This installer will configure the in-game Mods Menu, 60FPS transitions, custom 3D badges, asset overrides, and prepare the 'mods/' directory for community mods.",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 65, 460, 40, hWnd, NULL, NULL, NULL);
        SendMessageA(hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hPathLbl = CreateWindowA("STATIC", "Select Game Installation Folder („Ã·œ «··⁄»…):",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 115, 460, 20, hWnd, NULL, NULL, NULL);
        SendMessageA(hPathLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        AutoDetectGameDir(g_gamePath, sizeof(g_gamePath));
        g_hPathEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_gamePath,
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 140, 360, 26, hWnd, (HMENU)101, NULL, NULL);
        SendMessageA(g_hPathEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBrowseBtn = CreateWindowA("BUTTON", "Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        390, 139, 90, 28, hWnd, (HMENU)102, NULL, NULL);
        SendMessageA(hBrowseBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hShortcutCheck = CreateWindowA("BUTTON", "Create Desktop Shortcut (≈‰‘«¡ «Œ ’«— ⁄·Ï ”ÿÕ «·„ﬂ »)",
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 175, 460, 24, hWnd, (HMENU)103, NULL, NULL);
        SendMessageA(g_hShortcutCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(g_hShortcutCheck, BM_SETCHECK, BST_CHECKED, 0);

        g_hStatusText = CreateWindowA("STATIC", "Ready to install. Click 'Install Mods Config' below.",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 210, 460, 35, hWnd, (HMENU)104, NULL, NULL);
        SendMessageA(g_hStatusText, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_hInstallBtn = CreateWindowA("BUTTON", "Install Mods Config ( À»Ì  «·„Êœ« )",
                                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 250, 220, 36, hWnd, (HMENU)105, NULL, NULL);
        SendMessageA(g_hInstallBtn, WM_SETFONT, (WPARAM)hFontBold, TRUE);

        g_hLaunchBtn = CreateWindowA("BUTTON", "Launch Game ( ‘€Ì· «··⁄»…)",
                                     WS_CHILD | BS_PUSHBUTTON, 250, 250, 140, 36, hWnd, (HMENU)106, NULL, NULL);
        SendMessageA(g_hLaunchBtn, WM_SETFONT, (WPARAM)hFontBold, TRUE);

        g_hOpenModsBtn = CreateWindowA("BUTTON", "GitHub (osamalvzx)", WS_CHILD | BS_PUSHBUTTON, 395, 250, 100, 36, hWnd, (HMENU)107, NULL, NULL);
        SendMessageA(g_hOpenModsBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == 102) { // Browse
            BROWSEINFOA bi = { 0 };
            bi.lpszTitle = "Select Tom and Jerry Game Directory:";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
            if (pidl) {
                SHGetPathFromIDListA(pidl, g_gamePath);
                CoTaskMemFree(pidl);
                SetWindowTextA(g_hPathEdit, g_gamePath);
            }
        }
        else if (id == 105) { // Install
            GetWindowTextA(g_hPathEdit, g_gamePath, sizeof(g_gamePath));
            if (!g_gamePath[0]) {
                MessageBoxA(hWnd, "Please select the game installation folder!", "Error", MB_ICONERROR);
                return 0;
            }

            SetWindowTextA(g_hStatusText, "Installing mods config and assets... Please wait.");
            EnableWindow(g_hInstallBtn, FALSE);
            UpdateWindow(hWnd);

            CreateDirectoryA(g_gamePath, NULL);
            bool ok = ExtractPayloadTo(g_gamePath);

            if (ok) {
                if (SendMessageA(g_hShortcutCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    char targetExe[MAX_PATH];
                    _snprintf_s(targetExe, sizeof(targetExe), _TRUNCATE, "%s\\Tom and Jerry - War of the Whiskers.exe", g_gamePath);
                    if (GetFileAttributesA(targetExe) == INVALID_FILE_ATTRIBUTES) {
                        _snprintf_s(targetExe, sizeof(targetExe), _TRUNCATE, "%s\\PLAY.cmd", g_gamePath);
                    }
                    CreateShortcut(targetExe, g_gamePath, "Tom & Jerry (Mods Enhanced)");
                }

                SetWindowTextA(g_hStatusText, "SUCCESS: Mods Config and System installed successfully!\n „  À»Ì  «·„Êœ«  Ê„Ã·œ mods/ »‰Ã«Õ!");
                ShowWindow(g_hLaunchBtn, SW_SHOW);
                ShowWindow(g_hOpenModsBtn, SW_SHOW);
                SetWindowTextA(g_hInstallBtn, "Re-Install (≈⁄«œ… «· À»Ì )");
                EnableWindow(g_hInstallBtn, TRUE);

                MessageBoxA(hWnd, "Mods Config installed successfully!\n\nAll enhancements, 3D badges, 60FPS cartoon transitions, and the 'mods/' folder are ready.\n\nEnjoy the game!", "Installation Complete", MB_ICONINFORMATION);
            } else {
                SetWindowTextA(g_hStatusText, "Installation failed. Please check folder permissions.");
                EnableWindow(g_hInstallBtn, TRUE);
                MessageBoxA(hWnd, "Failed to extract files. Make sure you have write permissions to the destination folder.", "Error", MB_ICONERROR);
            }
        }
        else if (id == 106) { // Launch Game
            GetWindowTextA(g_hPathEdit, g_gamePath, sizeof(g_gamePath));
            char targetExe[MAX_PATH];
            _snprintf_s(targetExe, sizeof(targetExe), _TRUNCATE, "%s\\Tom and Jerry - War of the Whiskers.exe", g_gamePath);
            if (GetFileAttributesA(targetExe) == INVALID_FILE_ATTRIBUTES) {
                _snprintf_s(targetExe, sizeof(targetExe), _TRUNCATE, "%s\\PLAY.cmd", g_gamePath);
            }
            ShellExecuteA(NULL, "open", targetExe, NULL, g_gamePath, SW_SHOWNORMAL);
            PostQuitMessage(0);
        }
        else if (id == 107) { // Open mods/ folder
            GetWindowTextA(g_hPathEdit, g_gamePath, sizeof(g_gamePath));
            char modsPath[MAX_PATH];
            _snprintf_s(modsPath, sizeof(modsPath), _TRUNCATE, "%s\\mods", g_gamePath);
            CreateDirectoryA(modsPath, NULL);
            ShellExecuteA(NULL, "open", modsPath, NULL, NULL, SW_SHOWNORMAL);
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;
    InitCommonControls();
    
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "TJ_Mods_Installer_Class";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExA(&wc)) return 1;

    int w = 515, h = 345;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - w) / 2;
    int y = (screenH - h) / 2;

    HWND hWnd = CreateWindowExA(WS_EX_APPWINDOW, "TJ_Mods_Installer_Class",
                                "Tom and Jerry: War of the Whiskers - Mods Installer",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!hWnd) return 1;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
