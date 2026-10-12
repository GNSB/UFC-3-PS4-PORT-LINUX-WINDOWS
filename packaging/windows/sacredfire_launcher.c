/* SacredFire.exe (Windows): what run.sh does on Linux, as a native program so players need no
 * shell and no installed Python. From its own folder it
 *   1. finds the game folder (--game <dir>, BB_GAME_DIR, or the folder remembered in
 *      <data>\sacredfire.ini; without one, a folder picker; the game check decides),
 *   2. runs the preparation scripts with the bundled Python (python\python.exe, the official
 *      embeddable package): game_check, prepare, link_libc, link_modules, gen_import_names
 *      (with the bundled aerolib.inl: nothing is downloaded), content_profile, patches,
 *   3. starts bin\sacredfire-game.exe with the same arguments run.sh passes, BB_RESTART_COMMAND
 *      set so the in-game restart relaunches this program, and waits for it.
 * Data (generated files, saves, shader cache, bbport.ini): %LOCALAPPDATA%\SacredFire unless
 * BB_DATA_DIR says otherwise; a portable layout puts everything beside the executable with
 * BB_DATA_DIR=. Everything is UTF-8 inside (the manifest selects the UTF-8 code page) and
 * UTF-16 at the Win32 calls. Exit code: the game's. GPL-2.0-or-later. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t root[MAX_PATH * 4];   /* the folder this program runs from */
static wchar_t data[MAX_PATH * 4];   /* data directory */
static wchar_t game[MAX_PATH * 4];   /* the game folder */
static wchar_t out[MAX_PATH * 4];    /* <data>\out */
static wchar_t python[MAX_PATH * 4];

static void fail(const wchar_t *message) {
    fwprintf(stderr, L"%ls\n", message);
    if (!GetEnvironmentVariableW(L"BB_NO_DIALOGS", NULL, 0)) MessageBoxW(NULL, message, L"SacredFire", MB_OK | MB_ICONERROR);
    exit(2);
}
static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
static void join(wchar_t *out_path, size_t n, const wchar_t *a, const wchar_t *b) { _snwprintf(out_path, n, L"%ls\\%ls", a, b); out_path[n - 1] = 0; }
static void ensure_directory(const wchar_t *path) {
    wchar_t buffer[MAX_PATH * 4];
    wcsncpy(buffer, path, MAX_PATH * 4 - 1); buffer[MAX_PATH * 4 - 1] = 0;
    for (wchar_t *p = buffer + 3; *p; ++p) if (*p == L'\\') { *p = 0; CreateDirectoryW(buffer, NULL); *p = L'\\'; }
    CreateDirectoryW(buffer, NULL);
}
/* Quotes one argument the way CommandLineToArgvW undoes it. */
static void append_argument(wchar_t *line, size_t n, const wchar_t *argument) {
    size_t at = wcslen(line);
    if (at && at < n - 1) line[at++] = L' ';
    if (at < n - 1) line[at++] = L'"';
    size_t backslashes = 0;
    for (const wchar_t *p = argument; *p && at < n - 8; ++p) {
        if (*p == L'\\') { ++backslashes; line[at++] = *p; continue; }
        if (*p == L'"') { for (size_t i = 0; i <= backslashes; ++i) line[at++] = L'\\'; }
        backslashes = 0;
        line[at++] = *p;
    }
    for (size_t i = 0; i < backslashes && at < n - 3; ++i) line[at++] = L'\\';
    line[at++] = L'"'; line[at] = 0;
}
/* Runs a command line, waits, returns its exit code (or 127 when it cannot start). stdout/stderr
 * are inherited: the log is this console. */
/* The log file of this run (<data>\logs\<time>.log): everything the game and the scripts print,
 * next to the console, so a crash leaves its report even when the console closes with it. */
