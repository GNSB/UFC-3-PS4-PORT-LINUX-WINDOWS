/* SacredFire.exe (Windows): the launcher, a native Win32 program (the Linux one is GTK4/Python,
 * which cannot be produced from Linux for the bundled Python; this one is built by the cross
 * toolchain with no dependency beyond Windows). GPL-2.0-or-later.
 *
 * Window: the game folder (picker; the game check decides), the controller for port 1, the
 * game patches (written to bbport.ini as the in-game menu does), PS4 Pro mode, fullscreen,
 * the developer switches (frame statistics, Vulkan validation, clear the shader cache, menu
 * trace), Play/Stop, the log of the run, and buttons for the logs and data folders. Settings
 * are kept in <data>\sacredfire.ini.
 * Play does what run.sh does on Linux: the preparation scripts with the bundled Python
 * (python\python.exe, the official embeddable package; game_check, prepare, link_libc,
 * link_modules, gen_import_names with the bundled aerolib.inl, content_profile, patches),
 * then bin\sacredfire-game.exe with the same arguments and BB_RESTART_COMMAND for the in-game
 * restart. Everything printed goes to the window, to <data>\logs\<time>.log, and to the console
 * when started from one.
 * Command line: `--play` starts the game at once with the saved settings and no window (Steam
 * shortcuts); `--game <folder>` sets the folder; other arguments go to the game.
 * Data (generated files, saves, shader cache, bbport.ini, logs): %LOCALAPPDATA%\SacredFire, or
 * BB_DATA_DIR; BB_DATA_DIR=. keeps everything beside the executable. UTF-8 inside (the manifest
 * selects the UTF-8 code page), UTF-16 at the Win32 calls. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PATHW (MAX_PATH * 4)
static wchar_t root[PATHW];   /* the folder this program runs from */
static wchar_t data[PATHW];   /* data directory */
static wchar_t python[PATHW], game_exe[PATHW], caps_exe[PATHW];

/* ---- settings (<data>\sacredfire.ini) ---- */
typedef struct {
    wchar_t game[PATHW], gamepad[128];
    int neo, fullscreen, clear_cache, validation, frame_stats, menu_trace;
    int patch_60fps, patch_no_motion_blur, patch_pro_1080p; /* mirrors of bbport.ini */
} Settings;
static Settings settings;

static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
static void join(wchar_t *out_path, size_t n, const wchar_t *a, const wchar_t *b) {
    wchar_t joined[PATHW];
    _snwprintf(joined, PATHW, L"%ls\\%ls", a, b); joined[PATHW - 1] = 0;
    wcsncpy(out_path, joined, n - 1); out_path[n - 1] = 0;
}
static void ensure_directory(const wchar_t *path) {
    wchar_t buffer[PATHW];
    wcsncpy(buffer, path, PATHW - 1); buffer[PATHW - 1] = 0;
    for (wchar_t *p = buffer + 3; *p; ++p) if (*p == L'\\') { *p = 0; CreateDirectoryW(buffer, NULL); *p = L'\\'; }
    CreateDirectoryW(buffer, NULL);
}
static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }
static void trimw(wchar_t *s) { size_t n = wcslen(s); while (n && (s[n - 1] == L'\n' || s[n - 1] == L'\r' || s[n - 1] == L' ')) s[--n] = 0; }
static const wchar_t *env_or(const wchar_t *name, const wchar_t *fallback) {
    static wchar_t values[8][1024]; static int next;
    wchar_t *value = values[next++ % 8];
    DWORD n = GetEnvironmentVariableW(name, value, 1024);
    return n && n < 1024 ? value : fallback;
}
static void read_version(char *version, size_t n) {
    wchar_t path[PATHW]; join(path, PATHW, root, L"VERSION");
    snprintf(version, n, "unknown version");
    FILE *f = _wfopen(path, L"rb");
    if (f) { if (fgets(version, (int)n, f)) trim(version); fclose(f); }
}
static void settings_path(wchar_t *out_path) { join(out_path, PATHW, data, L"sacredfire.ini"); }
static void settings_load(void) {
    wchar_t path[PATHW]; settings_path(path);
    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[PATHW];
    while (fgetws(line, PATHW, f)) {
        trimw(line);
        wchar_t *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0; const wchar_t *value = eq + 1;
        if (!wcscmp(line, L"game")) wcsncpy(settings.game, value, PATHW - 1);
        else if (!wcscmp(line, L"gamepad")) wcsncpy(settings.gamepad, value, 127);
        else if (!wcscmp(line, L"neo")) settings.neo = _wtoi(value);
        else if (!wcscmp(line, L"fullscreen")) settings.fullscreen = _wtoi(value);
        else if (!wcscmp(line, L"clear_cache")) settings.clear_cache = _wtoi(value);
        else if (!wcscmp(line, L"validation")) settings.validation = _wtoi(value);
        else if (!wcscmp(line, L"frame_stats")) settings.frame_stats = _wtoi(value);
        else if (!wcscmp(line, L"menu_trace")) settings.menu_trace = _wtoi(value);
    }
    fclose(f);
}
static void settings_save(void) {
    wchar_t path[PATHW]; settings_path(path);
    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"game=%ls\ngamepad=%ls\nneo=%d\nfullscreen=%d\nclear_cache=%d\nvalidation=%d\nframe_stats=%d\nmenu_trace=%d\n",
             settings.game, settings.gamepad, settings.neo, settings.fullscreen, settings.clear_cache, settings.validation,
             settings.frame_stats, settings.menu_trace);
    fclose(f);
}
/* bbport.ini: "key=value" lines (UTF-8), shared with the in-game menu. */
static void config_path(wchar_t *out_path) {
    const wchar_t *c = env_or(L"BB_CONFIG", NULL);
    if (c) { wcsncpy(out_path, c, PATHW - 1); out_path[PATHW - 1] = 0; } else join(out_path, PATHW, data, L"bbport.ini");
}
static const char *ini_value(const char *key) {
    static char value[512]; wchar_t path[PATHW]; config_path(path);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    char line[1024]; size_t k = strlen(key); const char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, key, k) && line[k] == '=') { snprintf(value, sizeof(value), "%s", line + k + 1); result = value; break; }
    }
    fclose(f);
    return result;
}
static int ini_flag(const char *key) { const char *v = ini_value(key); return v && !strcmp(v, "1"); }
/* Replaces the key's line, or appends it; the file is rewritten whole and swapped in as the
 * in-game menu does. */
