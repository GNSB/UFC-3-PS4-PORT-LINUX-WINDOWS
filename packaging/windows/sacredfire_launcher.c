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
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define PATHW (MAX_PATH * 4)
static wchar_t root[PATHW];   /* the folder this program runs from */
static wchar_t data[PATHW];   /* data directory */
static wchar_t python[PATHW], game_exe[PATHW], caps_exe[PATHW];

/* ---- settings (<data>\sacredfire.ini) ---- */
typedef struct {
    wchar_t game[PATHW], gamepad[128], player[32];
    int neo, fullscreen, clear_cache, validation, frame_stats, menu_trace;
    /* UFC 3, as the GoW3R launcher's Video panel: the PS4 output size, the frame-rate cap,
     * the swapchain present mode; show_fps mirrors bbport.ini. */
    int resolution, fps_mode, present_mode, show_fps, upscaler, sharpness;
    /* The window (presented image) size: the game's 1080p frame scaled to 1440p / 4K. */
    int display;
} Settings;
static Settings settings = {.resolution = 1, .fps_mode = 4, .present_mode = 2, .upscaler = 0, .sharpness = 2};
/* Choices of the Video panel (index = the saved value). */
static const wchar_t *const resolution_names[] = {L"1280x720", L"1920x1080"};
static const wchar_t *const resolution_w[] = {L"1280", L"1920"}, *const resolution_h[] = {L"720", L"1080"};
static const wchar_t *const display_names[] = {L"Same as the game", L"1920x1080 (Full HD)", L"2560x1440 (2K)", L"3840x2160 (4K)"};
static const wchar_t *const display_sizes[] = {NULL, L"1920x1080", L"2560x1440", L"3840x2160"};
static const wchar_t *const fps_names[] = {L"60 FPS", L"120 FPS", L"144 FPS", L"240 FPS", L"Unlimited"};
static const wchar_t *const fps_limits[] = {L"60", L"120", L"144", L"240", L"0"};
/* Immediate is the default on Windows: NVIDIA's Mailbox there paced the menu at twice the
 * display refresh (120 on a 60 Hz laptop panel, Omen 16 in discrete mode; Immediate 240). */
static const wchar_t *const present_names[] = {L"Mailbox (capped at 2x refresh on NVIDIA)", L"VSync (FIFO)", L"Immediate (uncapped, may tear)"};
static const wchar_t *const present_modes[] = {L"Mailbox", L"Fifo", L"Immediate"};
/* Upscaling at present (spatial, so it fits any game): FSR 1.0 when the window or screen is
 * larger than the game's output; RCAS sharpening with it (attenuation in stops: lower = sharper). */
static const wchar_t *const upscaler_names[] = {L"Off (bilinear)", L"AMD FSR 1.0"};
static const wchar_t *const sharpness_names[] = {L"Off", L"Low", L"Medium", L"High"};
static const wchar_t *const sharpness_attenuation[] = {L"2000", L"1000", L"500", L"200"};
#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))
/* Variables the launcher sets for the game: their values from the environment the launcher was
 * started with (a value there wins over the window), captured once, because Play sets them in
 * this process and a second Play must not take the first one's values for the user's. */
