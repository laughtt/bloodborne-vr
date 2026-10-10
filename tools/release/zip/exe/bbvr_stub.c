/* SPDX-License-Identifier: GPL-2.0-or-later */
/* BloodborneVR.exe: a tiny launcher stub (GUI subsystem, so it opens no console of its own).
 * It runs   python\python.exe launcher\bbvr_launcher.py <your arguments>   from its own folder in a
 * NEW console window (the launcher prints its progress there and may ask questions), waits for it and
 * returns its exit code. Without the bundled python folder it falls back to `py -3` and `python` on
 * PATH, like the old BloodborneVR.bat. BBVR_LAUNCHER_PYTHON / BBVR_LAUNCHER_SCRIPT override the two
 * paths (used by the tests). BBVR_EXE=1 is set for the child so the launcher knows it has its own
 * console and keeps it open when something went wrong. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define LIMIT 32768

/* Appends one argument, quoted so that CommandLineToArgvW / the C runtime read it back unchanged. */
static void append_quoted(wchar_t *line, size_t cap, const wchar_t *arg)
{
    size_t n = wcslen(line);
    int plain = *arg && !wcspbrk(arg, L" \t\n\v\"");
    if (n + 3 >= cap)
        return;
    if (n)
        line[n++] = L' ';
    if (plain) {
        for (; *arg && n + 1 < cap; arg++)
            line[n++] = *arg;
        line[n] = 0;
        return;
    }
    line[n++] = L'"';
    for (;; arg++) {
        size_t slashes = 0;
        while (*arg == L'\\') {
            slashes++;
            arg++;
        }
        if (!*arg) {  /* backslashes before the closing quote are doubled */
            for (size_t i = 0; i < slashes * 2 && n + 3 < cap; i++)
                line[n++] = L'\\';
            break;
        }
        if (*arg == L'"') {  /* backslashes before a quote are doubled, the quote is escaped */
            for (size_t i = 0; i < slashes * 2 && n + 3 < cap; i++)
                line[n++] = L'\\';
            if (n + 3 < cap) {
                line[n++] = L'\\';
                line[n++] = L'"';
            }
        } else {
            for (size_t i = 0; i < slashes && n + 3 < cap; i++)
                line[n++] = L'\\';
            if (n + 3 < cap)
                line[n++] = *arg;
        }
    }
    line[n++] = L'"';
    line[n] = 0;
}

static int file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void fail(const wchar_t *text)
{
    MessageBoxW(NULL, text, L"Bloodborne VR", MB_OK | MB_ICONERROR);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR cmdline, int show)
{
    (void)instance, (void)previous, (void)cmdline, (void)show;
    static wchar_t dir[LIMIT], python[LIMIT], script[LIMIT], found[LIMIT], line[LIMIT], text[LIMIT];
    static wchar_t override_python[LIMIT], override_script[LIMIT];
    DWORD length = GetModuleFileNameW(NULL, dir, LIMIT);
    if (length == 0 || length >= LIMIT) {
        fail(L"Cannot find the folder of BloodborneVR.exe.");
        return 1;
    }
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;

    DWORD a = GetEnvironmentVariableW(L"BBVR_LAUNCHER_PYTHON", override_python, LIMIT);
    DWORD b = GetEnvironmentVariableW(L"BBVR_LAUNCHER_SCRIPT", override_script, LIMIT);
    if (b && b < LIMIT)
        wcscpy(script, override_script);
    else
        swprintf(script, LIMIT, L"%ls\\launcher\\bbvr_launcher.py", dir);
    if (!file_exists(script)) {
        swprintf(text, LIMIT, L"The launcher script was not found:\n%ls\n\nUnzip the whole download and start "
                 L"BloodborneVR.exe from the unzipped folder (do not copy the exe on its own).", script);
        fail(text);
        return 1;
    }

    /* Which interpreter: the bundled one, else the Python launcher, else python on PATH. */
    const wchar_t *extra = L"";
    if (a && a < LIMIT) {
        wcscpy(python, override_python);
    } else {
        swprintf(python, LIMIT, L"%ls\\python\\python.exe", dir);
        if (!file_exists(python)) {
            if (SearchPathW(NULL, L"py.exe", NULL, LIMIT, found, NULL)) {
                wcscpy(python, found);
                extra = L"-3";
            } else if (SearchPathW(NULL, L"python.exe", NULL, LIMIT, found, NULL)) {
                wcscpy(python, found);
            } else {
                fail(L"Python was not found.\n\nUse the full download (it has a python folder next to "
                     L"BloodborneVR.exe) or install Python 3.10 or newer.");
                return 1;
            }
        }
    }

    line[0] = 0;
    append_quoted(line, LIMIT, python);
    if (*extra)
        append_quoted(line, LIMIT, extra);
    append_quoted(line, LIMIT, script);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++)
        append_quoted(line, LIMIT, argv[i]);
    if (argv)
        LocalFree(argv);

    SetEnvironmentVariableW(L"BBVR_EXE", L"1");
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(python, line, NULL, NULL, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, NULL,
                        dir, &startup, &process)) {
        swprintf(text, LIMIT, L"Could not start Python (error %lu):\n%ls", GetLastError(), python);
        fail(text);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
