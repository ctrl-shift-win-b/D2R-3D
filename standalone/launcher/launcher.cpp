// d2r-3d.exe - starts D2R.exe and loads d2r-3d.dll into it.
//
// Put d2r-3d.exe and d2r-3d.dll into the Diablo II Resurrected folder and start
// d2r-3d.exe instead of D2R.exe.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace {

FILE* g_log = nullptr;

void Log(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vwprintf(fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfwprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); }
}

std::wstring SelfDir() {
    wchar_t buf[MAX_PATH * 4];
    std::wstring p(buf, GetModuleFileNameW(nullptr, buf, MAX_PATH * 4));
    return p.substr(0, p.find_last_of(L"\\/"));
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ModuleLoaded(DWORD pid, const std::wstring& path) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof me};
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = _wcsicmp(me.szExePath, path.c_str()) == 0;
    CloseHandle(snap);
    return found;
}

int Fail(PROCESS_INFORMATION* pi, const wchar_t* what) {
    Log(L"%s failed (error %lu)\n", what, GetLastError());
    if (pi && pi->hProcess) { TerminateProcess(pi->hProcess, 1); CloseHandle(pi->hThread); CloseHandle(pi->hProcess); }
    return 1;
}

}  // namespace

int wmain() {
    const std::wstring dir = SelfDir();
    const std::wstring exe = dir + L"\\D2R.exe", dll = dir + L"\\d2r-3d.dll";
    _wfopen_s(&g_log, (dir + L"\\d2r-3d-launcher.log").c_str(), L"w");

    if (!FileExists(exe)) { Log(L"not found: %s (put d2r-3d.exe into the game folder)\n", exe.c_str()); return 1; }
    if (!FileExists(dll)) { Log(L"not found: %s\n", dll.c_str()); return 1; }

    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi))
        return Fail(nullptr, L"CreateProcess");
    Log(L"started D2R.exe (pid %lu), suspended\n", pi.dwProcessId);

    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr)) return Fail(&pi, L"writing the DLL path");
    auto loadLibrary = (PAPCFUNC)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!QueueUserAPC(loadLibrary, pi.hThread, (ULONG_PTR)remote)) return Fail(&pi, L"QueueUserAPC");
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    Log(L"queued LoadLibraryW(%s) and resumed the game\n", dll.c_str());

    bool loaded = false;
    for (int i = 0; i < 150 && !loaded; ++i) {   // up to 15 s
        loaded = ModuleLoaded(pi.dwProcessId, dll);
        if (!loaded && WaitForSingleObject(pi.hProcess, 100) != WAIT_TIMEOUT) break;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) Log(L"D2R.exe exited early with code 0x%08lX\n", code);
    Log(loaded ? L"d2r-3d.dll loaded - press F12 in a game\n" : L"d2r-3d.dll did NOT load\n");
    CloseHandle(pi.hProcess);
    if (g_log) fclose(g_log);
    return loaded ? 0 : 1;
}