static const wchar_t *const managed_env[] = {
    L"BB_FULLSCREEN", L"BB_VK_VALIDATION", L"BB_FRAME_STATS", L"BB_MENU_TRACE", L"BB_GAMEPAD", L"BB_NEO",
    L"BB_READBACKS", L"BB_READBACK_LINEAR", L"BB_GPU_WRITE_TWINS", L"BB_WRITE_WINDOW", L"BB_FLIP_RATE", L"BB_VBLANK_HZ",
    L"BB_Z16S8_AS_D16", L"BB_PREUPLOAD", L"BB_COPY_GPU_BUFFERS", L"BB_FPS_LIMIT", L"BB_PRESENT_MODE",
    L"BB_INTERNAL_WIDTH", L"BB_INTERNAL_HEIGHT", L"BB_WINDOW_SIZE", L"BB_USER_NAME", L"BB_FSR1", L"BB_RCAS", L"BB_RCAS_ATTENUATION",
};
static wchar_t managed_value[sizeof(managed_env) / sizeof(managed_env[0])][1024];
static int managed_given[sizeof(managed_env) / sizeof(managed_env[0])];
static void managed_env_capture(void) {
    for (int i = 0; i < (int)(sizeof(managed_env) / sizeof(managed_env[0])); ++i) {
        const DWORD n = GetEnvironmentVariableW(managed_env[i], managed_value[i], 1024);
        managed_given[i] = n && n < 1024;
    }
}
/* The user's value if the launcher was started with one, else `value` (NULL: unset). */
static void game_env(const wchar_t *name, const wchar_t *value) {
    for (int i = 0; i < (int)(sizeof(managed_env) / sizeof(managed_env[0])); ++i)
        if (!wcscmp(managed_env[i], name)) { if (managed_given[i]) value = managed_value[i]; break; }
    SetEnvironmentVariableW(name, value);
}
static int clampi(int v, int n) { return v < 0 || v >= n ? 0 : v; }

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
        else if (!wcscmp(line, L"player")) wcsncpy(settings.player, value, 16);
        else if (!wcscmp(line, L"neo")) settings.neo = _wtoi(value);
        else if (!wcscmp(line, L"fullscreen")) settings.fullscreen = _wtoi(value);
        else if (!wcscmp(line, L"clear_cache")) settings.clear_cache = _wtoi(value);
        else if (!wcscmp(line, L"validation")) settings.validation = _wtoi(value);
        else if (!wcscmp(line, L"frame_stats")) settings.frame_stats = _wtoi(value);
        else if (!wcscmp(line, L"menu_trace")) settings.menu_trace = _wtoi(value);
        else if (!wcscmp(line, L"resolution")) settings.resolution = clampi(_wtoi(value), COUNT(resolution_names));
        else if (!wcscmp(line, L"fps_mode")) settings.fps_mode = clampi(_wtoi(value), COUNT(fps_names));
        else if (!wcscmp(line, L"present_mode")) settings.present_mode = clampi(_wtoi(value), COUNT(present_names));
        else if (!wcscmp(line, L"upscaler")) settings.upscaler = clampi(_wtoi(value), COUNT(upscaler_names));
        else if (!wcscmp(line, L"sharpness")) settings.sharpness = clampi(_wtoi(value), COUNT(sharpness_names));
        else if (!wcscmp(line, L"display")) settings.display = clampi(_wtoi(value), COUNT(display_names));
    }
    fclose(f);
}
static void settings_save(void) {
    wchar_t path[PATHW]; settings_path(path);
    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"player=%ls\n", settings.player);
    fwprintf(f, L"game=%ls\ngamepad=%ls\nneo=%d\nfullscreen=%d\nclear_cache=%d\nvalidation=%d\nframe_stats=%d\nmenu_trace=%d\n"
                L"resolution=%d\nfps_mode=%d\npresent_mode=%d\nupscaler=%d\nsharpness=%d\ndisplay=%d\n",
             settings.game, settings.gamepad, settings.neo, settings.fullscreen, settings.clear_cache, settings.validation,
             settings.frame_stats, settings.menu_trace, settings.resolution, settings.fps_mode, settings.present_mode,
             settings.upscaler, settings.sharpness, settings.display);
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
    const char *fps = ini_value("show_fps");
    settings.show_fps = !fps || !strcmp(fps, "1"); /* on until the menu or the launcher turns it off */
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
    if (!exists(python)) { fail(L"python\\python.exe is missing next to the launcher: the package is incomplete."); return 2; }
    if (!exists(game_exe)) { fail(L"bin\\sacredfire-game.exe is missing next to the launcher: the package is incomplete."); return 2; }
    SetEnvironmentVariableW(L"BB_CONFIG", config);
    SetEnvironmentVariableW(L"BB_USER_DIR", env_or(L"BB_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"BB_GPU_USER_DIR", env_or(L"BB_GPU_USER_DIR", user_dir));
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    const wchar_t *game = settings.game;
    wchar_t eboot[PATHW]; join(eboot, PATHW, game, L"eboot.bin");
    if (!game[0] || !exists(eboot)) {
        wchar_t m[PATHW + 128];
        _snwprintf(m, PATHW + 128, L"No eboot.bin in \"%ls\": choose the game folder (CUSA06534, with CUSA06534-patch beside it or merged in).", game);
        m[PATHW + 127] = 0; fail(m); return 2;
    }
    /* Base and update side by side (CUSA06534 + CUSA06534-patch, as PKG extractors leave them):
     * merge_update.py links them into CUSA06534-merged once, and that folder is played. */
    static wchar_t merged_game[PATHW];
    {
        static char merged[PATHW * 3];
        const wchar_t *args[] = {game};
        if (script(L"scripts", L"merge_update.py", args, 1, merged, sizeof(merged))) {
            logf("%s", merged);
            fail(L"Merging the 1.14 update into the game folder failed (see the log).");
            return 2;
        }
        /* The folder is the last line (progress lines come before it). */
        trim(merged);
        char *last = strrchr(merged, '\n');
        if (last) { *last = 0; logf("%s\n", merged); last += 1; } else last = merged;
        MultiByteToWideChar(CP_UTF8, 0, last, -1, merged_game, PATHW); merged_game[PATHW - 1] = 0;
        trimw(merged_game);
        if (merged_game[0]) game = merged_game;
        logw(L"Game folder: %ls\n", game);
    }
    char profile[256] = "";
    if (wcscmp(env_or(L"BB_SKIP_GAME_CHECK", L"0"), L"1")) {
        const wchar_t *args[] = {game};
        if (script(L"scripts", L"game_check.py", args, 1, profile, sizeof(profile))) {
            logf("%s", profile);
            fail(L"The game check failed (see the log): the folder is not EA SPORTS UFC 3 CUSA06534 with the 1.14 update (CUSA06534-patch) beside it or merged in.");
            return 2;
        }
        trim(profile);
    }
    if (!profile[0]) snprintf(profile, sizeof(profile), "ufc3");
    /* The update's build (5.05, 6.72+ or ?): the address-keyed diagnostics know the 5.05 build only. */
    {
        char variant[64] = "?";
        const wchar_t *args[] = {game, L"--variant"};
        if (script(L"scripts", L"game_check.py", args, 2, variant, sizeof(variant)) == 0) trim(variant);
        if (!variant[0]) snprintf(variant, sizeof(variant), "?");
        wchar_t variant_w[64]; MultiByteToWideChar(CP_UTF8, 0, variant, -1, variant_w, 64); variant_w[63] = 0;
        SetEnvironmentVariableW(L"BB_GAME_VARIANT", variant_w);
    }
    wchar_t profile_w[64]; MultiByteToWideChar(CP_UTF8, 0, profile, -1, profile_w, 64); profile_w[63] = 0;
    SetEnvironmentVariableW(L"BB_GAME_PROFILE", profile_w);
    logw(L"Game profile: %ls\n", profile_w);
    /* Preparation (scripts/ as run.sh runs them; mod overlays need symlinks and are not applied here). */
    const wchar_t *prepare[] = {game, L"--out", out};
    if (script(L"scripts", L"prepare.py", prepare, 3, NULL, 0)) { fail(L"prepare.py failed (see the log)"); return 2; }
    if (script(L"scripts", L"link_libc.py", prepare, 3, NULL, 0)) { fail(L"link_libc.py failed (see the log)"); return 2; }
    const wchar_t *link_args[] = {game, L"--out", out, L"--modules", L"libc.prx", L"libSceFios2.prx"};
    if (script(L"scripts", L"link_modules.py", link_args, 6, NULL, 0)) { fail(L"link_modules.py failed (see the log)"); return 2; }
    wchar_t aerolib[PATHW]; join(aerolib, PATHW, root, L"share\\aerolib.inl");
    const wchar_t *names_args[] = {L"--out", out, L"--aerolib", aerolib};
    if (script(L"tools", L"gen_import_names.py", names_args, 4, NULL, 0)) { fail(L"gen_import_names.py failed (see the log)"); return 2; }
    const wchar_t *content_args[] = {game, L"--out", out, L"--sku", env_or(L"BB_CONTENT_SKU", L"full")};
    if (script(L"scripts", L"content_profile.py", content_args, 5, NULL, 0)) { fail(L"content_profile.py failed (see the log)"); return 2; }
    wchar_t patches_dir[PATHW], patches_config[PATHW];
    join(patches_dir, PATHW, root, L"patches"); join(patches_config, PATHW, data, L"patches.json");
    const wchar_t *patch_args[] = {L"--patches-dir", env_or(L"BB_PATCHES_DIR", patches_dir), L"--patches-config", env_or(L"BB_PATCHES_CONFIG", patches_config),
                                   L"--app-version", L"01.14", L"--title-id", L"CUSA06534", L"--out", out, L"--settings", config};
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
    /* The switches (the window's, or the saved ones with --play) and run.sh's defaults for the
     * ufc3 profile. An environment variable set before the launcher wins over each of them. */
    game_env(L"BB_FULLSCREEN", settings.fullscreen ? L"1" : NULL);
    game_env(L"BB_VK_VALIDATION", settings.validation ? L"1" : NULL);
    game_env(L"BB_FRAME_STATS", settings.frame_stats ? L"1" : NULL);
    game_env(L"BB_MENU_TRACE", settings.menu_trace ? L"1" : NULL);
    game_env(L"BB_GAMEPAD", settings.gamepad[0] ? settings.gamepad : NULL);
    game_env(L"BB_USER_NAME", settings.player[0] ? settings.player : NULL); /* the PS4 user name the game shows */
    /* The fight scene renders black without precise readbacks of linear images (shadPS4 issue
     * #4375, UFC 4, same engine); precise readbacks deadlock against the GPU write twins, so
     * those stay off. The game asks for flip rate 1 (30 Hz); its simulation follows the real
     * frame time, so flip rate 0 with a 480 Hz virtual vblank and the present thread's cap set
     * the rate. Z16+S8 shadow data uploaded from memory is kept as D16 (stencil dropped). */
    game_env(L"BB_NEO", L"0");
    game_env(L"BB_READBACKS", L"2"); game_env(L"BB_READBACK_LINEAR", L"1"); game_env(L"BB_GPU_WRITE_TWINS", L"0");
    game_env(L"BB_WRITE_WINDOW", L"1");
    game_env(L"BB_FLIP_RATE", L"0"); game_env(L"BB_VBLANK_HZ", L"480");
    game_env(L"BB_Z16S8_AS_D16", L"1");
    game_env(L"BB_PREUPLOAD", L"1"); game_env(L"BB_COPY_GPU_BUFFERS", L"1");
    game_env(L"BB_FPS_LIMIT", fps_limits[settings.fps_mode]);
    game_env(L"BB_PRESENT_MODE", present_modes[settings.present_mode]);
    game_env(L"BB_INTERNAL_WIDTH", resolution_w[settings.resolution]);
    game_env(L"BB_INTERNAL_HEIGHT", resolution_h[settings.resolution]);
    game_env(L"BB_WINDOW_SIZE", display_sizes[settings.display]);
    game_env(L"BB_FSR1", settings.upscaler == 1 ? L"1" : L"0");
    game_env(L"BB_RCAS", settings.sharpness ? L"1" : L"0");
    game_env(L"BB_RCAS_ATTENUATION", sharpness_attenuation[settings.sharpness]);
    SetEnvironmentVariableW(L"BB_UFFD", L"0");            /* Linux only */
    SetEnvironmentVariableW(L"BB_GUEST_IN_PLACE", L"0");  /* dma-buf memory model: Linux only */
    /* env_or keeps 8 values at a time: two lines. */
    logw(L"UFC 3: output %lsx%ls, frame limit %ls (0: none), present mode %ls, fullscreen %ls\n",
         env_or(L"BB_INTERNAL_WIDTH", L"?"), env_or(L"BB_INTERNAL_HEIGHT", L"?"), env_or(L"BB_FPS_LIMIT", L"?"),
         env_or(L"BB_PRESENT_MODE", L"?"), env_or(L"BB_FULLSCREEN", L"0"));
    logw(L"UFC 3: player name \"%ls\"\n", env_or(L"BB_USER_NAME", L"Player 1"));
    logw(L"UFC 3: window %ls, FSR 1.0 %ls, RCAS %ls (attenuation %ls), readbacks %ls (2 = precise)\n",
         env_or(L"BB_WINDOW_SIZE", L"= output"), env_or(L"BB_FSR1", L"0"), env_or(L"BB_RCAS", L"?"), env_or(L"BB_RCAS_ATTENUATION", L"?"), env_or(L"BB_READBACKS", L"?"));
    if (settings.clear_cache || !wcscmp(env_or(L"BB_CLEAR_SHADER_CACHE", L"0"), L"1")) {
        wchar_t cache[PATHW]; join(cache, PATHW, env_or(L"BB_USER_DIR", user_dir), L"cache\\CUSA06534");
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


/* ---- Steam: a non-Steam shortcut (userdata\<user>\config\shortcuts.vdf) ----
 * The same entry the Linux launcher writes (launcher/steam_shortcuts.py): Steam keeps its
 * non-Steam games in a binary VDF (a map of numbered entries; strings 0x01, 32-bit integers
 * 0x02, nested maps 0x00 ended by 0x08) that it rewrites when it exits, so Steam has to be
 * closed while the entry is added. The app id is the CRC32 of the quoted executable and the
 * name with the top bit set, as Steam computes it. The shortcut starts SacredFire.exe --play. */
static uint32_t crc32_of(const char *text) {
    uint32_t crc = 0xffffffffu;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        crc ^= *p;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static int steam_running(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry = {.dwSize = sizeof(entry)};
    int found = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        if (!_wcsicmp(entry.szExeFile, L"steam.exe")) found = 1;
    CloseHandle(snapshot);
    return found;
}
static int steam_root(wchar_t *out_path) {
    DWORD size = PATHW * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, out_path, &size) != ERROR_SUCCESS) return 0;
    for (wchar_t *p = out_path; *p; ++p) if (*p == L'/') *p = L'\\';
    return exists(out_path);
}
/* A map's contents from `at` (just after its key) to its closing 0x08; -1 on a malformed file. */
static long vdf_skip_map(const unsigned char *d, long n, long at) {
    while (at < n) {
        unsigned char kind = d[at++];
        if (kind == 0x08) return at;
        while (at < n && d[at]) ++at; /* the key */
        if (at++ >= n) return -1;
        if (kind == 0x00) { at = vdf_skip_map(d, n, at); if (at < 0) return -1; }
        else if (kind == 0x01) { while (at < n && d[at]) ++at; if (at++ >= n) return -1; }
        else if (kind == 0x02) at += 4;
        else return -1;
    }
    return -1;
}
static void vdf_put(unsigned char **out, const void *bytes, size_t n) { memcpy(*out, bytes, n); *out += n; }
static void vdf_string(unsigned char **out, const char *key, const char *value) { unsigned char k = 1; vdf_put(out, &k, 1); vdf_put(out, key, strlen(key) + 1); vdf_put(out, value, strlen(value) + 1); }
static void vdf_int(unsigned char **out, const char *key, int32_t value) { unsigned char k = 2; vdf_put(out, &k, 1); vdf_put(out, key, strlen(key) + 1); vdf_put(out, &value, 4); }
/* Adds the entry to one shortcuts.vdf (created if missing; an entry with the same executable
 * and name is replaced). Returns 1 added, 2 replaced, 0 on failure (message in the log). */
static int steam_add_shortcut(const wchar_t *path, const char *exe_quoted, const char *start_dir_quoted) {
    static unsigned char data[1 << 20], fresh[4096];
    long n = 0;
    FILE *f = _wfopen(path, L"rb");
    if (f) { n = (long)fread(data, 1, sizeof(data), f); fclose(f); if (n >= (long)sizeof(data)) { logw(L"%ls: too large\n", path); return 0; } }
    /* The new entry. */
    char appid_text[1200]; snprintf(appid_text, sizeof(appid_text), "%sSacredFire", exe_quoted);
    const int32_t appid = (int32_t)(crc32_of(appid_text) | 0x80000000u);
    unsigned char entry[4096], *e = entry;
    unsigned char kind_map = 0, end = 8;
    char index_key[16] = "0";
    /* Walk the existing file: the "shortcuts" map's entries. */
    long body_start = -1, body_end = -1, replace_start = -1, replace_end = -1; int next_index = 0;
    if (n > 0) {
        long at = 0;
        if (data[at] != 0x00 || strcmp((const char *)data + 1, "shortcuts") != 0) { logw(L"%ls: not a shortcuts file\n", path); return 0; }
        at = 1 + (long)strlen("shortcuts") + 1;
        body_start = at;
        while (at < n && data[at] != 0x08) {
            if (data[at] != 0x00) { logw(L"%ls: unexpected field in the shortcuts map\n", path); return 0; }
            const long entry_start = at;
            const char *key = (const char *)data + at + 1;
            long inner = at + 1 + (long)strlen(key) + 1;
            const long entry_end = vdf_skip_map(data, n, inner);
            if (entry_end < 0) { logw(L"%ls: malformed\n", path); return 0; }
            const int index = atoi(key); if (index + 1 > next_index) next_index = index + 1;
            /* Its Exe and AppName. */
            const char *exe = NULL, *name = NULL;
            for (long k = inner; k < entry_end - 1;) {
                unsigned char kind = data[k++]; const char *field = (const char *)data + k; k += (long)strlen(field) + 1;
                if (kind == 0x01) { const char *value = (const char *)data + k; k += (long)strlen(value) + 1; if (!strcmp(field, "Exe")) exe = value; else if (!strcmp(field, "AppName")) name = value; }
                else if (kind == 0x02) k += 4;
                else if (kind == 0x00) { k = vdf_skip_map(data, n, k); if (k < 0) break; }
                else break;
            }
            if (exe && name && !strcmp(exe, exe_quoted) && !strcmp(name, "SacredFire") && replace_start < 0) { replace_start = entry_start; replace_end = entry_end; snprintf(index_key, sizeof(index_key), "%s", key); }
            at = entry_end;
        }
        if (at >= n) { logw(L"%ls: malformed (no end)\n", path); return 0; }
        body_end = at;
    }
    if (replace_start < 0) snprintf(index_key, sizeof(index_key), "%d", next_index);
    vdf_put(&e, &kind_map, 1); vdf_put(&e, index_key, strlen(index_key) + 1);
    vdf_int(&e, "appid", appid);
    vdf_string(&e, "AppName", "SacredFire");
    vdf_string(&e, "Exe", exe_quoted);
    vdf_string(&e, "StartDir", start_dir_quoted);
    vdf_string(&e, "icon", "");
    vdf_string(&e, "ShortcutPath", "");
    vdf_string(&e, "LaunchOptions", "--play");
    vdf_int(&e, "IsHidden", 0); vdf_int(&e, "AllowDesktopConfig", 1); vdf_int(&e, "AllowOverlay", 1);
    vdf_int(&e, "OpenVR", 0); vdf_int(&e, "Devkit", 0);
    vdf_string(&e, "DevkitGameID", "");
    vdf_int(&e, "DevkitOverrideAppID", 0); vdf_int(&e, "LastPlayTime", 0);
    vdf_string(&e, "FlatpakAppID", "");
    vdf_put(&e, &kind_map, 1); vdf_put(&e, "tags", 5); vdf_put(&e, &end, 1);
    vdf_put(&e, &end, 1);
    const size_t entry_bytes = (size_t)(e - entry);
    /* The new file. */
    wchar_t temp[PATHW]; _snwprintf(temp, PATHW, L"%ls.tmp", path); temp[PATHW - 1] = 0;
    wchar_t folder[PATHW]; wcsncpy(folder, path, PATHW - 1); folder[PATHW - 1] = 0; { wchar_t *slash = wcsrchr(folder, L'\\'); if (slash) *slash = 0; } ensure_directory(folder);
    FILE *out = _wfopen(temp, L"wb");
    if (!out) { logw(L"%ls: cannot write\n", temp); return 0; }
    int result;
    if (n == 0) {
        unsigned char *h = fresh;
        vdf_put(&h, &kind_map, 1); vdf_put(&h, "shortcuts", 10);
        fwrite(fresh, 1, (size_t)(h - fresh), out); fwrite(entry, 1, entry_bytes, out); fputc(8, out); fputc(8, out);
        result = 1;
    } else if (replace_start >= 0) {
        fwrite(data, 1, (size_t)replace_start, out); fwrite(entry, 1, entry_bytes, out); fwrite(data + replace_end, 1, (size_t)(n - replace_end), out);
        result = 2;
    } else {
        fwrite(data, 1, (size_t)body_end, out); fwrite(entry, 1, entry_bytes, out); fwrite(data + body_end, 1, (size_t)(n - body_end), out);
        result = 1;
    }
    (void)body_start;
    fclose(out);
    if (!MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { logw(L"%ls: cannot replace\n", path); return 0; }
    return result;
}
/* The button: every Steam user of this Steam installation gets the shortcut. */
static void add_to_steam(void) {
    if (steam_running()) { fail(L"Close Steam first: it overwrites its shortcuts when it exits. Then press Add to Steam again."); return; }
    wchar_t steam[PATHW], userdata[PATHW], pattern[PATHW];
    if (!steam_root(steam)) { fail(L"Steam was not found (no SteamPath in the registry)."); return; }
    join(userdata, PATHW, steam, L"userdata"); join(pattern, PATHW, userdata, L"*");
    wchar_t self[PATHW]; GetModuleFileNameW(NULL, self, PATHW);
    char exe_utf8[PATHW * 3], root_utf8[PATHW * 3], exe_quoted[PATHW * 3 + 4], root_quoted[PATHW * 3 + 4];
    WideCharToMultiByte(CP_UTF8, 0, self, -1, exe_utf8, sizeof(exe_utf8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, root, -1, root_utf8, sizeof(root_utf8), NULL, NULL);
    snprintf(exe_quoted, sizeof(exe_quoted), "\"%s\"", exe_utf8); snprintf(root_quoted, sizeof(root_quoted), "\"%s\"", root_utf8);
    int added = 0, replaced = 0, failed = 0;
    WIN32_FIND_DATAW found; HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !iswdigit(found.cFileName[0]) || !wcscmp(found.cFileName, L"0")) continue;
            wchar_t config[PATHW], vdf[PATHW];
            join(config, PATHW, userdata, found.cFileName); join(config, PATHW, config, L"config");
            if (!exists(config)) continue;
            join(vdf, PATHW, config, L"shortcuts.vdf");
            const int r = steam_add_shortcut(vdf, exe_quoted, root_quoted);
            if (r == 1) ++added; else if (r == 2) ++replaced; else ++failed;
            logw(L"Steam: %ls: %ls\n", vdf, r == 1 ? L"shortcut added" : r == 2 ? L"shortcut updated" : L"failed");
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (added + replaced == 0) { fail(failed ? L"The Steam shortcut could not be written (see the log)." : L"No Steam user folder was found (Steam\\userdata)."); return; }
    wchar_t message[256];
    _snwprintf(message, 256, L"Added to Steam as \"SacredFire\" (%d added, %d updated). Start Steam to see it; it starts the game with the saved settings (--play).", added, replaced);
    logw(L"%ls\n", message);
    if (!GetEnvironmentVariableW(L"BB_NO_DIALOGS", NULL, 0)) MessageBoxW(main_window, message, L"SacredFire", MB_OK | MB_ICONINFORMATION);
}

/* ---- the window ---- */
enum { ID_GAME = 100, ID_BROWSE, ID_PLAYER, ID_GAMEPAD, ID_RESCAN, ID_CONTROLS, ID_RESOLUTION, ID_DISPLAY, ID_FPS, ID_PRESENT, ID_UPSCALER, ID_SHARPNESS, ID_SHOWFPS, ID_FULLSCREEN, ID_CLEAR, ID_VALIDATION, ID_STATS, ID_TRACE,
       ID_PLAY, ID_STOP, ID_STEAM, ID_LOGS, ID_DATA, ID_LOG, ID_LAST };
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

/* ---- Controls: key.<input>= / pad.<input>= lines of bbport.ini (runtime_pad.c reads them) ---- */
enum { CTL_INPUTS = 25 };
static const char *const ctl_inputs[CTL_INPUTS] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "options", "touchpad",
    "touchpad_right", "up", "down", "left", "right", "move_up", "move_down", "move_left", "move_right",
    "look_up", "look_down", "look_left", "look_right"};
static const wchar_t *const ctl_labels[CTL_INPUTS] = {
    L"Cross (X)", L"Circle", L"Square", L"Triangle", L"L1", L"R1", L"L2", L"R2", L"L3 (left stick click)",
    L"R3 (right stick click)", L"Options", L"Touchpad (left half)", L"Touchpad (right half)", L"D-pad up",
    L"D-pad down", L"D-pad left", L"D-pad right", L"Left stick up", L"Left stick down", L"Left stick left",
    L"Left stick right", L"Right stick up", L"Right stick down", L"Right stick left", L"Right stick right"};
/* The runtime's defaults (runtime_pad.c bind_defaults), as SDL names. */
static const char *const ctl_default_keys[CTL_INPUTS] = {
    "Space", "Left Shift", "E", "Q", "1", "3", "R", "F", "Z", "C", "Return", "Tab", "Backspace",
    "I", "K", "J", "L", "W", "S", "A", "D", "Up", "Down", "Left", "Right"};
static const char *const ctl_default_pads[CTL_INPUTS] = {
    "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick",
    "rightstick", "start", "back,touchpad", "", "dpup", "dpdown", "dpleft", "dpright"};
/* Gamepad buttons (SDL names) and what they are on Xbox / PlayStation pads. */
static const char *const ctl_pad_names[] = {
    "", "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick",
    "rightstick", "start", "back", "guide", "touchpad", "dpup", "dpdown", "dpleft", "dpright", "misc1",
    "paddle1", "paddle2", "paddle3", "paddle4"};
static const wchar_t *const ctl_pad_labels[] = {
    L"(none)", L"A / Cross", L"B / Circle", L"X / Square", L"Y / Triangle", L"LB / L1", L"RB / R1",
    L"LT / L2", L"RT / R2", L"Left stick click", L"Right stick click", L"Start / Options", L"Back / Share",
    L"Guide / PS", L"Touchpad click", L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right",
    L"Misc (Share / Capture)", L"Paddle 1", L"Paddle 2", L"Paddle 3", L"Paddle 4"};
static char ctl_keys[CTL_INPUTS][64], ctl_pads[CTL_INPUTS][64];
static HWND ctl_window, ctl_key_buttons[CTL_INPUTS], ctl_pad_combos[CTL_INPUTS];
static int ctl_capture = -1; /* the row waiting for a key */
enum { ID_CTL_KEY = 2000, ID_CTL_PAD = 2100, ID_CTL_SAVE = 2200, ID_CTL_DEFAULTS, ID_CTL_CANCEL };

static void ctl_show_key(int row) {
    wchar_t text[96];
    if (ctl_capture == row) wcscpy(text, L"Press a key... (Esc: cancel, Delete: none)");
    else if (!ctl_keys[row][0]) wcscpy(text, L"(none)");
    else { MultiByteToWideChar(CP_UTF8, 0, ctl_keys[row], -1, text, 96); }
    SetWindowTextW(ctl_key_buttons[row], text);
}
static void ctl_show_pad(int row) {
    if (row >= 17) return;
    int selected = 0;
    char first[64]; snprintf(first, sizeof(first), "%s", ctl_pads[row]);
    char *comma = strchr(first, ','); if (comma) *comma = 0;
    for (int i = 0; i < COUNT(ctl_pad_names); ++i) if (!_stricmp(first, ctl_pad_names[i])) selected = i;
    SendMessageW(ctl_pad_combos[row], CB_SETCURSEL, selected, 0);
}
static void ctl_load(int defaults) {
    for (int i = 0; i < CTL_INPUTS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "key.%s", ctl_inputs[i]);
        const char *k = defaults ? NULL : ini_value(key);
        snprintf(ctl_keys[i], sizeof(ctl_keys[i]), "%s", k ? k : ctl_default_keys[i]);
        snprintf(key, sizeof(key), "pad.%s", ctl_inputs[i]);
        const char *p = (defaults || i >= 17) ? NULL : ini_value(key);
        snprintf(ctl_pads[i], sizeof(ctl_pads[i]), "%s", p ? p : (i < 17 ? ctl_default_pads[i] : ""));
    }
}
static void ctl_save(void) {
    for (int i = 0; i < CTL_INPUTS; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "key.%s", ctl_inputs[i]); ini_set(key, ctl_keys[i]);
        if (i < 17) { snprintf(key, sizeof(key), "pad.%s", ctl_inputs[i]); ini_set(key, ctl_pads[i]); }
    }
    logw(L"Controls saved to bbport.ini\n");
}
/* A Windows key press as SDL's scancode name (SDL_GetScancodeFromName in runtime_pad.c). */
static int vk_to_sdl_name(WPARAM vk, LPARAM lparam, char *out, size_t n) {
    const int extended = (lparam >> 24) & 1, scan = (lparam >> 16) & 0xff;
    if (vk >= 'A' && vk <= 'Z') { snprintf(out, n, "%c", (int)vk); return 1; }
    if (vk >= '0' && vk <= '9') { snprintf(out, n, "%c", (int)vk); return 1; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(out, n, "F%d", (int)(vk - VK_F1 + 1)); return 1; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, n, "Keypad %d", (int)(vk - VK_NUMPAD0)); return 1; }
    const char *name = NULL;
    switch (vk) {
    case VK_SPACE: name = "Space"; break;
    case VK_RETURN: name = extended ? "Keypad Enter" : "Return"; break;
    case VK_TAB: name = "Tab"; break;
    case VK_BACK: name = "Backspace"; break;
    case VK_SHIFT: name = scan == 0x36 ? "Right Shift" : "Left Shift"; break;
    case VK_CONTROL: name = extended ? "Right Ctrl" : "Left Ctrl"; break;
    case VK_MENU: name = extended ? "Right Alt" : "Left Alt"; break;
    case VK_UP: name = "Up"; break;
    case VK_DOWN: name = "Down"; break;
    case VK_LEFT: name = "Left"; break;
    case VK_RIGHT: name = "Right"; break;
    case VK_HOME: name = "Home"; break;
    case VK_END: name = "End"; break;
    case VK_PRIOR: name = "PageUp"; break;
    case VK_NEXT: name = "PageDown"; break;
    case VK_INSERT: name = "Insert"; break;
    case VK_CAPITAL: name = "CapsLock"; break;
    case VK_OEM_COMMA: name = ","; break;
    case VK_OEM_PERIOD: name = "."; break;
    case VK_OEM_MINUS: name = "-"; break;
    case VK_OEM_PLUS: name = "="; break;
    case VK_OEM_1: name = ";"; break;
    case VK_OEM_2: name = "/"; break;
    case VK_OEM_3: name = "`"; break;
    case VK_OEM_4: name = "["; break;
    case VK_OEM_5: name = "\\"; break;
    case VK_OEM_6: name = "]"; break;
    case VK_OEM_7: name = "'"; break;
    case VK_MULTIPLY: name = "Keypad *"; break;
    case VK_ADD: name = "Keypad +"; break;
    case VK_SUBTRACT: name = "Keypad -"; break;
    case VK_DIVIDE: name = "Keypad /"; break;
    case VK_DECIMAL: name = "Keypad ."; break;
    }
    if (!name) return 0;
    snprintf(out, n, "%s", name);
    return 1;
}
/* Called from the message loop: the key for the row waiting for one. */
static int ctl_capture_key(const MSG *msg) {
    if (ctl_capture < 0 || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) return 0;
    const int row = ctl_capture;
    if (msg->wParam == VK_ESCAPE) { ctl_capture = -1; ctl_show_key(row); return 1; }
    if (msg->wParam == VK_DELETE) { ctl_keys[row][0] = 0; ctl_capture = -1; ctl_show_key(row); return 1; }
    char name[64];
    if (!vk_to_sdl_name(msg->wParam, msg->lParam, name, sizeof(name))) return 1; /* not bindable: keep waiting */
    for (int i = 0; i < CTL_INPUTS; ++i) /* one input per key: the old owner is cleared */
        if (i != row && !_stricmp(ctl_keys[i], name)) { ctl_keys[i][0] = 0; ctl_show_key(i); }
    snprintf(ctl_keys[row], sizeof(ctl_keys[row]), "%s", name);
    ctl_capture = -1;
    ctl_show_key(row);
    return 1;
}
static HWND ctl_make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND h_wnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, sx(x), sx(y), sx(w), sx(h), ctl_window,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h_wnd, WM_SETFONT, (WPARAM)ui_font, TRUE);
    return h_wnd;
}
static void ctl_refresh(void) { for (int i = 0; i < CTL_INPUTS; ++i) { ctl_show_key(i); ctl_show_pad(i); } }
static LRESULT CALLBACK ctl_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id >= ID_CTL_KEY && id < ID_CTL_KEY + CTL_INPUTS) {
            const int previous = ctl_capture;
            ctl_capture = id - ID_CTL_KEY;
            if (previous >= 0) ctl_show_key(previous);
            ctl_show_key(ctl_capture);
        } else if (id >= ID_CTL_PAD && id < ID_CTL_PAD + 17 && HIWORD(wparam) == CBN_SELCHANGE) {
            const int row = id - ID_CTL_PAD;
            const int sel = (int)SendMessageW(ctl_pad_combos[row], CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < COUNT(ctl_pad_names)) snprintf(ctl_pads[row], sizeof(ctl_pads[row]), "%s", ctl_pad_names[sel]);
        } else if (id == ID_CTL_DEFAULTS) { ctl_capture = -1; ctl_load(1); ctl_refresh(); }
        else if (id == ID_CTL_SAVE) { ctl_capture = -1; ctl_save(); DestroyWindow(window); }
        else if (id == ID_CTL_CANCEL) { ctl_capture = -1; DestroyWindow(window); }
        return 0;
    }
    case WM_CLOSE: ctl_capture = -1; DestroyWindow(window); return 0;
    case WM_DESTROY: ctl_window = NULL; EnableWindow(main_window, TRUE); SetForegroundWindow(main_window); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
