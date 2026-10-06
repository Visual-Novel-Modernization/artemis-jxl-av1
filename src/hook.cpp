// hooks the engine's PNG loader (sub_60BE50) so it also reads JXL.
// finding that function is written up in docs/TECHNICAL.md;
// the stack swap itself is in thunk.S.
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <vector>
#include <jxl/decode.h>

#include "jxl2png.h"
#include "target.h"

extern "C" void PngLoaderThunk(void);
extern "C" void *__cdecl JxlMaybeSwapStream(void *stream);
extern "C" {
void *g_trampEntry = NULL;                         // read by thunk.S
}
// .data isn't executable -- jumping there took a DEP fault
static unsigned char *g_tramp = NULL;
static const int TRAMP_SIZE = 64;

static char g_logPath[MAX_PATH] = {0};
static CRITICAL_SECTION g_logCs;
static bool g_logReady = false;
static bool g_verbose = false;
static bool g_enabled = true;

static void LogInit(HMODULE h) {
    GetModuleFileNameA(h, g_logPath, MAX_PATH);
    char *p = strrchr(g_logPath, '\\');
    if (p) strcpy(p + 1, "artemis_jxl.log");
    InitializeCriticalSection(&g_logCs);
    g_logReady = true;
}

static void Log(const char *fmt, ...) {
    if (!g_logReady) return;
    EnterCriticalSection(&g_logCs);
    FILE *f = fopen(g_logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

// same file, tagged so the AV1 side is distinguishable
void Av1MftLog(const char *fmt, ...) {
    if (!g_logReady) return;
    EnterCriticalSection(&g_logCs);
    FILE *f = fopen(g_logPath, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] [AV1MFT] ", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

extern bool RegisterAv1Mft();

// MinGW's mfapi.h may not define these
#ifndef MF_VERSION
#define MF_VERSION 0x00020070
#endif
#ifndef MFSTARTUP_LITE
#define MFSTARTUP_LITE 1
#endif

// its own thread -- MFStartup under the loader lock can deadlock
static DWORD WINAPI Av1InitThread(LPVOID) {
    // failing here just means the main thread got there first
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    Av1MftLog("CoInitializeEx hr=0x%08lX", (unsigned long)hrCo);

    HMODULE h = LoadLibraryW(L"mfplat.dll");
    if (!h) {
        Av1MftLog("failed to load mfplat.dll %lu", GetLastError());
        return 0;
    }
    typedef HRESULT(WINAPI * PFN_MFStartup)(ULONG, DWORD);
    PFN_MFStartup pfn = (PFN_MFStartup)GetProcAddress(h, "MFStartup");
    if (pfn) {
        HRESULT hr = pfn(MF_VERSION, MFSTARTUP_LITE);
        Av1MftLog("MFStartup hr=0x%08lX", (unsigned long)hr);
    } else {
        Av1MftLog("MFStartup not found");
    }
    RegisterAv1Mft();
    return 0;
}

// engine calls these as (*(vtable+N))(self, ...): ECX = self, rest on the stack
static inline int StreamRead(void *st, void *buf, unsigned n) {
    void **vt = *(void ***)st;
    typedef int(__fastcall * F)(void *, void *, void *, unsigned);
    return ((F)vt[5])(st, NULL, buf, n);
}
static inline int StreamSeek(void *st, unsigned off, unsigned whence) {
    void **vt = *(void ***)st;
    typedef int(__fastcall * F)(void *, void *, unsigned, unsigned);
    return ((F)vt[6])(st, NULL, off, whence);
}
static inline int StreamSize(void *st) {
    void **vt = *(void ***)st;
    typedef int(__fastcall * F)(void *, void *);
    return ((F)vt[8])(st, NULL);
}

struct ShimStream {
    void **vtbl;  // must sit at offset 0
    const unsigned char *data;
    unsigned size;
    unsigned pos;
};

static int __fastcall ShimRead(ShimStream *s, void *edx, void *buf, unsigned n) {
    if (s->pos >= s->size) return 0;
    unsigned avail = s->size - s->pos;
    if (n > avail) n = avail;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return (int)n;
}

static int __fastcall ShimSeek(ShimStream *s, void *edx, unsigned off,
                               unsigned whence) {
    switch (whence) {
        case 0: s->pos = off; break;
        case 1: s->pos += off; break;
        case 2: s->pos = (off <= s->size) ? s->size - off : 0; break;
        default: break;
    }
    if (s->pos > s->size) s->pos = s->size;
    return (int)s->pos;
}

static int __fastcall ShimSize(ShimStream *s, void *edx) {
    return (int)s->size;
}

static void *g_shimVtbl[9] = {
    NULL, NULL, NULL, NULL, NULL,
    (void *)&ShimRead,
    (void *)&ShimSeek,
    NULL,
    (void *)&ShimSize
};

struct TlsSlot {
    unsigned char *buf;
    size_t cap;
    ShimStream shim;
};
static __thread TlsSlot *t_slot = NULL;

static volatile long g_statJxl = 0;
static volatile long g_statPng = 0;
static volatile long g_statFail = 0;
static volatile long g_calls = 0;

// thunk.S calls this; non-zero means "swap a4 for the returned stream"
extern "C" void *__cdecl JxlMaybeSwapStream(void *stream) {
    long n = InterlockedIncrement(&g_calls);
    if (n == 1) Log("PNG loader call #1 (stream=%p) - thunk is live", stream);
    if (!g_enabled || !stream) return NULL;

    unsigned char head[16];
    memset(head, 0, sizeof(head));
    StreamSeek(stream, 0, 0);
    int got = StreamRead(stream, head, (unsigned)sizeof(head));

    if (got < 2 || !JxlIsJxl(head, (size_t)got)) {
        InterlockedIncrement(&g_statPng);
        return NULL;
    }

    long idx = InterlockedIncrement(&g_statJxl);

    int total = StreamSize(stream);
    if (total <= 0 || total > (int)(768u * 1024 * 1024)) {
        Log("JXL #%ld: odd stream size %d, falling back", idx, total);
        InterlockedIncrement(&g_statFail);
        StreamSeek(stream, 0, 0);
        return NULL;
    }

    if (!t_slot) {
        t_slot = (TlsSlot *)calloc(1, sizeof(TlsSlot));
        if (!t_slot) {
            InterlockedIncrement(&g_statFail);
            StreamSeek(stream, 0, 0);
            return NULL;
        }
    }

    std::vector<unsigned char> jxl;
    try {
        jxl.resize((size_t)total);
    } catch (...) {
        Log("JXL #%ld: out of memory (%d bytes)", idx, total);
        InterlockedIncrement(&g_statFail);
        StreamSeek(stream, 0, 0);
        return NULL;
    }

    StreamSeek(stream, 0, 0);
    int rd = StreamRead(stream, jxl.data(), (unsigned)total);
    if (rd != total) {
        Log("JXL #%ld: short read %d/%d, falling back", idx, rd, total);
        InterlockedIncrement(&g_statFail);
        StreamSeek(stream, 0, 0);
        return NULL;
    }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    std::vector<unsigned char> png;
    int w = 0, h = 0, ch = 0;
    char err[256] = {0};
    bool ok = false;
    try {
        ok = JxlToPng(jxl.data(), jxl.size(), png, &w, &h, &ch, err, sizeof(err));
    } catch (...) {
        strcpy(err, "C++ exception during transcode");
    }

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

    if (!ok) {
        InterlockedIncrement(&g_statFail);
        Log("JXL #%ld: transcode failed (%s), falling back to the original path", idx, err);
        StreamSeek(stream, 0, 0);
        return NULL;
    }

    if (png.size() > t_slot->cap) {
        unsigned char *nb = (unsigned char *)realloc(t_slot->buf, png.size());
        if (!nb) {
            InterlockedIncrement(&g_statFail);
            Log("JXL #%ld: realloc failed (%u bytes)", idx, (unsigned)png.size());
            StreamSeek(stream, 0, 0);
            return NULL;
        }
        t_slot->buf = nb;
        t_slot->cap = png.size();
    }
    memcpy(t_slot->buf, png.data(), png.size());

    t_slot->shim.vtbl = g_shimVtbl;
    t_slot->shim.data = t_slot->buf;
    t_slot->shim.size = (unsigned)png.size();
    t_slot->shim.pos = 0;

    if (g_verbose || idx <= 40)
        Log("JXL #%ld: %d B -> PNG %d B  %dx%d ch=%d  %.1f ms", idx,
            (int)jxl.size(), (int)png.size(), w, h, ch, ms);

    return &t_slot->shim;
}

// Reading past the module end faults inside DllMain, which the loader
// reports as a plain "load failed". Check the RVA first.
static bool RvaInsideModule(HMODULE mod, unsigned long rva, size_t len) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS *nt =
        (const IMAGE_NT_HEADERS *)((const unsigned char *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return (unsigned long long)rva + len <= nt->OptionalHeader.SizeOfImage;
}

// Even in range, the page may be uncommitted or unreadable.
static bool TargetIsMapped(const unsigned char *p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) return false;
    const unsigned char *end =
        (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    return end >= p + ARTEMIS_PROLOGUE_LEN;
}

static bool InstallHook() {
    HMODULE base = GetModuleHandleW(NULL);

    if (!RvaInsideModule(base, ARTEMIS_RVA_PNG_LOADER, ARTEMIS_PROLOGUE_LEN)) {
        Log("HOOK: not this engine -- RVA 0x%lX is past the end of the image at "
            "%p, nothing installed",
            ARTEMIS_RVA_PNG_LOADER, (void *)base);
        return false;
    }
    unsigned char *target = (unsigned char *)base + ARTEMIS_RVA_PNG_LOADER;

    if (!TargetIsMapped(target)) {
        Log("HOOK: 0x%lX is not readable in %p, nothing installed",
            ARTEMIS_RVA_PNG_LOADER, (void *)base);
        return false;
    }

    if (memcmp(target, ARTEMIS_PROLOGUE, ARTEMIS_PROLOGUE_LEN) != 0) {
        Log("HOOK: prologue mismatch at %p: %02X %02X %02X %02X %02X", target,
            target[0], target[1], target[2], target[3], target[4]);
        return false;
    }

    g_tramp = (unsigned char *)VirtualAlloc(NULL, TRAMP_SIZE,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    if (!g_tramp) {
        Log("HOOK: trampoline VirtualAlloc failed %lu", GetLastError());
        return false;
    }
    memcpy(g_tramp, target, ARTEMIS_PROLOGUE_LEN);
    g_tramp[ARTEMIS_PROLOGUE_LEN] = 0xE9;
    *(int *)(g_tramp + ARTEMIS_PROLOGUE_LEN + 1) =
        (int)((target + ARTEMIS_PROLOGUE_LEN) -
              (g_tramp + ARTEMIS_PROLOGUE_LEN + 5));
    g_trampEntry = g_tramp;
    FlushInstructionCache(GetCurrentProcess(), g_tramp, TRAMP_SIZE);

    DWORD old = 0;
    if (!VirtualProtect(target, ARTEMIS_PROLOGUE_LEN, PAGE_EXECUTE_READWRITE,
                        &old)) {
        Log("HOOK: VirtualProtect failed %lu", GetLastError());
        return false;
    }
    unsigned char patch[ARTEMIS_PROLOGUE_LEN];
    patch[0] = 0xE9;
    *(int *)(patch + 1) = (int)((unsigned char *)&PngLoaderThunk - (target + 5));
    memcpy(target, patch, ARTEMIS_PROLOGUE_LEN);
    DWORD tmp = 0;
    VirtualProtect(target, ARTEMIS_PROLOGUE_LEN, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), target, ARTEMIS_PROLOGUE_LEN);

    Log("HOOK: installed at %p (base=%p, thunk=%p, tramp=%p)", target, base,
        (void *)&PngLoaderThunk, (void *)g_tramp);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        LogInit(hInst);

        char buf[16];
        if (GetEnvironmentVariableA("ARTEMIS_JXL", buf, sizeof(buf)) && buf[0] == '0')
            g_enabled = false;
        if (GetEnvironmentVariableA("ARTEMIS_JXL_VERBOSE", buf, sizeof(buf)) && buf[0] == '1')
            g_verbose = true;

        Log("=== artemis_jxl injected (pid=%lu, enabled=%d) ===",
            GetCurrentProcessId(), (int)g_enabled);
        Log("libjxl statically linked, JxlDecoderVersion=0x%08X", (unsigned)JxlDecoderVersion());

        // Not the target engine: don't register an AV1 decoder.
        if (g_enabled && !InstallHook()) return TRUE;

        HANDLE th = CreateThread(NULL, 0, Av1InitThread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("=== artemis_jxl unloading (JXL=%ld PNG=%ld FAIL=%ld) ===", g_statJxl,
            g_statPng, g_statFail);
    }
    return TRUE;
}