static void ini_set(const char *key, const char *value) {
    wchar_t path[PATHW], temp[PATHW]; config_path(path);
    _snwprintf(temp, PATHW, L"%ls.tmp", path); temp[PATHW - 1] = 0;
    FILE *in = _wfopen(path, L"rb"), *out = _wfopen(temp, L"wb");
    if (!out) { if (in) fclose(in); return; }
    int written = 0; char line[1024]; size_t k = strlen(key);
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            char copy[1024]; snprintf(copy, sizeof(copy), "%s", line); trim(copy);
            if (!strncmp(copy, key, k) && copy[k] == '=') { if (!written) fprintf(out, "%s=%s\n", key, value); written = 1; }
            else fputs(line, out);
        }
        fclose(in);
    } else fputs("# bbport settings (in-game menu: Insert / L3+R3)\n", out);
    if (!written) fprintf(out, "%s=%s\n", key, value);
    fclose(out);
    MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
static void patches_load(void) {
    settings.patch_60fps = ini_flag("patch_60fps");
    settings.patch_no_motion_blur = ini_flag("patch_no_motion_blur");
    settings.patch_pro_1080p = ini_flag("patch_pro_1080p");
}

/* ---- the log: window, file, console ---- */
static HWND main_window, log_edit;
static FILE *log_file;
static HANDLE console_out;
#define WM_APP_LOG (WM_APP + 1)   /* lParam: a heap wchar_t* line for the log control */
#define WM_APP_DONE (WM_APP + 2)  /* wParam: the game's exit code */
static void log_write(const char *text, size_t n) {
    if (console_out && console_out != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(console_out, text, (DWORD)n, &written, NULL); }
    if (log_file) { fwrite(text, 1, n, log_file); fflush(log_file); }
    if (main_window) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, text, (int)n, NULL, 0);
        wchar_t *w = malloc(((size_t)wn + 1) * sizeof(wchar_t));
        if (w) {
            MultiByteToWideChar(CP_UTF8, 0, text, (int)n, w, wn); w[wn] = 0;
            if (!PostMessageW(main_window, WM_APP_LOG, 0, (LPARAM)w)) free(w);
        }
    }
}
static void logf(const char *format, ...) {
    char text[4096]; va_list args; va_start(args, format);
    int n = vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (n > 0) log_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
}
static void logw(const wchar_t *format, ...) {
    wchar_t text[4096]; va_list args; va_start(args, format);
    _vsnwprintf(text, 4096, format, args); va_end(args); text[4095] = 0;
    char utf8[16384]; int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n > 1) log_write(utf8, (size_t)n - 1);
}
/* Fatal before the game starts: the message in the log and a box (unless BB_NO_DIALOGS). */
static void fail(const wchar_t *message) {
    logw(L"%ls\n", message);
    if (!GetEnvironmentVariableW(L"BB_NO_DIALOGS", NULL, 0)) MessageBoxW(main_window, message, L"SacredFire", MB_OK | MB_ICONERROR);
}

/* ---- running programs ---- */
static HANDLE game_process; /* the running game (Stop) */
/* Quotes one argument the way CommandLineToArgvW undoes it. */
static void append_argument(wchar_t *line, size_t n, const wchar_t *argument) {
    size_t at = wcslen(line);
    if (at && at < n - 1) line[at++] = L' ';
    if (at < n - 1) line[at++] = L'"';
    size_t backslashes = 0;
    for (const wchar_t *p = argument; *p && at < n - 8; ++p) {
        if (*p == L'\\') { ++backslashes; line[at++] = *p; continue; }
        if (*p == L'"') { for (size_t i = 0; i <= backslashes && at < n - 8; ++i) line[at++] = L'\\'; }
        backslashes = 0;
        line[at++] = *p;
    }
    for (size_t i = 0; i < backslashes && at < n - 3; ++i) line[at++] = L'\\';
    line[at++] = L'"'; line[at] = 0;
}
/* Runs a command line and waits; its output goes to the log, or into `captured` (then at most
 * `wait_ms`). Returns the exit code, 127 when it cannot start. */
