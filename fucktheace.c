/*
 * FUCK THE ACE — 减少99%ACE扫盘带来的卡顿
 *
 * 监听 SGuardSvc64.exe / SGuard64.exe / ACE-Tray.exe，
 * 运行：
 *    1. 设置优先级为低
 *    2. CPU 绑定到最小核
 *
 * 系统托盘驻留，右键菜单，需要管理员权限。
 *
 * 编译 (MinGW-w64):
 *   windres resource.rc -o resource.o
 *   gcc -Os -s -mwindows -municode -static
 *       -ffunction-sections -fdata-sections -Wl,--gc-sections
 *       -Wl,--stack,65536
 *       fucktheace.c resource.o -o fucktheace.exe
 *
 * 运行时内存: < 1 MB
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

// ─── 常量 ───────────────────────────────────────────────────
#define WM_TRAYICON     (WM_USER + 1)
#define IDM_LOG         1001
#define IDM_AUTOSTART   1002
#define IDM_EXIT        1003
#define IDM_INTERVAL_1S 2001
#define IDM_INTERVAL_10S 2002
#define IDM_INTERVAL_30S 2003
#define IDM_INTERVAL_60S 2004
#define IDM_INTERVAL_120S 2005
#define IDM_INTERVAL_300S 2006
#define TIMER_ID        1
#define TRAY_RETRY_ID   2
#define POLL_INTERVAL_MS (60 * 1000)

#define MUTEX_NAME      L"Local\\FuckTheAce_Singleton"

static const WCHAR *TARGETS[] = {
    L"sguardsvc64.exe",
    L"sguard64.exe",
    L"ace-tray.exe",
};
#define TARGET_COUNT 3

// ─── 全局状态 ──────────────────────────────────────────────
static NOTIFYICONDATAW g_nid         = {0};
static int             g_last_cpu    = 0;
static int             g_poll_ms     = POLL_INTERVAL_MS;
static BOOL            g_autostart   = FALSE;
static int             g_tray_retries = 0;
static WCHAR           g_log_path[MAX_PATH];
static WCHAR           g_config_path[MAX_PATH];
static WCHAR           g_exe_dir[MAX_PATH];
static CRITICAL_SECTION g_log_cs;

static DWORD          *g_handled_pids = NULL;
static size_t          g_handled_count = 0;
static CRITICAL_SECTION g_handled_cs;

// ─── 工具函数 ──────────────────────────────────────────────

static BOOL is_target_process(const WCHAR *name)
{
    for (int i = 0; i < TARGET_COUNT; i++)
        if (lstrcmpW(name, TARGETS[i]) == 0)
            return TRUE;
    return FALSE;
}

static int get_cpu_count(void)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

// ─── 日志 & 配置路径 ──────────────────────────────────────
static void log_init(void)
{
    WCHAR buf[MAX_PATH];

    // exe 所在目录（图标回退用）
    GetModuleFileNameW(NULL, g_exe_dir, MAX_PATH);
    WCHAR *last = wcsrchr(g_exe_dir, L'\\');
    if (last) *last = L'\0';

    // %APPDATA%\fucktheace\
    ExpandEnvironmentStringsW(L"%APPDATA%", buf, MAX_PATH);
    wsprintfW(g_log_path, L"%s\\fucktheace", buf);
    CreateDirectoryW(g_log_path, NULL);

    // config.ini
    lstrcpyW(g_config_path, g_log_path);
    lstrcatW(g_config_path, L"\\config.ini");

    // fucktheace.log
    lstrcatW(g_log_path, L"\\fucktheace.log");

    InitializeCriticalSection(&g_log_cs);
}

static void log_write(const WCHAR *msg)
{
    EnterCriticalSection(&g_log_cs);

    SYSTEMTIME lt;
    GetLocalTime(&lt);
    WCHAR line[1024];
    int len = wsprintfW(line, L"[%02d:%02d:%02d] %s\r\n",
                        lt.wHour, lt.wMinute, lt.wSecond, msg);

    char utf8[2048];
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, line, len,
                                       utf8, sizeof(utf8), NULL, NULL);

    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, utf8, utf8_len, &written, NULL);
        CloseHandle(h);
    }

    LeaveCriticalSection(&g_log_cs);
}

// ─── 配置持久化 ────────────────────────────────────────────
static void config_load(void)
{
    g_autostart = GetPrivateProfileIntW(L"Settings", L"AutoStart", 0, g_config_path);
    g_poll_ms   = GetPrivateProfileIntW(L"Settings", L"PollInterval", POLL_INTERVAL_MS, g_config_path);
}

static void config_save(void)
{
    WCHAR buf[32];
    wsprintfW(buf, L"%d", g_autostart);
    WritePrivateProfileStringW(L"Settings", L"AutoStart", buf, g_config_path);
    wsprintfW(buf, L"%d", g_poll_ms);
    WritePrivateProfileStringW(L"Settings", L"PollInterval", buf, g_config_path);
}

// ─── 管理员 ────────────────────────────────────────────────
static BOOL is_admin(void)
{
    BOOL ok = FALSE;
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION elev;
        DWORD len = sizeof(elev);
        if (GetTokenInformation(tok, TokenElevation, &elev, len, &len))
            ok = elev.TokenIsElevated;
        CloseHandle(tok);
    }
    return ok;
}

static void elevate(void)
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow  = SW_HIDE;
    if (!ShellExecuteExW(&sei))
        ExitProcess(1);
    ExitProcess(0);
}

// ─── 单例 ──────────────────────────────────────────────────
static BOOL check_singleton(void)
{
    HANDLE h = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (!h) return FALSE;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return FALSE;
    }
    return TRUE;
}

// ─── 进程枚举 ──────────────────────────────────────────────
typedef struct {
    DWORD pid;
    WCHAR name[260];
} ProcEntry;

typedef struct {
    ProcEntry *entries;
    size_t     count;
} ProcList;

static ProcList enum_processes(void)
{
    ProcList list = {NULL, 0};
    HANDLE   snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return list;

    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcEntry *tmp = realloc(list.entries,
                                     (list.count + 1) * sizeof(ProcEntry));
            if (!tmp) break;
            list.entries = tmp;
            list.entries[list.count].pid = pe.th32ProcessID;
            lstrcpyW(list.entries[list.count].name, pe.szExeFile);
            CharLowerBuffW(list.entries[list.count].name,
                           lstrlenW(list.entries[list.count].name));
            list.count++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return list;
}

static void proc_list_free(ProcList *list)
{
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
}

// ─── 压制 / 巡检 ──────────────────────────────────────────

static void nerf_process(DWORD pid, WCHAR *out, int out_len)
{
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
                           FALSE, pid);
    if (!h) {
        lstrcpynW(out, L"OpenProcess失败", out_len);
        return;
    }

    out[0] = L'\0';
    DWORD_PTR mask = (DWORD_PTR)1 << g_last_cpu;

    if (SetPriorityClass(h, IDLE_PRIORITY_CLASS))
        lstrcatW(out, L"低优先✓ ");
    else {
        WCHAR buf[64];
        wsprintfW(buf, L"低优先✗(%lu) ", GetLastError());
        lstrcatW(out, buf);
    }

    if (SetProcessAffinityMask(h, mask))
        wsprintfW(out + lstrlenW(out), L"CPU%d✓", g_last_cpu);
    else {
        WCHAR buf[64];
        wsprintfW(buf, L"CPU✗(%lu)", GetLastError());
        lstrcatW(out, buf);
    }

    CloseHandle(h);
}

static BOOL check_process_health(DWORD pid, WCHAR *out, int out_len)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        // 无法打开进程（权限不足或已退出），不触发重新压制避免日志刷屏
        wsprintfW(out, L"无法打开(%lu)", GetLastError());
        return TRUE;
    }

    BOOL ok = TRUE;
    out[0] = L'\0';

    DWORD prio = GetPriorityClass(h);
    if (prio != IDLE_PRIORITY_CLASS) {
        ok = FALSE;
        WCHAR buf[64];
        wsprintfW(buf, L"优先级已恢复(%lu); ", prio);
        lstrcatW(out, buf);
    }

    DWORD_PTR proc_mask = 0, sys_mask = 0;
    if (GetProcessAffinityMask(h, &proc_mask, &sys_mask)) {
        if (proc_mask != (DWORD_PTR)1 << g_last_cpu) {
            ok = FALSE;
            wsprintfW(out + lstrlenW(out), L"CPU亲和性已改变(%I64x); ",
                      (unsigned long long)proc_mask);
        }
    } else {
        ok = FALSE;
        wsprintfW(out + lstrlenW(out), L"CPU检查失败(%lu); ", GetLastError());
    }

    CloseHandle(h);
    if (ok) lstrcpynW(out, L"OK", out_len);
    return ok;
}

// ─── 已处理 PID 管理 ──────────────────────────────────────

static BOOL pid_is_handled(DWORD pid)
{
    for (size_t i = 0; i < g_handled_count; i++)
        if (g_handled_pids[i] == pid) return TRUE;
    return FALSE;
}

static void pid_add(DWORD pid)
{
    DWORD *tmp = realloc(g_handled_pids,
                         (g_handled_count + 1) * sizeof(DWORD));
    if (tmp) {
        g_handled_pids = tmp;
        g_handled_pids[g_handled_count++] = pid;
    }
}

static void pid_purge_dead(const DWORD *alive_pids, size_t alive_count)
{
    size_t write = 0;
    for (size_t i = 0; i < g_handled_count; i++) {
        for (size_t j = 0; j < alive_count; j++) {
            if (g_handled_pids[i] == alive_pids[j]) {
                g_handled_pids[write++] = g_handled_pids[i];
                break;
            }
        }
    }
    g_handled_count = write;
}

// ─── 扫描 & 压制 ──────────────────────────────────────────

static void scan_and_nerf(void)
{
    ProcList procs = enum_processes();
    if (!procs.entries) return;

    // 一次遍历：收集目标 PID + 处理
    DWORD *target_pids = NULL;
    size_t target_count = 0;

    for (size_t i = 0; i < procs.count; i++) {
        if (!is_target_process(procs.entries[i].name)) continue;

        DWORD pid = procs.entries[i].pid;

        // 记录存活 PID
        DWORD *tmp = realloc(target_pids, (target_count + 1) * sizeof(DWORD));
        if (!tmp) { free(target_pids); proc_list_free(&procs); return; }
        target_pids = tmp;
        target_pids[target_count++] = pid;

        // 处理
        WCHAR logmsg[512];
        EnterCriticalSection(&g_handled_cs);

        if (!pid_is_handled(pid)) {
            LeaveCriticalSection(&g_handled_cs);

            WCHAR detail[256];
            nerf_process(pid, detail, 256);
            wsprintfW(logmsg, L"[✓] %s (PID=%lu) → %s",
                      procs.entries[i].name, pid, detail);
            log_write(logmsg);

            EnterCriticalSection(&g_handled_cs);
            pid_add(pid);
            LeaveCriticalSection(&g_handled_cs);

        } else {
            LeaveCriticalSection(&g_handled_cs);

            WCHAR health[256];
            if (!check_process_health(pid, health, 256)) {
                wsprintfW(logmsg,
                    L"[⚠] %s (PID=%lu) 状态已失效: %s，重新压制",
                    procs.entries[i].name, pid, health);
                log_write(logmsg);

                WCHAR detail[256];
                nerf_process(pid, detail, 256);
                wsprintfW(logmsg, L"    → %s", detail);
                log_write(logmsg);
            }
        }
    }

    // 清理已退出的 PID
    EnterCriticalSection(&g_handled_cs);
    pid_purge_dead(target_pids, target_count);
    LeaveCriticalSection(&g_handled_cs);

    free(target_pids);
    proc_list_free(&procs);
}

// ─── 计划任务辅助 ──────────────────────────────────────────
static DWORD schtasks_run(const WCHAR *args, int timeout_ms)
{
    WCHAR cmd[1024];
    wsprintfW(cmd, L"cmd /c \"schtasks %s >nul 2>&1\"", args);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    DWORD exit = 1;

    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, timeout_ms);
        GetExitCodeProcess(pi.hProcess, &exit);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return exit;
}

// ─── 开机自启（计划任务，免UAC） ───────────────────────────
static BOOL autostart_enabled(void)
{
    return g_autostart;
}

static void toggle_autostart(void)
{
    g_autostart = !g_autostart;

    if (!g_autostart) {
        schtasks_run(L"/delete /tn \"FuckTheAce\" /f", 10000);
        config_save();
        log_write(L"开机启动: 已关闭");
        return;
    }

    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);

    WCHAR args[1024];
    wsprintfW(args, L"/create /tn \"FuckTheAce\" /tr \"\\\"%s\\\"\" /sc onlogon /rl highest /f", exe);

    DWORD exit = schtasks_run(args, 10000);

    if (exit == 0) {
        config_save();
        log_write(L"开机启动: 已开启（计划任务，免UAC）");
    } else {
        g_autostart = FALSE;
        WCHAR logmsg[256];
        wsprintfW(logmsg, L"开机启动: 创建失败（退出码 %lu）", exit);
        log_write(logmsg);
    }
}

// ─── 监控间隔 ──────────────────────────────────────────────
static void set_poll_interval(HWND hwnd, int ms)
{
    g_poll_ms = ms;
    KillTimer(hwnd, TIMER_ID);
    SetTimer(hwnd, TIMER_ID, ms, NULL);
    config_save();

    WCHAR logmsg[64];
    if (ms <= 1000)
        lstrcpyW(logmsg, L"监控间隔: 实时");
    else
        wsprintfW(logmsg, L"监控间隔: %ds", ms / 1000);
    log_write(logmsg);
}

// ─── 日志查看 ──────────────────────────────────────────────
static void show_log(void)
{
    HANDLE h = CreateFileW(g_log_path, 0, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    ShellExecuteW(NULL, L"open", g_log_path, NULL, NULL, SW_SHOWNORMAL);
}

// ─── 系统托盘 ──────────────────────────────────────────────
static HICON load_icon(void)
{
    HICON h = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (h) return h;

    WCHAR path[MAX_PATH];
    wsprintfW(path, L"%s\\app.ico", g_exe_dir);
    h = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    if (h) return h;

    return LoadIconW(NULL, IDI_SHIELD);
}

static void show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    BOOL auto_on = autostart_enabled();

    AppendMenuW(menu, MF_STRING, IDM_LOG, L"查看日志");
    AppendMenuW(menu, MF_STRING | (auto_on ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART, L"开机时启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    // 监控间隔子菜单
    HMENU sub = CreatePopupMenu();
    static const struct { int id; int ms; const WCHAR *label; } items[] = {
        {IDM_INTERVAL_1S,   1000,   L"实时"},
        {IDM_INTERVAL_10S,  10000,  L"10s"},
        {IDM_INTERVAL_30S,  30000,  L"30s"},
        {IDM_INTERVAL_60S,  60000,  L"60s（推荐）"},
        {IDM_INTERVAL_120S, 120000, L"120s"},
        {IDM_INTERVAL_300S, 300000, L"300s"},
    };
    for (int i = 0; i < 6; i++) {
        UINT f = MF_STRING;
        if (g_poll_ms == items[i].ms) f |= MF_CHECKED;
        AppendMenuW(sub, f, items[i].id, items[i].label);
    }
    AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)sub, L"监控间隔");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出程序");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    INT cmd = (INT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);

    switch (cmd) {
    case IDM_LOG:       show_log();                              break;
    case IDM_AUTOSTART: toggle_autostart();                       break;
    case IDM_INTERVAL_1S:  set_poll_interval(hwnd, 1000);   break;
    case IDM_INTERVAL_10S: set_poll_interval(hwnd, 10000);  break;
    case IDM_INTERVAL_30S: set_poll_interval(hwnd, 30000);  break;
    case IDM_INTERVAL_60S: set_poll_interval(hwnd, 60000);  break;
    case IDM_INTERVAL_120S: set_poll_interval(hwnd, 120000); break;
    case IDM_INTERVAL_300S: set_poll_interval(hwnd, 300000); break;
    case IDM_EXIT:
        log_write(L"用户选择退出");
        DestroyWindow(hwnd);
        break;
    }
}

// ─── 窗口过程 ──────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        HICON hIcon = load_icon();
        g_nid.cbSize = sizeof(NOTIFYICONDATAW);
        g_nid.hWnd   = hwnd;
        g_nid.uID    = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon  = hIcon;
        lstrcpynW(g_nid.szTip, L"Fuck The Ace (运行中)", 128);

        // 任务计划程序启动时 explorer 可能还没就绪，注册失败则定时重试
        if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
            SetTimer(hwnd, TRAY_RETRY_ID, 2000, NULL);

        SetTimer(hwnd, TIMER_ID, g_poll_ms, NULL);
        scan_and_nerf();

        WCHAR logmsg[128];
        wsprintfW(logmsg,
            L"守护已启动 — %d 逻辑核, 目标绑核 CPU%d, 轮询 %ds",
            get_cpu_count(), g_last_cpu, g_poll_ms / 1000);
        log_write(logmsg);
        return 0;
    }

    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP) show_menu(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ID) {
            scan_and_nerf();
        } else if (wp == TRAY_RETRY_ID) {
            if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
                KillTimer(hwnd, TRAY_RETRY_ID);
            } else if (++g_tray_retries >= 15) {
                KillTimer(hwnd, TRAY_RETRY_ID);
                log_write(L"托盘图标注册失败（已重试15次）");
            }
        }
        return 0;

    case WM_DESTROY:
        g_nid.uFlags = 0;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        log_write(L"守护已退出");
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── 入口 ─────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    log_init();
    config_load();

    if (!is_admin()) {
        elevate();
        return 0;
    }

    // 同步计划任务：配置已开 → 确保任务存在；配置已关 → 删除
    if (g_autostart) {
        DWORD exit = schtasks_run(L"/query /tn \"FuckTheAce\" /fo csv /nh", 3000);
        if (exit != 0) {
            WCHAR exe[MAX_PATH];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            WCHAR args[1024];
            wsprintfW(args, L"/create /tn \"FuckTheAce\" /tr \"\\\"%s\\\"\" /sc onlogon /rl highest /f", exe);
            if (schtasks_run(args, 10000) != 0) {
                g_autostart = FALSE;
                config_save();
            }
        }
    }

    if (!check_singleton()) {
        MessageBoxW(NULL, L"FuckTheAce 已在运行中（系统托盘）",
                    L"Fuck The Ace", MB_ICONINFORMATION);
        return 0;
    }

    InitializeCriticalSection(&g_handled_cs);
    g_last_cpu = get_cpu_count() - 1;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"FuckTheAceTray";

    if (!RegisterClassExW(&wc)) {
        WCHAR buf[128];
        wsprintfW(buf, L"RegisterClassEx 失败: %lu", GetLastError());
        log_write(buf);
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, L"FuckTheAceTray", L"", 0,
                                0, 0, 0, 0,
                                HWND_MESSAGE, NULL, hInstance, NULL);
    if (!hwnd) {
        WCHAR buf[128];
        wsprintfW(buf, L"CreateWindowEx 失败: %lu", GetLastError());
        log_write(buf);
        return 1;
    }

    log_write(L"Fuck The Ace 启动成功");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    free(g_handled_pids);
    DeleteCriticalSection(&g_handled_cs);
    DeleteCriticalSection(&g_log_cs);

    return (int)msg.wParam;
}
