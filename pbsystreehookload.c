/* pbsystreehookload.c - loader for pbsystreehook.dll (prototype)
 *
 * Copyright (c) 2026 Bruce Armstrong
 * Licensed under the MIT License. See the LICENSE file in this directory for the
 * full text (https://opensource.org/licenses/MIT).
 *
 * Finds the running PowerBuilder IDE frame window, loads pbsystreehook.dll into THIS
 * process, and asks it to install a thread-level hook on the IDE's GUI thread.
 * The OS maps pbsystreehook.dll into the IDE as messages flow. The hooks stay active
 * while this loader is running; closing it (OK on the message box) removes them.
 *
 * Build (MinGW-w64, 32-bit):
 *   i686-w64-mingw32-gcc -O2 -o pbsystreehookload.exe pbsystreehookload.c -luser32
 */

#include <windows.h>
#include <stdio.h>

typedef BOOL (*InstallFn)(DWORD);
typedef void (*RemoveFn)(void);

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;

    HWND frame = FindWindowW(L"PBFRAME", NULL);
    if (!frame) {
        MessageBoxW(NULL, L"PowerBuilder IDE (PBFRAME window) not found. Is it running?",
                    L"pbsystreehook loader", MB_OK | MB_ICONERROR);
        return 1;
    }
    DWORD tid = GetWindowThreadProcessId(frame, NULL);

    /* Load the DLL from the loader's own folder. */
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) wcscpy(slash + 1, L"pbsystreehook.dll"); else wcscpy(path, L"pbsystreehook.dll");

    HMODULE dll = LoadLibraryW(path);
    if (!dll) {
        MessageBoxW(NULL, L"Could not load pbsystreehook.dll (must sit next to this exe).",
                    L"pbsystreehook loader", MB_OK | MB_ICONERROR);
        return 1;
    }

    InstallFn Install = (InstallFn)GetProcAddress(dll, "InstallHook");
    RemoveFn  Remove  = (RemoveFn) GetProcAddress(dll, "RemoveHook");
    if (!Install || !Remove) {
        MessageBoxW(NULL, L"pbsystreehook.dll is missing InstallHook/RemoveHook exports.",
                    L"pbsystreehook loader", MB_OK | MB_ICONERROR);
        FreeLibrary(dll);
        return 1;
    }

    if (!Install(tid)) {
        MessageBoxW(NULL, L"SetWindowsHookEx failed. Is this loader the same bitness (32-bit) "
                          L"and running as the same user as the IDE?",
                    L"pbsystreehook loader", MB_OK | MB_ICONERROR);
        FreeLibrary(dll);
        return 1;
    }

    MessageBoxW(NULL, L"Hook installed. Right-click an object in the IDE's System Tree.\n\n"
                      L"Click OK here to remove the hook and exit.",
                L"pbsystreehook loader", MB_OK | MB_ICONINFORMATION);

    Remove();
    FreeLibrary(dll);
    return 0;
}