static DWORD run(wchar_t *line, char *captured, size_t capture_size, int is_game, DWORD wait_ms) {
    SECURITY_ATTRIBUTES inheritable = {.nLength = sizeof(inheritable), .bInheritHandle = TRUE};
    HANDLE read_end = NULL, write_end = NULL;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) { fail(L"CreatePipe failed"); return 127; }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup = {.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES, .hStdOutput = write_end, .hStdError = write_end};
    PROCESS_INFORMATION process;
    if (!captured) logw(L"> %ls\n", line);
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, root, &startup, &process)) {
        logw(L"cannot start %ls (error %lu)\n", line, GetLastError());
        CloseHandle(read_end); CloseHandle(write_end);
        return 127;
    }
    CloseHandle(process.hThread);
    CloseHandle(write_end);
    if (is_game) game_process = process.hProcess;
    size_t total = 0; DWORD got; char chunk[16384];
    for (;;) {
        if (captured) {
            if (total + 1 >= capture_size || !ReadFile(read_end, captured + total, (DWORD)(capture_size - 1 - total), &got, NULL) || !got) break;
            total += got;
        } else {
            if (!ReadFile(read_end, chunk, sizeof(chunk), &got, NULL) || !got) break;
            log_write(chunk, got);
        }
    }
    if (captured) captured[total] = 0;
    CloseHandle(read_end);
    if (WaitForSingleObject(process.hProcess, wait_ms) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (is_game) game_process = NULL;
    CloseHandle(process.hProcess);
    return code;
}
/* python -E -s <script> args...: the bundled standard library and the script's folder only. */
static DWORD script(const wchar_t *folder, const wchar_t *name, const wchar_t *const *arguments, int count, char *captured, size_t capture_size) {
    static wchar_t line[32768];
    wchar_t path[PATHW];
    line[0] = 0;
    append_argument(line, 32768, python); append_argument(line, 32768, L"-E"); append_argument(line, 32768, L"-s");
    join(path, PATHW, root, folder); join(path, PATHW, path, name);
    append_argument(line, 32768, path);
    for (int i = 0; i < count; ++i) append_argument(line, 32768, arguments[i]);
    if (captured) logw(L"> %ls\n", line);
    return run(line, captured, capture_size, 0, INFINITE);
}

/* ---- the controllers (bb-gpu-capabilities --gamepads: "GUID<tab>name" per line) ---- */
typedef struct { wchar_t guid[64], name[128]; } Gamepad;
static Gamepad gamepads[16]; static int gamepad_count;
static void gamepads_refresh(void) {
    gamepad_count = 0;
    if (!exists(caps_exe)) return;
    static wchar_t line[32768]; line[0] = 0;
    append_argument(line, 32768, caps_exe); append_argument(line, 32768, L"--gamepads");
    static char out[16384];
    if (run(line, out, sizeof(out), 0, 10000) != 0) return;
    for (char *p = out; *p && gamepad_count < 16;) {
        char *end = strchr(p, '\n'); if (end) *end = 0;
        char *tab = strchr(p, '\t');
        if (tab) {
            *tab = 0;
            MultiByteToWideChar(CP_UTF8, 0, p, -1, gamepads[gamepad_count].guid, 64);
            MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, gamepads[gamepad_count].name, 128);
            gamepads[gamepad_count].guid[63] = gamepads[gamepad_count].name[127] = 0;
            trimw(gamepads[gamepad_count].name);
            ++gamepad_count;
        }
        if (!end) break;
        p = end + 1;
    }
}