static void controls_open(void) {
    if (ctl_window) { SetForegroundWindow(ctl_window); return; }
    static int registered;
    if (!registered) {
        WNDCLASSW wc = {.lpfnWndProc = ctl_proc, .hInstance = GetModuleHandleW(NULL), .hCursor = LoadCursorW(NULL, IDC_ARROW),
                        .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .lpszClassName = L"UFC3NativeControls"};
        RegisterClassW(&wc);
        registered = 1;
    }
    ctl_load(0);
    const int rows_h = 26, top = 40, client_w = 640, client_h = top + CTL_INPUTS * rows_h + 56;
    RECT r = {0, 0, sx(client_w), sx(client_h)};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    ctl_window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"UFC3NativeControls", L"Controls", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, main_window, NULL,
                                 GetModuleHandleW(NULL), NULL);
    if (!ctl_window) return;
    ctl_make(L"STATIC", L"Keyboard: click a key box, then press the key. Gamepad: pick a button. The keyboard and a "
                        L"gamepad work together.", 0, 12, 8, 616, 30, 0);
    for (int i = 0; i < CTL_INPUTS; ++i) {
        const int y = top + i * rows_h;
        ctl_make(L"STATIC", ctl_labels[i], SS_CENTERIMAGE, 12, y, 180, 22, 0);
        ctl_key_buttons[i] = ctl_make(L"BUTTON", L"", BS_PUSHBUTTON, 196, y, 230, 22, ID_CTL_KEY + i);
        if (i < 17) {
            ctl_pad_combos[i] = ctl_make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 436, y, 192, 300, ID_CTL_PAD + i);
            for (int k = 0; k < COUNT(ctl_pad_labels); ++k) SendMessageW(ctl_pad_combos[i], CB_ADDSTRING, 0, (LPARAM)ctl_pad_labels[k]);
        } else ctl_make(L"STATIC", L"(gamepad: the stick itself)", SS_CENTERIMAGE, 436, y, 192, 22, 0);
    }
    const int by = top + CTL_INPUTS * rows_h + 14;
    ctl_make(L"BUTTON", L"Defaults", BS_PUSHBUTTON, 12, by, 110, 30, ID_CTL_DEFAULTS);
    ctl_make(L"BUTTON", L"Save", BS_DEFPUSHBUTTON, 400, by, 110, 30, ID_CTL_SAVE);
    ctl_make(L"BUTTON", L"Cancel", BS_PUSHBUTTON, 518, by, 110, 30, ID_CTL_CANCEL);
    ctl_refresh();
    EnableWindow(main_window, FALSE);
    ShowWindow(ctl_window, SW_SHOW);
}