static FILE *log_file;
static void log_write(const char *text, size_t n) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)n, &written, NULL);
    if (log_file) { fwrite(text, 1, n, log_file); fflush(log_file); }
}
static DWORD run(wchar_t *line, int capture_fd, char *captured, size_t capture_size) {
    STARTUPINFOW startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
                            .hStdInput = GetStdHandle(STD_INPUT_HANDLE), .hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE),
                            .hStdError = GetStdHandle(STD_ERROR_HANDLE)};
    HANDLE read_end = NULL, write_end = NULL;
    const int piped = capture_fd || log_file != NULL;
    if (piped) {
        SECURITY_ATTRIBUTES inheritable = {.nLength = sizeof(inheritable), .bInheritHandle = TRUE};
        if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) fail(L"CreatePipe failed");
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
        startup.hStdOutput = write_end;
        if (!capture_fd) startup.hStdError = write_end; /* the log gets both streams, in order */
    }
    PROCESS_INFORMATION process;
    { char head[32768]; int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, head + 2, sizeof(head) - 4, NULL, NULL);
      if (n > 0) { head[0] = '>'; head[1] = ' '; head[n + 1] = '\n'; log_write(head, (size_t)n + 2); } }
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, root, &startup, &process)) {
        fwprintf(stderr, L"cannot start (error %lu)\n", GetLastError());
        return 127;
    }
    CloseHandle(process.hThread);
    if (piped) {
        CloseHandle(write_end);
        size_t total = 0; DWORD got;
        char chunk[16384];
        for (;;) {
            if (capture_fd) {
                if (total + 1 >= capture_size || !ReadFile(read_end, captured + total, (DWORD)(capture_size - 1 - total), &got, NULL) || !got) break;
                total += got;
            } else {
                if (!ReadFile(read_end, chunk, sizeof(chunk), &got, NULL) || !got) break;
                log_write(chunk, got);
            }
        }
        if (capture_fd) captured[total] = 0;
        CloseHandle(read_end);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code;
}
/* python -E -s scripts\<name> args...: no PYTHON* variables, no user site-packages; imports come
 * from the bundled standard library (python313.zip beside python.exe) and the script's own
 * folder (the scripts import each other), never from the current directory. */
static DWORD script(const wchar_t *name, const wchar_t *const *arguments, int count, char *captured, size_t capture_size) {
    static wchar_t line[32768];
    wchar_t path[MAX_PATH * 4];
    line[0] = 0;
    append_argument(line, sizeof(line) / sizeof(*line), python);
    append_argument(line, sizeof(line) / sizeof(*line), L"-E"); append_argument(line, sizeof(line) / sizeof(*line), L"-s");
    join(path, MAX_PATH * 4, root, L"scripts"); join(path, MAX_PATH * 4, path, name);
    append_argument(line, sizeof(line) / sizeof(*line), path);
    for (int i = 0; i < count; ++i) append_argument(line, sizeof(line) / sizeof(*line), arguments[i]);
    return run(line, captured != NULL, captured, capture_size);
}
static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }
static void utf8_to_wide(const char *in, wchar_t *out_text, int n) { int k = MultiByteToWideChar(CP_UTF8, 0, in, -1, out_text, n); if (k <= 0) out_text[0] = 0; }
/* <data>\sacredfire.ini: "game=<folder>" lines. */
static void remember_game(void) {
    wchar_t path[MAX_PATH * 4]; join(path, MAX_PATH * 4, data, L"sacredfire.ini");
    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"game=%ls\n", game); fclose(f);
}
static int remembered_game(void) {
    wchar_t path[MAX_PATH * 4]; join(path, MAX_PATH * 4, data, L"sacredfire.ini");
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return 0;
    wchar_t line[MAX_PATH * 4];
    int found = 0;
    while (fgetws(line, MAX_PATH * 4, f)) {
        size_t n = wcslen(line); while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r')) line[--n] = 0;
        if (!wcsncmp(line, L"game=", 5)) { wcsncpy(game, line + 5, MAX_PATH * 4 - 1); found = 1; }
    }
    fclose(f);
    return found && game[0];
}
static int pick_game_folder(void) {
    if (GetEnvironmentVariableW(L"BB_NO_DIALOGS", NULL, 0)) return 0;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 0;
    IFileOpenDialog *dialog = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog))) {
        DWORD options = 0;
        dialog->lpVtbl->GetOptions(dialog, &options);
        dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->lpVtbl->SetTitle(dialog, L"SacredFire: choose the game folder (the one with eboot.bin, with the 1.21 update merged in)");
        if (SUCCEEDED(dialog->lpVtbl->Show(dialog, NULL))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(dialog->lpVtbl->GetResult(dialog, &item))) {
                PWSTR chosen = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &chosen))) {
                    wcsncpy(game, chosen, MAX_PATH * 4 - 1); CoTaskMemFree(chosen); ok = 1;
                }
                item->lpVtbl->Release(item);
            }
        }
        dialog->lpVtbl->Release(dialog);
    }
    CoUninitialize();
    return ok;
}
static const wchar_t *env_or(const wchar_t *name, const wchar_t *fallback) {
    static wchar_t values[8][1024]; static int next;
    wchar_t *value = values[next++ % 8];
    DWORD n = GetEnvironmentVariableW(name, value, 1024);
    return n && n < 1024 ? value : fallback;
}
static int env_is(const wchar_t *name, const wchar_t *expected) { return !wcscmp(env_or(name, L""), expected); }
/* The value of a key in bbport.ini ("key=value" lines, UTF-8), or NULL. */
static const char *ini_value(const wchar_t *path, const char *key) {
    static char value[512];
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    char line[1024]; size_t k = strlen(key);
    const char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, key, k) && line[k] == '=') { snprintf(value, sizeof(value), "%s", line + k + 1); result = value; break; }
    }
    fclose(f);
    return result;
}

