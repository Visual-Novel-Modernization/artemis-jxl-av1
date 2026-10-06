// 32-bit injection launcher. Starts the game suspended, injects
// artemis_jxl.dll with a remote LoadLibraryW, then resumes -- so the hooks are
// in place before any game code runs, and DLL search order never comes into it.
//
//   launcher.exe [game exe] [--dll <path>] [--extra-dll <path>]... [-- game args]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "target.h"

#define MAX_EXTRA 4

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

static bool ReadAt(HANDLE f, DWORD off, void *buf, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = off;
    if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(f, buf, len, &got, NULL) && got == len;
}

// RVA -> file offset via the section table (they are not always equal).
static DWORD RvaToOffset(HANDLE f, const IMAGE_DOS_HEADER *dos,
                         const IMAGE_NT_HEADERS *nt, DWORD rva) {
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh;
        DWORD at = dos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                   nt->FileHeader.SizeOfOptionalHeader +
                   (DWORD)i * sizeof(IMAGE_SECTION_HEADER);
        if (!ReadAt(f, at, &sh, sizeof(sh))) return 0;
        DWORD span = sh.Misc.VirtualSize > sh.SizeOfRawData
                         ? sh.Misc.VirtualSize : sh.SizeOfRawData;
        if (rva >= sh.VirtualAddress && rva - sh.VirtualAddress < span)
            return sh.PointerToRawData + (rva - sh.VirtualAddress);
    }
    return 0;
}

// Is this file the engine we can hook? Checked by content, not by name.
bool ExeLooksLikeGame(const wchar_t *path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    if (ReadAt(f, 0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        dos.e_lfanew >= (LONG)sizeof(IMAGE_DOS_HEADER) &&
        ReadAt(f, (DWORD)dos.e_lfanew, &nt, sizeof(nt)) &&
        nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        ARTEMIS_RVA_PNG_LOADER + ARTEMIS_PROLOGUE_LEN <=
            nt.OptionalHeader.SizeOfImage) {
        DWORD off = RvaToOffset(f, &dos, &nt, ARTEMIS_RVA_PNG_LOADER);
        if (off) {
            unsigned char pro[ARTEMIS_PROLOGUE_LEN];
            if (ReadAt(f, off, pro, sizeof(pro)))
                ok = memcmp(pro, ARTEMIS_PROLOGUE, sizeof(pro)) == 0;
        }
    }
    CloseHandle(f);
    return ok;
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
        if (ExeLooksLikeGame(out)) {
            found = true;
            break;
        }
        Log(L"skipping %ls -- not this engine", fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!found) out[0] = 0;
    return found;
}

static void ResolvePath(const wchar_t *dir, wchar_t *path, size_t pathCount) {
    if (!path[0]) return;
    if ((path[0] == L'\\' || path[0] == L'/') ||
        (path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')))
        return;
    wchar_t joined[MAX_PATH * 2];
    _snwprintf(joined, sizeof(joined) / sizeof(joined[0]), L"%ls\\%ls", dir, path);
    wcsncpy(path, joined, pathCount - 1);
    path[pathCount - 1] = 0;
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
    FARPROC fn = k32 ? GetProcAddress(k32, "LoadLibraryW") : NULL;
    if (!fn) {
        Log(L"LoadLibraryW not found");
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
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

    // Extra DLLs injected, in order, into the same suspended process.
    wchar_t extraDlls[MAX_EXTRA][MAX_PATH];
    int extraCount = 0;

    wchar_t extraArgs[1024] = {0};
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            wcscpy(dllPath, argv[++i]);
        } else if (_wcsicmp(argv[i], L"--extra-dll") == 0 && i + 1 < argc) {
            ++i;
            if (extraCount < MAX_EXTRA) {
                wcscpy(extraDlls[extraCount++], argv[i]);
            } else {
                Log(L"--extra-dll: dropping %ls, limit is %d", argv[i], MAX_EXTRA);
            }
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

    ResolvePath(dir, dllPath, MAX_PATH);
    for (int i = 0; i < extraCount; ++i) ResolvePath(dir, extraDlls[i], MAX_PATH);

    Log(L"--- launcher starting ---");
    Log(L"dir: %ls", dir);

    if (!gamePath[0] && !FindGameExe(dir, gamePath, MAX_PATH)) {
        Fail(L"No matching Artemis game executable found in this directory.\n\n"
             L"Candidates are checked by PE content at RVA 0x20BE50, so crash\n"
             L"handlers, patch launchers, and other bundled tools are skipped.\n\n"
             L"You can also specify the original game executable explicitly:\n"
             L"    launcher.exe <game exe path>");
        return 1;
    }
    if (!ExeLooksLikeGame(gamePath)) {
        Log(L"refusing to start %ls -- not the expected Artemis binary", gamePath);
        Fail(L"The specified executable is not the expected Artemis game binary.\n\n"
             L"The five-byte prologue at RVA 0x20BE50 did not match.");
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
    for (int i = 0; i < extraCount; ++i) Log(L"extra: %ls", extraDlls[i]);

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
        for (int i = 0; i < extraCount; ++i) {
            if (GetFileAttributesW(extraDlls[i]) == INVALID_FILE_ATTRIBUTES) {
                Log(L"--extra-dll: %ls not found, skipping", extraDlls[i]);
                continue;
            }
            // Non-fatal: the main hook is already in.
            if (InjectDll(pi.hProcess, extraDlls[i]))
                Log(L"--extra-dll: injected %ls", extraDlls[i]);
            else
                Log(L"--extra-dll: %ls failed to load, continuing", extraDlls[i]);
        }
    }

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
