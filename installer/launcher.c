// Tiny native launcher for MTF-Recomp-Setup.exe: invokes python\pythonw.exe -m installer.gui
// using the bundled embeddable Python instead of duplicating Python via PyInstaller.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

static void append_quoted(wchar_t *dst, size_t cap, const wchar_t *arg) {
    size_t len = wcslen(dst);
    if (len + 2 >= cap) return;
    if (len > 0) dst[len++] = L' ';
    int need_q = (arg[0] == 0 || wcschr(arg, L' ') || wcschr(arg, L'\t') || wcschr(arg, L'"'));
    if (need_q && len < cap - 1) dst[len++] = L'"';
    for (const wchar_t *p = arg; *p && len < cap - 2; p++) {
        if (*p == L'"') dst[len++] = L'\\';
        dst[len++] = *p;
    }
    if (need_q && len < cap - 1) dst[len++] = L'"';
    dst[len] = 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmdLine; (void)nShow;

    wchar_t root[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, root, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(root, L'\\');
    if (slash) *slash = 0;

    wchar_t pyw[MAX_PATH];
    _snwprintf(pyw, MAX_PATH, L"%s\\python\\pythonw.exe", root);
    if (GetFileAttributesW(pyw) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"Could not find python\\pythonw.exe next to MTF-Recomp-Setup.exe.",
                    L"MTF Recomp Setup", MB_ICONERROR | MB_OK);
        return 1;
    }

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t cmd[32768] = {0};
    append_quoted(cmd, 32768, pyw);
    append_quoted(cmd, 32768, L"-m");
    append_quoted(cmd, 32768, L"installer.gui");
    for (int i = 1; i < argc; i++) append_quoted(cmd, 32768, argv[i]);
    if (argv) LocalFree(argv);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(pyw, cmd, NULL, NULL, FALSE, 0, NULL, root, &si, &pi)) {
        MessageBoxW(NULL, L"Failed to launch python\\pythonw.exe -m installer.gui.",
                    L"MTF Recomp Setup", MB_ICONERROR | MB_OK);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