int wmain(int argc, wchar_t **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    GetModuleFileNameW(NULL, root, MAX_PATH * 4);
    *wcsrchr(root, L'\\') = 0;
    SetCurrentDirectoryW(root);
    /* --after <pid>: an in-game restart waits for the previous game process (its GPU device). */
    int first = 1;
    if (argc >= 3 && !wcscmp(argv[1], L"--after")) {
        HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(argv[2], NULL, 10));
        if (previous) { WaitForSingleObject(previous, 30000); CloseHandle(previous); }
        first = 3;
    }
    const wchar_t *game_argument = NULL;
    int extra_count = 0; const wchar_t *extra[64];
    for (int i = first; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--game") && i + 1 < argc) game_argument = argv[++i];
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            wprintf(L"SacredFire.exe [--game <folder>] [game options]\n  BB_DATA_DIR, BB_GAME_DIR, BB_NEO=1, BB_FPS, BB_TIMEOUT and the other variables of run.sh apply.\n");
            return 0;
        } else if (extra_count < 64) extra[extra_count++] = argv[i];
    }
    /* Data directory. */
    const wchar_t *data_env = env_or(L"BB_DATA_DIR", NULL);
    if (data_env) { if (!GetFullPathNameW(data_env, MAX_PATH * 4, data, NULL)) fail(L"BB_DATA_DIR is not a usable path"); }
    else {
        PWSTR local = NULL;
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) fail(L"no %LOCALAPPDATA%");
        join(data, MAX_PATH * 4, local, L"SacredFire"); CoTaskMemFree(local);
    }
    ensure_directory(data);
    join(out, MAX_PATH * 4, data, L"out"); ensure_directory(out);
    wchar_t user_dir[MAX_PATH * 4], config[MAX_PATH * 4], game_exe[MAX_PATH * 4], caps_exe[MAX_PATH * 4];
    join(user_dir, MAX_PATH * 4, data, L"user"); ensure_directory(user_dir);
    join(config, MAX_PATH * 4, data, L"bbport.ini");
    wchar_t logs[MAX_PATH * 4], log_path[MAX_PATH * 4];
    join(logs, MAX_PATH * 4, data, L"logs"); ensure_directory(logs);
    {
        SYSTEMTIME now; GetLocalTime(&now);
        _snwprintf(log_path, MAX_PATH * 4, L"%ls\\%04u%02u%02u_%02u%02u%02u.log", logs, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        log_file = _wfopen(log_path, L"wb");
        wprintf(L"Log: %ls\n", log_file ? log_path : L"(not writable)");
    }
    join(python, MAX_PATH * 4, root, L"python\\python.exe");
    join(game_exe, MAX_PATH * 4, root, L"bin\\sacredfire-game.exe");
    join(caps_exe, MAX_PATH * 4, root, L"bin\\bb-gpu-capabilities.exe");
    if (!exists(python)) fail(L"python\\python.exe is missing next to SacredFire.exe: the package is incomplete.");
    if (!exists(game_exe)) fail(L"bin\\sacredfire-game.exe is missing next to SacredFire.exe: the package is incomplete.");
    SetEnvironmentVariableW(L"BB_CONFIG", env_or(L"BB_CONFIG", config));
    SetEnvironmentVariableW(L"BB_USER_DIR", env_or(L"BB_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"BB_GPU_USER_DIR", env_or(L"BB_GPU_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    /* The game folder: argument, environment, remembered, picked. The game check decides. */
    if (game_argument) wcsncpy(game, game_argument, MAX_PATH * 4 - 1);
    else if (env_or(L"BB_GAME_DIR", NULL)) wcsncpy(game, env_or(L"BB_GAME_DIR", L""), MAX_PATH * 4 - 1);
    else if (!remembered_game() && !pick_game_folder()) fail(L"No game folder: start with --game <folder> or set BB_GAME_DIR.");
    wchar_t eboot[MAX_PATH * 4]; join(eboot, MAX_PATH * 4, game, L"eboot.bin");
    if (!exists(eboot)) { wchar_t m[MAX_PATH * 4 + 64]; _snwprintf(m, MAX_PATH * 4 + 64, L"No eboot.bin in %ls", game); fail(m); }
    char profile[256] = "";
    if (!env_is(L"BB_SKIP_GAME_CHECK", L"1")) {
        const wchar_t *args[] = {game};
        if (script(L"game_check.py", args, 1, profile, sizeof(profile))) fail(L"The game check failed (see the message above): the folder is not Crash Team Racing Nitro-Fueled CUSA13795 with the 1.21 update merged in.");
        trim(profile);
    }
    if (!profile[0]) snprintf(profile, sizeof(profile), "ctr");
    wchar_t profile_w[64]; utf8_to_wide(profile, profile_w, 64);
    SetEnvironmentVariableW(L"BB_GAME_PROFILE", profile_w);
    wprintf(L"Game profile: %ls\n", profile_w);
    remember_game();
    /* Preparation (scripts/ as run.sh runs them; mods overlays need symlinks and are not applied here). */
    const wchar_t *prepare[] = {game, L"--out", out};
    if (script(L"prepare.py", prepare, 3, NULL, 0)) fail(L"prepare.py failed");
    if (script(L"link_libc.py", prepare, 3, NULL, 0)) fail(L"link_libc.py failed");
    const wchar_t *link_args[] = {game, L"--out", out, L"--modules", L"libc.prx", L"libSceFios2.prx", L"libSceNpToolkit2.prx"};
    if (script(L"link_modules.py", link_args, 7, NULL, 0)) fail(L"link_modules.py failed");
    wchar_t aerolib[MAX_PATH * 4]; join(aerolib, MAX_PATH * 4, root, L"share\\aerolib.inl");
    const wchar_t *names_args[] = {L"--out", out, L"--aerolib", aerolib};
    {
        /* tools/gen_import_names.py lives in tools/, not scripts/. */
        static wchar_t line[32768]; wchar_t path[MAX_PATH * 4];
        line[0] = 0;
        append_argument(line, 32768, python); append_argument(line, 32768, L"-E"); append_argument(line, 32768, L"-s");
        join(path, MAX_PATH * 4, root, L"tools\\gen_import_names.py"); append_argument(line, 32768, path);
        for (int i = 0; i < 4; ++i) append_argument(line, 32768, names_args[i]);
        if (run(line, 0, NULL, 0)) fail(L"gen_import_names.py failed");
    }
    const wchar_t *content_args[] = {game, L"--out", out, L"--sku", env_or(L"BB_CONTENT_SKU", L"full")};
    if (script(L"content_profile.py", content_args, 5, NULL, 0)) fail(L"content_profile.py failed");
    wchar_t patches_dir[MAX_PATH * 4], patches_config[MAX_PATH * 4];
    join(patches_dir, MAX_PATH * 4, root, L"patches"); join(patches_config, MAX_PATH * 4, data, L"patches.json");
    const wchar_t *patch_args[] = {L"--patches-dir", env_or(L"BB_PATCHES_DIR", patches_dir), L"--patches-config", env_or(L"BB_PATCHES_CONFIG", patches_config),
                                   L"--app-version", L"01.21", L"--title-id", L"CUSA13795", L"--out", out, L"--settings", env_or(L"BB_CONFIG", config)};
    if (script(L"patches.py", patch_args, 12, NULL, 0)) fail(L"patches.py failed");
    /* The count of guest TCB loads the linker rewrote (link.json fs_loads_patched): the game
     * executable cross-checks its own scan against it. */
    wchar_t tls_loads[32] = L"-1";
    {
        wchar_t link_json[MAX_PATH * 4]; join(link_json, MAX_PATH * 4, out, L"link.json");
        FILE *f = _wfopen(link_json, L"rb");
        if (f) {
            static char text[1 << 20]; size_t n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f);
            const char *key = strstr(text, "\"fs_loads_patched\"");
            if (key && (key = strchr(key, ':'))) _snwprintf(tls_loads, 32, L"%ld", strtol(key + 1, NULL, 10));
        }
    }
    /* Defaults run.sh sets. */
    if (!env_or(L"BB_NEO", NULL)) SetEnvironmentVariableW(L"BB_NEO", L"0");
    wprintf(L"PS4 Pro mode: %ls (BB_NEO=1 turns it on)\n", env_or(L"BB_NEO", L"0"));
    if (!env_or(L"BB_PREUPLOAD", NULL)) SetEnvironmentVariableW(L"BB_PREUPLOAD", L"1");
    if (!env_or(L"BB_COPY_GPU_BUFFERS", NULL)) SetEnvironmentVariableW(L"BB_COPY_GPU_BUFFERS", L"1");
    if (!env_or(L"BB_GPU_WRITE_TWINS", NULL)) SetEnvironmentVariableW(L"BB_GPU_WRITE_TWINS", L"1");
    if (!env_or(L"BB_GPU_WRITE_TWINS_MAX", NULL)) SetEnvironmentVariableW(L"BB_GPU_WRITE_TWINS_MAX", L"65536");
    SetEnvironmentVariableW(L"BB_UFFD", L"0");            /* Linux only */
    SetEnvironmentVariableW(L"BB_GUEST_IN_PLACE", L"0");  /* dma-buf memory model: Linux only */
    if (!env_or(L"BB_VBLANK_HZ", NULL)) {
        const char *sixty = ini_value(env_or(L"BB_CONFIG", config), "patch_60fps");
        const wchar_t *fps = env_or(L"BB_FPS", L"30");
        SetEnvironmentVariableW(L"BB_VBLANK_HZ", (sixty && !strcmp(sixty, "1")) ? L"480" : !wcscmp(fps, L"uncap") ? L"480" : !wcscmp(fps, L"90") ? L"90" : L"60");
    }
    if (env_is(L"BB_CLEAR_SHADER_CACHE", L"1")) {
        wchar_t cache[MAX_PATH * 4]; join(cache, MAX_PATH * 4, env_or(L"BB_USER_DIR", user_dir), L"cache\\CUSA13795");
        wchar_t command[MAX_PATH * 4 + 64]; _snwprintf(command, MAX_PATH * 4 + 64, L"cmd.exe /c rd /s /q \"%ls\"", cache);
        run(command, 0, NULL, 0);
        wprintf(L"Shader cache: cleared for this launch (BB_CLEAR_SHADER_CACHE=1)\n");
    }
    /* The restart command: this program again, after this game process is gone. */
    {
        static wchar_t restart[32768]; wchar_t self[MAX_PATH * 4], pid[32];
        GetModuleFileNameW(NULL, self, MAX_PATH * 4);
        restart[0] = 0;
        append_argument(restart, 32768, self); append_argument(restart, 32768, L"--after");
        _snwprintf(pid, 32, L"%lu", GetCurrentProcessId()); append_argument(restart, 32768, pid);
        append_argument(restart, 32768, L"--game"); append_argument(restart, 32768, game);
        for (int i = 0; i < extra_count; ++i) append_argument(restart, 32768, extra[i]);
        SetEnvironmentVariableW(L"BB_RESTART_COMMAND", restart);
    }
    /* The game. */
    static wchar_t line[32768]; wchar_t path[MAX_PATH * 4];
    line[0] = 0;
    append_argument(line, 32768, game_exe);
    join(path, MAX_PATH * 4, out, L"boot-linked.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--import-names"); join(path, MAX_PATH * 4, out, L"import_names.txt"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--content-profile"); join(path, MAX_PATH * 4, out, L"content.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--patches"); join(path, MAX_PATH * 4, out, L"patches.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--app0"); append_argument(line, 32768, game);
    append_argument(line, 32768, L"--user"); append_argument(line, 32768, env_or(L"BB_USER_DIR", user_dir));
    append_argument(line, 32768, L"--timeout"); append_argument(line, 32768, env_or(L"BB_TIMEOUT", L"0"));
    append_argument(line, 32768, L"--tls-loads"); append_argument(line, 32768, tls_loads);
    for (int i = 0; i < extra_count; ++i) append_argument(line, 32768, extra[i]);
    char version[64] = "unknown version";
    { wchar_t v[MAX_PATH * 4]; join(v, MAX_PATH * 4, root, L"VERSION"); FILE *f = _wfopen(v, L"rb"); if (f) { if (fgets(version, sizeof(version), f)) trim(version); fclose(f); } }
    wprintf(L"SacredFire %hs\n", version);
    { char head[128]; int n = snprintf(head, sizeof(head), "SacredFire %s (Windows)\n", version); if (log_file && n > 0) fwrite(head, 1, (size_t)n, log_file); }
    (void)caps_exe;
    const DWORD code = run(line, 0, NULL, 0);
    char tail[160];
    int n = snprintf(tail, sizeof(tail), "sacredfire-game.exe exited with code %lu%s\n", code,
                     code == 0 ? "" : code == 3 ? " (a fault; the report is above)" : code == 142 ? " (watchdog)" : code == 23 ? " (GPU library assertion)" : "");
    if (n > 0) log_write(tail, (size_t)n);
    if (log_file) fclose(log_file);
    DWORD owners[2];
    if (code != 0 && GetConsoleProcessList(owners, 2) <= 1) { /* our own console window: it would close with us */
        wprintf(L"The log is %ls. Press Enter to close.\n", log_path);
        fflush(stdout); getwchar();
    }
    return (int)code;
}
