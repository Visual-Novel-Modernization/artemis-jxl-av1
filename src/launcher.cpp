// 32-bit injection launcher. Starts the game suspended, injects
// artemis_jxl.dll with a remote LoadLibraryW, then resumes -- so the hooks are
// in place before any game code runs, and DLL search order never comes into it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static wchar_t g_log[MAX_PATH] = {0};

static void Log(const wchar_t *fmt, ...) {
    if (!g_log[0]) return;
    FILE *f = _wfopen(g_log, L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void Fail(const wchar_t *msg) {
    Log(L"ERROR: %ls", msg);
    MessageBoxW(NULL, msg, L"Artemis JXL Launcher", MB_ICONERROR | MB_OK);
}

static bool FindGameExe(const wchar_t *dir, wchar_t *out, size_t outCount) {
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\*.exe", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, L"launcher.exe") == 0) continue;
        _snwprintf(out, outCount, L"%ls\\%ls", dir, fd.cFileName);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static bool InjectDll(HANDLE hProc, const wchar_t *dllPath) {
    size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);

    void *remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        Log(L"VirtualAllocEx failed %lu", GetLastError());
        return false;
    }
    if (!WriteProcessMemory(hProc, remote, dllPath, bytes, NULL)) {
        Log(L"WriteProcessMemory failed %lu", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC fn = GetProcAddress(k32, "LoadLibraryW");
    if (!fn) {
        Log(L"LoadLibraryW not found");
        return false;
    }

    HANDLE th = CreateRemoteThread(hProc, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)fn, remote, 0, NULL);
    if (!th) {
        Log(L"CreateRemoteThread failed %lu", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return false;
    }

    DWORD wr = WaitForSingleObject(th, 30000);
    DWORD modBase = 0;
    GetExitCodeThread(th, &modBase);
    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

    if (wr != WAIT_OBJECT_0) {
        Log(L"injection thread timed out");
        return false;
    }
    if (modBase == 0) {
        Log(L"remote LoadLibraryW returned 0 (DLL load failed)");
        return false;
    }
    Log(L"injection succeeded, remote module base = 0x%08lX", modBase);
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, self, MAX_PATH);

    wchar_t dir[MAX_PATH] = {0};
    wcscpy(dir, self);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    _snwprintf(g_log, MAX_PATH, L"%ls\\launcher.log", dir);

    // launcher.exe [game exe] [--dll <path>] [-- extra game args]
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    wchar_t gamePath[MAX_PATH] = {0};
    wchar_t dllPath[MAX_PATH] = {0};
    _snwprintf(dllPath, MAX_PATH, L"%ls\\artemis_jxl.dll", dir);

    wchar_t extraArgs[1024] = {0};
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            wcscpy(dllPath, argv[++i]);
        } else if (_wcsicmp(argv[i], L"--") == 0) {
            for (int j = i + 1; j < argc; ++j) {
                wcscat(extraArgs, L" ");
                wcscat(extraArgs, argv[j]);
            }
            break;
        } else if (argv[i][0] != L'-' && !gamePath[0]) {
            wcscpy(gamePath, argv[i]);
        } else {
            wcscat(extraArgs, L" ");
            wcscat(extraArgs, argv[i]);
        }
    }
    if (argv) LocalFree(argv);

    Log(L"--- launcher starting ---");
    Log(L"dir: %ls", dir);

    if (!gamePath[0] && !FindGameExe(dir, gamePath, MAX_PATH)) {
        Fail(L"No game executable (.exe) found in the launcher's directory.\n\n"
             L"Put launcher.exe and artemis_jxl.dll next to the game, or pass\n"
             L"the path explicitly: launcher.exe <game exe path>");
        return 1;
    }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wchar_t m[MAX_PATH + 128];
        _snwprintf(m, MAX_PATH + 128,
                   L"Injection DLL not found:\n%ls\n\nMake sure artemis_jxl.dll sits next to launcher.exe.",
                   dllPath);
        Fail(m);
        return 2;
    }

    Log(L"game: %ls", gamePath);
    Log(L"DLL : %ls", dllPath);

    wchar_t cmdline[2048];
    _snwprintf(cmdline, 2048, L"\"%ls\"%ls", gamePath, extraArgs);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(gamePath, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        Log(L"CreateProcessW failed %lu", GetLastError());
        Fail(L"Could not start the game process.");
        return 3;
    }
    Log(L"created suspended, pid=%lu", pi.dwProcessId);

    bool ok = InjectDll(pi.hProcess, dllPath);

    if (ok) {
        ResumeThread(pi.hThread);
        Log(L"main thread resumed, game is running");
    } else {
        TerminateProcess(pi.hProcess, 1);
        Fail(L"Injection failed. See launcher.log and artemis_jxl.log for details.");
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok ? 0 : 4;
}
