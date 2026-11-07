// GTASA-Savestates.exe: starts gta_sa.exe with savestate.dll loaded, without changing any file of the game.
//
// The game is created suspended. Its first thread would call the exe's entry point after the Windows loader has
// loaded the static imports; the launcher points that call at a small stub in the game's memory instead, which loads
// savestate.dll and then jumps to the real entry point. So the DLL is in place before the game's C runtime creates
// its heap, exactly as with an ASI loader, and nothing is written to disk.
//
//   GTASA-Savestates.exe [path\to\gta_sa.exe] [--wait] [game arguments...]
//   Without a path: [Launcher] Game= in savestate.ini, else gta_sa.exe next to the launcher, else a file dialog
//   (the choice is saved to the ini). --wait: stay until the game exits and return its exit code.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cstdint>
#include <cwchar>
#include <string>

// Per-game build: GTASA-Savestates.exe (default), GTA3-Savestates.exe, GTAVC-Savestates.exe.
#ifndef LAUNCHER_TITLE
#define LAUNCHER_TITLE L"GTA SA Savestates"
#define LAUNCHER_EXE L"gta_sa.exe"
#endif
static const wchar_t* kTitle = LAUNCHER_TITLE;

static void Fail(const std::wstring& msg) { MessageBoxW(nullptr, msg.c_str(), kTitle, MB_ICONERROR); }

static std::wstring Dir(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }

static bool Exists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring AskForGame() {
    wchar_t file[MAX_PATH] = LAUNCHER_EXE;
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Game (" LAUNCHER_EXE L")\0" LAUNCHER_EXE L"\0Programs (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Where is " LAUNCHER_EXE L"?";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? file : L"";
}

static std::wstring Quote(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring r = L"\"";
    for (wchar_t c : s) { if (c == L'"') r += L'\\'; r += c; }
    return r + L"\"";
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring dir = Dir(self), dll = dir + L"\\savestate.dll", ini = dir + L"\\savestate.ini";
    if (!Exists(dll)) { Fail(L"savestate.dll is missing next to the launcher:\n" + dll); return 1; }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring game, args;
    bool wait = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--wait") wait = true;
        else if (game.empty() && a.size() > 4 && _wcsicmp(a.c_str() + a.size() - 4, L".exe") == 0) game = a;
        else args += L" " + Quote(a);
    }
    LocalFree(argv);
    if (game.empty()) {
        wchar_t buf[MAX_PATH] = {};
        GetPrivateProfileStringW(L"Launcher", L"Game", L"", buf, MAX_PATH, ini.c_str());
        game = buf;
    }
    if (game.empty() && Exists(dir + L"\\" LAUNCHER_EXE)) game = dir + L"\\" LAUNCHER_EXE;
    if (game.empty() || !Exists(game)) {
        game = AskForGame();
        if (game.empty()) return 1;
        WritePrivateProfileStringW(L"Launcher", L"Game", game.c_str(), ini.c_str());
    }

    std::wstring cmd = Quote(game) + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(game.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, Dir(game).c_str(),
                        &si, &pi)) {
        Fail(L"Cannot start " + game + L" (error " + std::to_wstring(GetLastError()) + L").");
        return 1;
    }
    auto abort = [&](const std::wstring& why) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Fail(why);
        return 1;
    };

    // Initial context of a new x86 process: EAX = the exe's entry point, EBX = its PEB.
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
    if (!GetThreadContext(pi.hThread, &ctx)) return abort(L"Cannot read the game's start-up state.");
    uint32_t base = 0, lfanew = 0, entryRva = 0;
    SIZE_T n = 0;
    ReadProcessMemory(pi.hProcess, (void*)(uintptr_t)(ctx.Ebx + 8), &base, 4, &n);
    ReadProcessMemory(pi.hProcess, (void*)(uintptr_t)(base + 0x3C), &lfanew, 4, &n);
    ReadProcessMemory(pi.hProcess, (void*)(uintptr_t)(base + lfanew + 0x28), &entryRva, 4, &n);
    const uint32_t entry = base + entryRva;
    if (!base || ctx.Eax != entry) return abort(L"Unexpected start-up state of " + game + L" (is it a 32-bit " LAUNCHER_EXE L"?).");

    // Stub page: code at +0, "done" flag at +0x80, HMODULE at +0x84, error at +0x88, DLL path at +0x100.
    auto* page = (uint8_t*)VirtualAllocEx(pi.hProcess, nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!page) return abort(L"Cannot prepare the game process.");
    const uint32_t p = (uint32_t)(uintptr_t)page;
    // kernel32 sits at the same address in every 32-bit process of this boot
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const uint32_t loadLibrary = (uint32_t)(uintptr_t)GetProcAddress(k32, "LoadLibraryW");
    const uint32_t getLastError = (uint32_t)(uintptr_t)GetProcAddress(k32, "GetLastError");
    uint8_t code[64];
    int c = 0;
    auto b = [&](uint8_t v) { code[c++] = v; };
    auto d = [&](uint32_t v) { memcpy(code + c, &v, 4); c += 4; };
    b(0x68); d(p + 0x100);                    // push path
    b(0xB8); d(loadLibrary);                  // mov eax, LoadLibraryW
    b(0xFF); b(0xD0);                         // call eax
    b(0xA3); d(p + 0x84);                     // mov [module], eax
    b(0xB8); d(getLastError);                 // mov eax, GetLastError
    b(0xFF); b(0xD0);                         // call eax
    b(0xA3); d(p + 0x88);                     // mov [error], eax
    b(0xC7); b(0x05); d(p + 0x80); d(1);      // mov dword [done], 1
    b(0xB8); d(entry);                        // mov eax, entry point
    b(0xFF); b(0xE0);                         // jmp eax (same stack: return address and PEB argument)
    bool ok = WriteProcessMemory(pi.hProcess, page, code, c, &n) &&
              WriteProcessMemory(pi.hProcess, page + 0x100, dll.c_str(), (dll.size() + 1) * 2, &n);
    FlushInstructionCache(pi.hProcess, page, c);
    ctx.Eax = p;
    if (!ok || !SetThreadContext(pi.hThread, &ctx)) return abort(L"Cannot prepare the game process.");
    ResumeThread(pi.hThread);

    // Wait for the stub to have run, then report a DLL that did not load (the game runs on without savestates).
    uint32_t state[3] = {};
    for (int i = 0; i < 300 && !state[0]; i++) {
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        ReadProcessMemory(pi.hProcess, page + 0x80, state, sizeof(state), &n);
    }
    if (state[0] && !state[1])
        Fail(L"savestate.dll could not be loaded into the game (error " + std::to_wstring(state[2]) +
             L"). The game runs without savestates.");
    DWORD exitCode = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exitCode;
}