/* The window into the settings; patch switches go to bbport.ini when they changed. */
static void read_controls(void) {
    GetWindowTextW(control(ID_GAME), settings.game, PATHW); trimw(settings.game);
    {
        /* The PS4 allows 16 characters; the game reads them as bytes: printable ASCII only. */
        wchar_t name[64]; GetWindowTextW(control(ID_PLAYER), name, 64); trimw(name);
        int k = 0;
        for (int i = 0; name[i] && k < 16; ++i) if (name[i] >= 0x20 && name[i] < 0x7f) settings.player[k++] = name[i];
        settings.player[k] = 0;
        SetWindowTextW(control(ID_PLAYER), settings.player);
    }
    int sel = (int)SendMessageW(control(ID_GAMEPAD), CB_GETCURSEL, 0, 0);
    if (sel > 0 && sel <= gamepad_count) wcsncpy(settings.gamepad, gamepads[sel - 1].guid, 127); else settings.gamepad[0] = 0;
    settings.fullscreen = checked(ID_FULLSCREEN); settings.clear_cache = checked(ID_CLEAR);
    settings.validation = checked(ID_VALIDATION); settings.frame_stats = checked(ID_STATS); settings.menu_trace = checked(ID_TRACE);
    settings.resolution = clampi((int)SendMessageW(control(ID_RESOLUTION), CB_GETCURSEL, 0, 0), COUNT(resolution_names));
    settings.fps_mode = clampi((int)SendMessageW(control(ID_FPS), CB_GETCURSEL, 0, 0), COUNT(fps_names));
    settings.present_mode = clampi((int)SendMessageW(control(ID_PRESENT), CB_GETCURSEL, 0, 0), COUNT(present_names));
    settings.upscaler = clampi((int)SendMessageW(control(ID_UPSCALER), CB_GETCURSEL, 0, 0), COUNT(upscaler_names));
    settings.sharpness = clampi((int)SendMessageW(control(ID_SHARPNESS), CB_GETCURSEL, 0, 0), COUNT(sharpness_names));
    settings.display = clampi((int)SendMessageW(control(ID_DISPLAY), CB_GETCURSEL, 0, 0), COUNT(display_names));
    const int show_fps = checked(ID_SHOWFPS);
    ini_set("show_fps", show_fps ? "1" : "0"); /* written always: the game's own default is off */
    settings.show_fps = show_fps;
    settings_save();
}
static int pick_folder(wchar_t *out_path) {
    int ok = 0;
    IFileOpenDialog *dialog = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog))) {
        DWORD options = 0;
        dialog->lpVtbl->GetOptions(dialog, &options);
        dialog->lpVtbl->SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->lpVtbl->SetTitle(dialog, L"The game folder (CUSA06534; CUSA06534-patch beside it is merged automatically)");
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
    EnableWindow(control(ID_STEAM), !on);
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == ID_BROWSE) { wchar_t folder[PATHW]; if (pick_folder(folder)) SetWindowTextW(control(ID_GAME), folder); }
        else if (id == ID_RESCAN) { gamepads_refresh(); fill_gamepads(); logw(L"Controllers: %d found\n", gamepad_count); }
        else if (id == ID_CONTROLS && !running) controls_open();
        else if (id == ID_PLAY && !running) {
            read_controls();
            open_log_file(NULL);
            set_running(1);
            HANDLE t = CreateThread(NULL, 0, play_thread, NULL, 0, NULL);
            if (t) CloseHandle(t); else { fail(L"CreateThread failed"); set_running(0); }
        } else if (id == ID_STOP && running) {
            if (game_process) { logw(L"Stop: the game process is ended\n"); TerminateProcess(game_process, 1); }
        } else if (id == ID_STEAM && !running) {
            read_controls(); /* the shortcut uses the saved settings */
            add_to_steam();
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
                    .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .lpszClassName = L"UFC3NativeLauncher", .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1))};
    RegisterClassW(&wc);
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    /* The client area is 760x640 at 96 DPI: the window is created, measured, and resized by the
     * difference (the frame metrics of the running system, whatever they are). */
    const int client_w = sx(760), client_h = sx(794);
    main_window = CreateWindowExW(0, wc.lpszClassName, L"EA SPORTS UFC 3 - native PC port", style, CW_USEDEFAULT, CW_USEDEFAULT, client_w + 16, client_h + 64,
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
    make(L"STATIC", L"EA SPORTS UFC 3 (CUSA06534): the game folder (CUSA06534-patch beside it is merged automatically):", 0, 12, y, 640, 18, 0); y += 20;
    make(L"EDIT", settings.game, WS_BORDER | ES_AUTOHSCROLL, 12, y, 640, 24, ID_GAME);
    make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 660, y, 88, 24, ID_BROWSE); y += 34;
    make(L"STATIC", L"Player name (shown in the game; 16 letters, no accents):", 0, 12, y + 4, 330, 18, 0);
    make(L"EDIT", settings.player[0] ? settings.player : L"Player 1", WS_BORDER | ES_AUTOHSCROLL, 344, y, 200, 24, ID_PLAYER);
    SendMessageW(control(ID_PLAYER), EM_SETLIMITTEXT, 16, 0); y += 34;
    make(L"STATIC", L"Controller for player 1 (in-game menu: L3+R3 or Insert):", 0, 12, y, 640, 18, 0); y += 20;
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 12, y, 520, 200, ID_GAMEPAD);
    make(L"BUTTON", L"Rescan", BS_PUSHBUTTON, 540, y, 88, 24, ID_RESCAN);
    make(L"BUTTON", L"Controls...", BS_PUSHBUTTON, 636, y, 112, 24, ID_CONTROLS); y += 34;
    make(L"BUTTON", L"Video", BS_GROUPBOX, 12, y, 360, 216, 0);
    make(L"STATIC", L"Game resolution", 0, 24, y + 24, 130, 20, 0);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 160, y + 20, 200, 200, ID_RESOLUTION);
    make(L"STATIC", L"Display resolution", 0, 24, y + 56, 130, 20, 0);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 160, y + 52, 200, 200, ID_DISPLAY);
    make(L"STATIC", L"Present mode", 0, 24, y + 88, 130, 20, 0);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 160, y + 84, 200, 200, ID_PRESENT);
    make(L"STATIC", L"Upscaling", 0, 24, y + 120, 130, 20, 0);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 160, y + 116, 200, 200, ID_UPSCALER);
    make(L"STATIC", L"Sharpening (RCAS)", 0, 24, y + 152, 130, 20, 0);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 160, y + 148, 200, 200, ID_SHARPNESS);
    make(L"BUTTON", L"Fullscreen", BS_AUTOCHECKBOX, 24, y + 184, 330, 20, ID_FULLSCREEN);
    make(L"BUTTON", L"Frame rate", BS_GROUPBOX, 388, y, 360, 216, 0);
    make(L"STATIC", L"FPS limit", 0, 400, y + 24, 130, 20, 0);
    make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 536, y + 20, 200, 200, ID_FPS);
    make(L"BUTTON", L"Show the FPS counter", BS_AUTOCHECKBOX, 400, y + 56, 330, 20, ID_SHOWFPS);
    make(L"BUTTON", L"Clear the shader cache at start", BS_AUTOCHECKBOX, 400, y + 88, 330, 20, ID_CLEAR);
    make(L"STATIC", L"2K / 4K: the game draws 1080p; pick the display resolution\r\n(windowed) or play fullscreen on a 1440p / 4K monitor, with\r\nUpscaling = AMD FSR 1.0 and some sharpening.", 0, 400, y + 120, 340, 56, 0); y += 226;
    make(L"BUTTON", L"Developer switches", BS_GROUPBOX, 12, y, 736, 48, 0);
    make(L"BUTTON", L"Frame statistics", BS_AUTOCHECKBOX, 24, y + 20, 160, 20, ID_STATS);
    make(L"BUTTON", L"Vulkan validation layer", BS_AUTOCHECKBOX, 200, y + 20, 200, 20, ID_VALIDATION);
    make(L"BUTTON", L"Menu input trace", BS_AUTOCHECKBOX, 420, y + 20, 200, 20, ID_TRACE); y += 58;
    make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON, 12, y, 120, 30, ID_PLAY);
    make(L"BUTTON", L"Stop", BS_PUSHBUTTON, 140, y, 90, 30, ID_STOP);
    make(L"BUTTON", L"Add to Steam", BS_PUSHBUTTON, 300, y, 130, 30, ID_STEAM);
    make(L"BUTTON", L"Open logs folder", BS_PUSHBUTTON, 480, y, 130, 30, ID_LOGS);
    make(L"BUTTON", L"Open data folder", BS_PUSHBUTTON, 618, y, 130, 30, ID_DATA); y += 40;
    log_edit = make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 12, y, 736, 794 - y - 12, ID_LOG);
    SendMessageW(log_edit, EM_SETLIMITTEXT, 0, 0);
    {
        const struct { int id, count, selected; const wchar_t *const *names; } combos[] = {
            {ID_RESOLUTION, COUNT(resolution_names), settings.resolution, resolution_names},
            {ID_DISPLAY, COUNT(display_names), settings.display, display_names},
            {ID_FPS, COUNT(fps_names), settings.fps_mode, fps_names},
            {ID_PRESENT, COUNT(present_names), settings.present_mode, present_names},
            {ID_UPSCALER, COUNT(upscaler_names), settings.upscaler, upscaler_names},
            {ID_SHARPNESS, COUNT(sharpness_names), settings.sharpness, sharpness_names},
        };
        for (int c = 0; c < COUNT(combos); ++c) {
            for (int i = 0; i < combos[c].count; ++i) SendMessageW(control(combos[c].id), CB_ADDSTRING, 0, (LPARAM)combos[c].names[i]);
            SendMessageW(control(combos[c].id), CB_SETCURSEL, combos[c].selected, 0);
        }
    }
    set_checked(ID_SHOWFPS, settings.show_fps); set_checked(ID_FULLSCREEN, settings.fullscreen); set_checked(ID_CLEAR, settings.clear_cache);
    set_checked(ID_STATS, settings.frame_stats); set_checked(ID_VALIDATION, settings.validation); set_checked(ID_TRACE, settings.menu_trace);
    gamepads_refresh(); fill_gamepads();
    set_running(0);
    char version[64]; read_version(version, sizeof(version));
    logf("SacredFire %s (Windows). Data: ", version); logw(L"%ls\n", data);
    logw(L"Controllers: %d found; window scale %d%%\n", gamepad_count, (int)(scale * 100));
    ShowWindow(main_window, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (ctl_capture_key(&msg)) continue;
        if (ctl_window && IsDialogMessageW(ctl_window, &msg)) continue;
        if (!IsDialogMessageW(main_window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line; (void)show;
    /* Started from a console (cmd, a script): its output stays there as well. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) console_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    managed_env_capture();
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
        join(data, PATHW, local, L"UFC3Native"); CoTaskMemFree(local);
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