/* ---- the run: what run.sh does, then the game. Returns the game's exit code. ---- */
static DWORD play(const wchar_t *const *extra, int extra_count) {
    wchar_t out[PATHW], user_dir[PATHW], config[PATHW];
    join(out, PATHW, data, L"out"); ensure_directory(out);
    join(user_dir, PATHW, data, L"user"); ensure_directory(user_dir);
    config_path(config);
    if (!exists(python)) { fail(L"python\\python.exe is missing next to SacredFire.exe: the package is incomplete."); return 2; }
    if (!exists(game_exe)) { fail(L"bin\\sacredfire-game.exe is missing next to SacredFire.exe: the package is incomplete."); return 2; }
    SetEnvironmentVariableW(L"BB_CONFIG", config);
    SetEnvironmentVariableW(L"BB_USER_DIR", env_or(L"BB_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"BB_GPU_USER_DIR", env_or(L"BB_GPU_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    const wchar_t *game = settings.game;
    wchar_t eboot[PATHW]; join(eboot, PATHW, game, L"eboot.bin");
    if (!game[0] || !exists(eboot)) {
        wchar_t m[PATHW + 128];
        _snwprintf(m, PATHW + 128, L"No eboot.bin in \"%ls\": choose the game folder (the one with eboot.bin, with the 1.21 update merged in).", game);
        m[PATHW + 127] = 0; fail(m); return 2;
    }
    char profile[256] = "";
    if (wcscmp(env_or(L"BB_SKIP_GAME_CHECK", L"0"), L"1")) {
        const wchar_t *args[] = {game};
        if (script(L"scripts", L"game_check.py", args, 1, profile, sizeof(profile))) {
            logf("%s", profile);
            fail(L"The game check failed (see the log): the folder is not Crash Team Racing Nitro-Fueled CUSA13795 with the 1.21 update merged in.");
            return 2;
        }
        trim(profile);
    }
    if (!profile[0]) snprintf(profile, sizeof(profile), "ctr");
    wchar_t profile_w[64]; MultiByteToWideChar(CP_UTF8, 0, profile, -1, profile_w, 64); profile_w[63] = 0;
    SetEnvironmentVariableW(L"BB_GAME_PROFILE", profile_w);
    logw(L"Game profile: %ls\n", profile_w);
    /* Preparation (scripts/ as run.sh runs them; mod overlays need symlinks and are not applied here). */
    const wchar_t *prepare[] = {game, L"--out", out};
    if (script(L"scripts", L"prepare.py", prepare, 3, NULL, 0)) { fail(L"prepare.py failed (see the log)"); return 2; }
    if (script(L"scripts", L"link_libc.py", prepare, 3, NULL, 0)) { fail(L"link_libc.py failed (see the log)"); return 2; }
    const wchar_t *link_args[] = {game, L"--out", out, L"--modules", L"libc.prx", L"libSceFios2.prx", L"libSceNpToolkit2.prx"};
    if (script(L"scripts", L"link_modules.py", link_args, 7, NULL, 0)) { fail(L"link_modules.py failed (see the log)"); return 2; }
    wchar_t aerolib[PATHW]; join(aerolib, PATHW, root, L"share\\aerolib.inl");
    const wchar_t *names_args[] = {L"--out", out, L"--aerolib", aerolib};
    if (script(L"tools", L"gen_import_names.py", names_args, 4, NULL, 0)) { fail(L"gen_import_names.py failed (see the log)"); return 2; }
    const wchar_t *content_args[] = {game, L"--out", out, L"--sku", env_or(L"BB_CONTENT_SKU", L"full")};
    if (script(L"scripts", L"content_profile.py", content_args, 5, NULL, 0)) { fail(L"content_profile.py failed (see the log)"); return 2; }
    wchar_t patches_dir[PATHW], patches_config[PATHW];
    join(patches_dir, PATHW, root, L"patches"); join(patches_config, PATHW, data, L"patches.json");
    const wchar_t *patch_args[] = {L"--patches-dir", env_or(L"BB_PATCHES_DIR", patches_dir), L"--patches-config", env_or(L"BB_PATCHES_CONFIG", patches_config),
                                   L"--app-version", L"01.21", L"--title-id", L"CUSA13795", L"--out", out, L"--settings", config};
    if (script(L"scripts", L"patches.py", patch_args, 12, NULL, 0)) { fail(L"patches.py failed (see the log)"); return 2; }
    /* The count of guest TCB loads the linker rewrote (link.json fs_loads_patched): the game
     * executable cross-checks its own scan against it. */
    wchar_t tls_loads[32] = L"-1";
    {
        wchar_t link_json[PATHW]; join(link_json, PATHW, out, L"link.json");
        FILE *f = _wfopen(link_json, L"rb");
        if (f) {
            static char text[1 << 20]; size_t n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f);
            const char *key = strstr(text, "\"fs_loads_patched\"");
            if (key && (key = strchr(key, ':'))) _snwprintf(tls_loads, 32, L"%ld", strtol(key + 1, NULL, 10));
        }
    }
    /* The switches (the window's, or the saved ones with --play) and run.sh's defaults. */
    SetEnvironmentVariableW(L"BB_NEO", settings.neo ? L"1" : L"0");
    logw(L"PS4 Pro mode: %d\n", settings.neo);
    if (settings.fullscreen) SetEnvironmentVariableW(L"BB_FULLSCREEN", L"1");
    if (settings.validation) SetEnvironmentVariableW(L"BB_VK_VALIDATION", L"1");
    if (settings.frame_stats) SetEnvironmentVariableW(L"BB_FRAME_STATS", L"1");
    if (settings.menu_trace) SetEnvironmentVariableW(L"BB_MENU_TRACE", L"1");
    if (settings.gamepad[0]) SetEnvironmentVariableW(L"BB_GAMEPAD", settings.gamepad);
    if (!env_or(L"BB_PREUPLOAD", NULL)) SetEnvironmentVariableW(L"BB_PREUPLOAD", L"1");
    if (!env_or(L"BB_COPY_GPU_BUFFERS", NULL)) SetEnvironmentVariableW(L"BB_COPY_GPU_BUFFERS", L"1");
    if (!env_or(L"BB_GPU_WRITE_TWINS", NULL)) SetEnvironmentVariableW(L"BB_GPU_WRITE_TWINS", L"1");
    if (!env_or(L"BB_GPU_WRITE_TWINS_MAX", NULL)) SetEnvironmentVariableW(L"BB_GPU_WRITE_TWINS_MAX", L"65536");
    SetEnvironmentVariableW(L"BB_UFFD", L"0");            /* Linux only */
    SetEnvironmentVariableW(L"BB_GUEST_IN_PLACE", L"0");  /* dma-buf memory model: Linux only */
    if (!env_or(L"BB_VBLANK_HZ", NULL)) {
        const wchar_t *fps = env_or(L"BB_FPS", L"30");
        SetEnvironmentVariableW(L"BB_VBLANK_HZ", ini_flag("patch_60fps") ? L"480" : !wcscmp(fps, L"uncap") ? L"480" : !wcscmp(fps, L"90") ? L"90" : L"60");
    }
    if (settings.clear_cache || !wcscmp(env_or(L"BB_CLEAR_SHADER_CACHE", L"0"), L"1")) {
        wchar_t cache[PATHW]; join(cache, PATHW, env_or(L"BB_USER_DIR", user_dir), L"cache\\CUSA13795");
        if (exists(cache)) {
            wchar_t command[PATHW + 64]; _snwprintf(command, PATHW + 64, L"cmd.exe /c rd /s /q \"%ls\"", cache); command[PATHW + 63] = 0;
            run(command, NULL, 0, 0, INFINITE);
        }
        logw(L"Shader cache: cleared for this launch\n");
    }
    /* The restart command: this program again (--play), after this game process is gone. */
    {
        static wchar_t restart[32768]; wchar_t self[PATHW], pid[32];
        GetModuleFileNameW(NULL, self, PATHW);
        restart[0] = 0;
        append_argument(restart, 32768, self); append_argument(restart, 32768, L"--after");
        _snwprintf(pid, 32, L"%lu", GetCurrentProcessId()); append_argument(restart, 32768, pid);
        append_argument(restart, 32768, L"--play");
        for (int i = 0; i < extra_count; ++i) append_argument(restart, 32768, extra[i]);
        SetEnvironmentVariableW(L"BB_RESTART_COMMAND", restart);
    }
    static wchar_t line[32768]; wchar_t path[PATHW];
    line[0] = 0;
    append_argument(line, 32768, game_exe);
    join(path, PATHW, out, L"boot-linked.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--import-names"); join(path, PATHW, out, L"import_names.txt"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--content-profile"); join(path, PATHW, out, L"content.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--patches"); join(path, PATHW, out, L"patches.bin"); append_argument(line, 32768, path);
    append_argument(line, 32768, L"--app0"); append_argument(line, 32768, game);
    append_argument(line, 32768, L"--user"); append_argument(line, 32768, env_or(L"BB_USER_DIR", user_dir));
    append_argument(line, 32768, L"--timeout"); append_argument(line, 32768, env_or(L"BB_TIMEOUT", L"0"));
    append_argument(line, 32768, L"--tls-loads"); append_argument(line, 32768, tls_loads);
    for (int i = 0; i < extra_count; ++i) append_argument(line, 32768, extra[i]);
    char version[64]; read_version(version, sizeof(version));
    logf("SacredFire %s (Windows)\n", version);
    const DWORD code = run(line, NULL, 0, 1, INFINITE);
    logf("sacredfire-game.exe exited with code %lu%s\n", code,
         code == 0 ? "" : code == 3 ? " (a fault; the report is above)" : code == 142 ? " (watchdog)" : code == 23 ? " (GPU library assertion)" : "");
    return code;
}

/* ---- the window ---- */
enum { ID_GAME = 100, ID_BROWSE, ID_GAMEPAD, ID_RESCAN, ID_P60, ID_PBLUR, ID_P1080, ID_NEO, ID_FULLSCREEN, ID_CLEAR, ID_VALIDATION, ID_STATS, ID_TRACE,
       ID_PLAY, ID_STOP, ID_LOGS, ID_DATA, ID_LOG, ID_LAST };
static HWND controls[ID_LAST - 100];
static int running;
static float scale = 1.0f;
static HFONT ui_font; /* the system's message font at the system DPI (scales with it) */
static int sx(int v) { return (int)(v * scale + 0.5f); }
static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND h_wnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, sx(x), sx(y), sx(w), sx(h), main_window, (HMENU)(INT_PTR)id,
                                 GetModuleHandleW(NULL), NULL);
    SendMessageW(h_wnd, WM_SETFONT, (WPARAM)ui_font, TRUE);
    if (id >= 100 && id < ID_LAST) controls[id - 100] = h_wnd;
    return h_wnd;
}
static HWND control(int id) { return controls[id - 100]; }
static int checked(int id) { return SendMessageW(control(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void set_checked(int id, int on) { SendMessageW(control(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static void fill_gamepads(void) {
    HWND combo = control(ID_GAMEPAD);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Any gamepad (or the keyboard)");
    int selected = 0;
    for (int i = 0; i < gamepad_count; ++i) {
        wchar_t label[256]; _snwprintf(label, 256, L"%ls  [%.8ls]", gamepads[i].name, gamepads[i].guid); label[255] = 0;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label);
        if (!wcscmp(gamepads[i].guid, settings.gamepad)) selected = i + 1;
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}
/* The window into the settings; patch switches go to bbport.ini when they changed. */
static void read_controls(void) {
    GetWindowTextW(control(ID_GAME), settings.game, PATHW); trimw(settings.game);
    int sel = (int)SendMessageW(control(ID_GAMEPAD), CB_GETCURSEL, 0, 0);
    if (sel > 0 && sel <= gamepad_count) wcsncpy(settings.gamepad, gamepads[sel - 1].guid, 127); else settings.gamepad[0] = 0;
    settings.neo = checked(ID_NEO); settings.fullscreen = checked(ID_FULLSCREEN); settings.clear_cache = checked(ID_CLEAR);
    settings.validation = checked(ID_VALIDATION); settings.frame_stats = checked(ID_STATS); settings.menu_trace = checked(ID_TRACE);
    const int p60 = checked(ID_P60), pblur = checked(ID_PBLUR), p1080 = checked(ID_P1080);
    if (p60 != settings.patch_60fps) ini_set("patch_60fps", p60 ? "1" : "0");
    if (pblur != settings.patch_no_motion_blur) ini_set("patch_no_motion_blur", pblur ? "1" : "0");
    if (p1080 != settings.patch_pro_1080p) ini_set("patch_pro_1080p", p1080 ? "1" : "0");
    settings.patch_60fps = p60; settings.patch_no_motion_blur = pblur; settings.patch_pro_1080p = p1080;
    settings_save();
}
static int pick_folder(wchar_t *out_path) {
    int ok = 0;
    IFileOpenDialog *dialog = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog))) {
        DWORD options = 0;
        dialog->lpVtbl->GetOptions(dialog, &options);
        dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->lpVtbl->SetTitle(dialog, L"The game folder: the one with eboot.bin, with the 1.21 update merged in");
        if (SUCCEEDED(dialog->lpVtbl->Show(dialog, main_window))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(dialog->lpVtbl->GetResult(dialog, &item))) {
                PWSTR chosen = NULL;
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &chosen))) {
                    wcsncpy(out_path, chosen, PATHW - 1); out_path[PATHW - 1] = 0; CoTaskMemFree(chosen); ok = 1;
                }
                item->lpVtbl->Release(item);
            }
        }
        dialog->lpVtbl->Release(dialog);
    }
    return ok;
}
/* <data>\logs\<time>.log for a run of the game; <data>\logs\launcher.log while the window is open. */
static void open_log_file(const wchar_t *fixed_name) {
    wchar_t logs[PATHW], log_path[PATHW];
    join(logs, PATHW, data, L"logs"); ensure_directory(logs);
    SYSTEMTIME now; GetLocalTime(&now);
    if (fixed_name) join(log_path, PATHW, logs, fixed_name);
    else _snwprintf(log_path, PATHW, L"%ls\\%04u%02u%02u_%02u%02u%02u.log", logs, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    log_path[PATHW - 1] = 0;
    if (log_file) fclose(log_file);
    log_file = _wfopen(log_path, fixed_name ? L"ab" : L"wb");
    logw(L"Log: %ls\n", log_file ? log_path : L"(not writable)");
}
static const wchar_t *const *play_extra; static int play_extra_count;
static DWORD WINAPI play_thread(LPVOID p) {
    (void)p;
    const DWORD code = play(play_extra, play_extra_count);
    PostMessageW(main_window, WM_APP_DONE, code, 0);
    return 0;
}
static void set_running(int on) {
    running = on;
    EnableWindow(control(ID_PLAY), !on); EnableWindow(control(ID_STOP), on);
    for (int id = ID_GAME; id <= ID_TRACE; ++id) EnableWindow(control(id), !on);
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == ID_BROWSE) { wchar_t folder[PATHW]; if (pick_folder(folder)) SetWindowTextW(control(ID_GAME), folder); }
        else if (id == ID_RESCAN) { gamepads_refresh(); fill_gamepads(); logw(L"Controllers: %d found\n", gamepad_count); }
        else if (id == ID_PLAY && !running) {
            read_controls();
            open_log_file(NULL);
            set_running(1);
            HANDLE t = CreateThread(NULL, 0, play_thread, NULL, 0, NULL);
            if (t) CloseHandle(t); else { fail(L"CreateThread failed"); set_running(0); }
        } else if (id == ID_STOP && running) {
            if (game_process) { logw(L"Stop: the game process is ended\n"); TerminateProcess(game_process, 1); }
        } else if (id == ID_LOGS || id == ID_DATA) {
            wchar_t folder[PATHW];
            if (id == ID_LOGS) join(folder, PATHW, data, L"logs"); else { wcsncpy(folder, data, PATHW - 1); folder[PATHW - 1] = 0; }
            ensure_directory(folder);
            ShellExecuteW(window, L"open", folder, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_APP_LOG: {
        wchar_t *text = (wchar_t *)lparam;
        /* Edit controls want \r\n. */
        size_t n = wcslen(text), extra = 0;
        for (size_t i = 0; i < n; ++i) if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) ++extra;
        wchar_t *crlf = malloc((n + extra + 1) * sizeof(wchar_t));
        if (crlf) {
            size_t k = 0;
            for (size_t i = 0; i < n; ++i) { if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) crlf[k++] = L'\r'; crlf[k++] = text[i]; }
            crlf[k] = 0;
            if (GetWindowTextLengthW(log_edit) > 400000) SetWindowTextW(log_edit, L"(older lines are in the log file)\r\n");
            SendMessageW(log_edit, EM_SETSEL, (WPARAM)-1, -1);
            SendMessageW(log_edit, EM_REPLACESEL, FALSE, (LPARAM)crlf);
            free(crlf);
        }
        free(text);
        return 0;
    }
    case WM_APP_DONE:
        set_running(0);
        open_log_file(L"launcher.log");
        return 0;
    case WM_CLOSE:
        if (running && game_process) {
            if (MessageBoxW(window, L"The game is running. Close it too?", L"SacredFire", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            TerminateProcess(game_process, 1);
        }
        read_controls();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static int gui(HINSTANCE instance, const wchar_t *const *extra, int extra_count) {
    play_extra = extra; play_extra_count = extra_count;
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    { const UINT dpi = GetDpiForSystem(); scale = (dpi >= 96 && dpi <= 480) ? dpi / 96.0f : 1.0f; }
    {
        NONCLIENTMETRICSW metrics = {.cbSize = sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) ui_font = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!ui_font) ui_font = GetStockObject(DEFAULT_GUI_FONT);
    }
    WNDCLASSW wc = {.lpfnWndProc = window_proc, .hInstance = instance, .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .lpszClassName = L"SacredFireLauncher", .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1))};
    RegisterClassW(&wc);
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    /* The client area is 760x640 at 96 DPI: the window is created, measured, and resized by the
     * difference (the frame metrics of the running system, whatever they are). */
    const int client_w = sx(760), client_h = sx(640);
    main_window = CreateWindowExW(0, wc.lpszClassName, L"SacredFire", style, CW_USEDEFAULT, CW_USEDEFAULT, client_w + 16, client_h + 64,
                                  NULL, NULL, instance, NULL);
    if (!main_window) return 1;
    open_log_file(L"launcher.log");
    {
        RECT window_rect, client_rect;
        GetWindowRect(main_window, &window_rect); GetClientRect(main_window, &client_rect);
        const int dw = (window_rect.right - window_rect.left) - client_rect.right, dh = (window_rect.bottom - window_rect.top) - client_rect.bottom;
        if (dw >= 0 && dw < 200 && dh >= 0 && dh < 200) SetWindowPos(main_window, NULL, 0, 0, client_w + dw, client_h + dh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(main_window, &window_rect); GetClientRect(main_window, &client_rect);
        logw(L"Window: %ld,%ld %ldx%ld, client %ldx%ld, frame +%d+%d, DPI %u, caption %d\n", window_rect.left, window_rect.top,
             window_rect.right - window_rect.left, window_rect.bottom - window_rect.top, client_rect.right, client_rect.bottom, dw, dh,
             GetDpiForWindow(main_window), GetSystemMetrics(SM_CYCAPTION));
    }
    int y = 12;
    make(L"STATIC", L"Game folder (the one with eboot.bin, update 1.21 merged in):", 0, 12, y, 600, 18, 0); y += 20;
    make(L"EDIT", settings.game, WS_BORDER | ES_AUTOHSCROLL, 12, y, 640, 24, ID_GAME);
    make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 660, y, 88, 24, ID_BROWSE); y += 34;
    make(L"STATIC", L"Controller for port 1 (ports 2-4 for split screen: in-game menu, L3+R3 or Insert):", 0, 12, y, 640, 18, 0); y += 20;
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 12, y, 640, 200, ID_GAMEPAD);
    make(L"BUTTON", L"Rescan", BS_PUSHBUTTON, 660, y, 88, 24, ID_RESCAN); y += 34;
    make(L"BUTTON", L"Game patches (by illusion and TroyWarez; applied at start)", BS_GROUPBOX, 12, y, 360, 96, 0);
    make(L"BUTTON", L"60 FPS unlock", BS_AUTOCHECKBOX, 24, y + 20, 330, 20, ID_P60);
    make(L"BUTTON", L"Disable motion blur", BS_AUTOCHECKBOX, 24, y + 44, 330, 20, ID_PBLUR);
    make(L"BUTTON", L"1080p in PS4 Pro mode", BS_AUTOCHECKBOX, 24, y + 68, 330, 20, ID_P1080);
    make(L"BUTTON", L"Display and console", BS_GROUPBOX, 388, y, 360, 96, 0);
    make(L"BUTTON", L"PS4 Pro mode (7 CPUs, 5568 MiB; needed for 1080p)", BS_AUTOCHECKBOX, 400, y + 20, 340, 20, ID_NEO);
    make(L"BUTTON", L"Fullscreen", BS_AUTOCHECKBOX, 400, y + 44, 330, 20, ID_FULLSCREEN);
    make(L"BUTTON", L"Clear the shader cache at start", BS_AUTOCHECKBOX, 400, y + 68, 330, 20, ID_CLEAR); y += 106;
    make(L"BUTTON", L"Developer switches", BS_GROUPBOX, 12, y, 736, 48, 0);
    make(L"BUTTON", L"Frame statistics", BS_AUTOCHECKBOX, 24, y + 20, 160, 20, ID_STATS);
    make(L"BUTTON", L"Vulkan validation layer", BS_AUTOCHECKBOX, 200, y + 20, 200, 20, ID_VALIDATION);
    make(L"BUTTON", L"Menu input trace", BS_AUTOCHECKBOX, 420, y + 20, 200, 20, ID_TRACE); y += 58;
    make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON, 12, y, 120, 30, ID_PLAY);
    make(L"BUTTON", L"Stop", BS_PUSHBUTTON, 140, y, 90, 30, ID_STOP);
    make(L"BUTTON", L"Open logs folder", BS_PUSHBUTTON, 480, y, 130, 30, ID_LOGS);
    make(L"BUTTON", L"Open data folder", BS_PUSHBUTTON, 618, y, 130, 30, ID_DATA); y += 40;
    log_edit = make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 12, y, 736, 640 - y - 12, ID_LOG);
    SendMessageW(log_edit, EM_SETLIMITTEXT, 0, 0);
    set_checked(ID_P60, settings.patch_60fps); set_checked(ID_PBLUR, settings.patch_no_motion_blur); set_checked(ID_P1080, settings.patch_pro_1080p);
    set_checked(ID_NEO, settings.neo); set_checked(ID_FULLSCREEN, settings.fullscreen); set_checked(ID_CLEAR, settings.clear_cache);
    set_checked(ID_STATS, settings.frame_stats); set_checked(ID_VALIDATION, settings.validation); set_checked(ID_TRACE, settings.menu_trace);
    gamepads_refresh(); fill_gamepads();
    set_running(0);
    char version[64]; read_version(version, sizeof(version));
    logf("SacredFire %s (Windows). Data: ", version); logw(L"%ls\n", data);
    logw(L"Controllers: %d found; window scale %d%%\n", gamepad_count, (int)(scale * 100));
    ShowWindow(main_window, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(main_window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line; (void)show;
    /* Started from a console (cmd, a script): its output stays there as well. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) console_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(NULL, root, PATHW);
    { wchar_t *slash = wcsrchr(root, L'\\'); if (slash) *slash = 0; }
    SetCurrentDirectoryW(root);
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int first = 1, play_now = 0;
    if (argc >= 3 && !wcscmp(argv[1], L"--after")) { /* an in-game restart waits for the previous game process (its GPU device) */
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)wcstoul(argv[2], NULL, 10));
        if (old) { WaitForSingleObject(old, 30000); CloseHandle(old); }
        first = 3;
    }
    const wchar_t *game_argument = NULL;
    static const wchar_t *extra[64]; int extra_count = 0;
    for (int i = first; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--game") && i + 1 < argc) game_argument = argv[++i];
        else if (!wcscmp(argv[i], L"--play")) play_now = 1;
        else if (!wcscmp(argv[i], L"--help") || !wcscmp(argv[i], L"-h")) {
            logw(L"SacredFire.exe [--play] [--game <folder>] [game options]\n"
                 L"  Without --play: the launcher window. --play starts the game with the saved settings.\n"
                 L"  BB_DATA_DIR, BB_GAME_DIR, BB_TIMEOUT and the other variables of run.sh apply.\n");
            return 0;
        } else if (extra_count < 64) extra[extra_count++] = argv[i];
    }
    const wchar_t *data_env = env_or(L"BB_DATA_DIR", NULL);
    if (data_env) { if (!GetFullPathNameW(data_env, PATHW, data, NULL)) { fail(L"BB_DATA_DIR is not a usable path"); return 2; } }
    else {
        PWSTR local = NULL;
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) { fail(L"no %LOCALAPPDATA%"); return 2; }
        join(data, PATHW, local, L"SacredFire"); CoTaskMemFree(local);
    }
    ensure_directory(data);
    join(python, PATHW, root, L"python\\python.exe");
    join(game_exe, PATHW, root, L"bin\\sacredfire-game.exe");
    join(caps_exe, PATHW, root, L"bin\\bb-gpu-capabilities.exe");
    settings_load();
    patches_load();
    if (game_argument) { wcsncpy(settings.game, game_argument, PATHW - 1); settings.game[PATHW - 1] = 0; }
    else if (env_or(L"BB_GAME_DIR", NULL)) { wcsncpy(settings.game, env_or(L"BB_GAME_DIR", L""), PATHW - 1); settings.game[PATHW - 1] = 0; }
    if (play_now) {
        open_log_file(NULL);
        if (!settings.game[0]) { fail(L"No game folder: start the launcher once to choose it, or use --game <folder>."); return 2; }
        settings_save();
        const DWORD code = play(extra, extra_count);
        if (log_file) fclose(log_file);
        return (int)code;
    }
    return gui(instance, extra, extra_count);
}
