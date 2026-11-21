/* ============================================================================
 *  selflog  -  自记录式启动 / I/O 追踪器 (Windows, C, MSVC)
 *
 *  作用：把"这次启动"的事实写进一个日志文件，不依赖任何外部工具。
 *
 *    1) 启动参数：完整命令行、逐个 argv、工作目录、父进程、用户、时间戳
 *    2) 标准输入：运行期间从 stdin 读到的每一个字节
 *    3) 标准输出：运行期间写给 stdout / stderr 的每一个字节
 *
 *  两种模式：
 *    selflog [选项]                    自身模式：像 tee 一样把 stdin 抄到 stdout，
 *                                      同时记录参数与全部 I/O
 *    selflog [选项] -- 程序 [参数...]   包装模式：用管道启动子进程，把子进程的
 *                                      命令行 + stdin/stdout/stderr 全部 tee 进日志
 *
 *  构建：见 build.bat（cl /O2 /MT /utf-8）
 * ==========================================================================*/

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#define SELFLOG_VER   "1.0.0"
#define IO_CHUNK      16384
#define LINE_WIDTH    118

/* ------------------------------------------------------------------ 全局 */
static FILE           *g_log  = NULL;
static CRITICAL_SECTION g_lock;
static ULONGLONG       g_t0   = 0;
static int             g_raw  = 0;
static UINT            g_cp   = 0;   /* 0 = 不做代码页解码；936 = GBK ... */   /* 1 = 负载按原始字节写入（不在 \n 处转义） */
static int             g_noecho = 0;
static int             g_quiet = 0;
static wchar_t         g_logpath[MAX_PATH * 2] = L"";
static int             g_depth = 0;   /* 代理嵌套层数，防 _real 指回自己造成死循环 */

static unsigned long long g_in_bytes = 0, g_out_bytes = 0, g_err_bytes = 0;

/* ------------------------------------------------------------------ 工具 */
static char *w2u(const wchar_t *w)
{
    int n;
    char *o;
    if (!w) w = L"";
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) n = 1;
    o = (char *)malloc((size_t)n);
    if (!o) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, o, n, NULL, NULL) <= 0) o[0] = '\0';
    return o;
}

static int is_console(HANDLE h)
{
    DWORD mode;
    return h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) ? 1 : 0;
}

/* 时间戳：本地 / UTC，毫秒精度 */
static void fmt_local(char *buf, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void fmt_utc(char *buf, size_t cap)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03dZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

/* 相对进程启动的耗时，用于给每一段 I/O 打上时间点 */
static void fmt_elapsed(char *buf, size_t cap)
{
    ULONGLONG ms = GetTickCount64() - g_t0;
    snprintf(buf, cap, "+%llu.%03llus",
             (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
}

/* ------------------------------------------------------------------ 写日志 */
static void log_bytes(const char *buf, size_t len)
{
    if (!g_log || !len) return;
    EnterCriticalSection(&g_lock);
    fwrite(buf, 1, len, g_log);
    fflush(g_log);            /* 每条都落盘：进程被杀也不丢已记录内容 */
    LeaveCriticalSection(&g_lock);
}

static void log_printf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;   /* 截断也要写出去 */
    log_bytes(buf, (size_t)n);
}

/* 宽字符串 → UTF-8 之后写一行 "key : value" */
static void log_kv_w(const char *key, const wchar_t *val)
{
    char *u = w2u(val);
    log_printf("%-14s: %s\n", key, u ? u : "");
    free(u);
}

static void log_kv(const char *key, const char *val)
{
    log_printf("%-14s: %s\n", key, val ? val : "");
}

/* 计数（与日志写入共用同一把锁，顺序天然一致） */
static void count_add(unsigned long long *p, size_t n)
{
    EnterCriticalSection(&g_lock);
    *p += (unsigned long long)n;
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------- 记录一段 stdin / stdout */
/*
 * 负载落盘有两种姿态：
 *   默认（字节精确）  不可见字节全部转义，日志永远是可读文本且可逆：
 *                     \r \n \t \\ 两字符转义，其余控制字符/非法字节写成 \xHH，
 *                     合法 UTF-8 多字节序列原样保留（中文正常显示）。
 *   --raw            原样写入，适合事后做字节级比对。
 *   --cp <n>         先把负载按代码页 n（如中文 Windows 的 936=GBK）解码，
 *                     再以 UTF-8 写日志——牺牲逐字节可逆，换取可读性。
 */
static int utf8_seq_len(unsigned char c)
{
    if (c >= 0xC2 && c <= 0xDF) return 1;
    if (c >= 0xE0 && c <= 0xEF) return 2;
    if (c >= 0xF0 && c <= 0xF4) return 3;
    return 0;
}

static char *mbcs_to_utf8(const unsigned char *b, size_t n, UINT cp, size_t *out_len)
{
    wchar_t *w;
    char    *u;
    int      wn, un;

    wn = MultiByteToWideChar(cp, 0, (const char *)b, (int)n, NULL, 0);
    if (wn <= 0) return NULL;

    w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(cp, 0, (const char *)b, (int)n, w, wn) <= 0) {
        free(w);
        return NULL;
    }

    un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    u  = (un > 0) ? (char *)malloc((size_t)un) : NULL;
    if (u && WideCharToMultiByte(CP_UTF8, 0, w, wn, u, un, NULL, NULL) > 0)
        *out_len = (size_t)un;
    else {
        free(u);
        u = NULL;
    }
    free(w);
    return u;
}

static void escape_write(const unsigned char *b, size_t n)
{
    char   out[512];
    size_t o = 0;
    int    col = 0;

    for (size_t i = 0; i < n; i++) {
        char esc[8];
        int  en;
        unsigned char c = b[i];

        /* 合法 UTF-8 多字节序列整段放过，中文才能直接读 */
        if (c >= 0x80) {
            int need = utf8_seq_len(c);          /* 序列占 i..i+need，需全部在 n 内 */
            if (need > 0 && i + (size_t)need < n) {
                int ok = 1;
                for (int k = 1; ok && k <= need; k++)
                    if ((b[i + k] & 0xC0) != 0x80) ok = 0;
                if (ok) {
                    if (col + need + 1 > LINE_WIDTH) { out[o++] = '\n'; log_bytes(out, o); o = 0; col = 0; }
                    if (o > sizeof out - 16) { log_bytes(out, o); o = 0; }
                    for (int k = 0; k <= need; k++) out[o++] = (char)b[i + k];
                    col += need + 1;
                    i += (size_t)need;
                    continue;
                }
            }
            snprintf(esc, sizeof esc, "\\x%02X", c);
            en = 4;
        } else {
            switch (c) {
            case '\r': esc[0] = '\\'; esc[1] = 'r';  en = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  en = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  en = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; en = 2; break;
            default:
                if (c >= 0x20 && c < 0x7F) { esc[0] = (char)c; en = 1; }
                else { snprintf(esc, sizeof esc, "\\x%02X", c); en = 4; }
                break;
            }
        }

        if (col + en > LINE_WIDTH) {          /* 折行，保持日志可读 */
            out[o++] = '\n';
            log_bytes(out, o);
            o = 0; col = 0;
        }
        if (o > sizeof out - 16) { log_bytes(out, o); o = 0; }
        for (int k = 0; k < en; k++) out[o++] = esc[k];
        col += en;
    }
    if (o) log_bytes(out, o);
}

static void log_payload(const char *kind, const unsigned char *b, size_t n)
{
    char head[160], el[32];
    int  hl;

    if (!n) return;

    fmt_elapsed(el, sizeof el);
    hl = snprintf(head, sizeof head, "--- [%s] %-3s %llu bytes ---\n",
                  el, kind, (unsigned long long)n);
    if (hl < 0) return;
    log_bytes(head, (size_t)hl);

    if (g_raw) {
        log_bytes((const char *)b, n);
    } else if (g_cp) {
        size_t clen = 0;
        char  *conv = mbcs_to_utf8(b, n, g_cp, &clen);
        if (conv) { escape_write((const unsigned char *)conv, clen); free(conv); }
        else       escape_write(b, n);
    } else {
        escape_write(b, n);
    }
    log_bytes("\n", 1);
}

/* --------------------------------------------------------- 控制台消息（自身）
 * 自己的提示信息走 WriteConsoleW，保证中文在任何代码页下都不乱码；
 * 输出被重定向到文件/管道时退化成 UTF-8 字节。
 */
static void con_write(HANDLE h, const wchar_t *w)
{
    DWORD mode, written = 0;
    if (!h || h == INVALID_HANDLE_VALUE || !w) return;

    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, w, (DWORD)wcslen(w), &written, NULL);
    } else {
        char *u = w2u(w);
        if (u) {
            WriteFile(h, u, (DWORD)strlen(u), &written, NULL);
            free(u);
        }
    }
}

static void con_printf(HANDLE h, const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 2048, fmt, ap);
    va_end(ap);
    con_write(h, buf);
}

/* --------------------------------------------------- 写出 stdout / stderr 并记录 */
static void emit_stream(int is_err, const unsigned char *b, size_t n)
{
    HANDLE h = GetStdHandle(is_err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD  w = 0;

    if (h && h != INVALID_HANDLE_VALUE)
        WriteFile(h, b, (DWORD)n, &w, NULL);

    log_payload(is_err ? "ERR" : "OUT", b, n);
    count_add(is_err ? &g_err_bytes : &g_out_bytes, n);
}

/* ================================================================= 参数解析 */
typedef struct {
    const wchar_t *log_path;
    const wchar_t *tag;
    const wchar_t *say;
    const wchar_t *errtext;
    int cp, raw, no_echo, no_stdin, quiet, truncate, help, version;
    int action;                 /* 0=正常跑 1=install 2=uninstall */
    const wchar_t *action_arg;
    wchar_t **child;
    int child_argc;
} Opt;

static void usage(HANDLE h)
{
    con_printf(h,
        L"selflog %hs —— 会记录自己的 exe：启动参数 + stdin/stdout/stderr\n"
        L"\n"
        L"用法:\n"
        L"  selflog [选项]                        自身模式：边读 stdin 边写 stdout，全程记录\n"
        L"  selflog [选项] -- 程序 [参数...]       包装模式：记录指定程序\n"
        L"  selflog --install <原始程序.exe>       把自己顶替成该程序（真身改名 <主名>_real.exe）\n"
        L"  selflog --uninstall <原始程序.exe>     撤销顶替，把真身改回来\n"
        L"\n"
        L"透明代理模式（主要用法）:\n"
        L"  original.exe 改名为 original_real.exe，再把 selflog.exe 复制成 original.exe。\n"
        L"  之后每次启动 original.exe，参数与 stdin/stdout/stderr 都原样转发给\n"
        L"  original_real.exe，同时写进日志；退出码原样返回，调用方察觉不到中间多了一层。\n"
        L"  此模式不解析任何命令行选项（参数必须原样转发），配置只能走环境变量。\n"
        L"\n"
        L"选项:\n"
        L"  -l, --log <文件>    指定日志文件（默认：exe 同目录 selflog.log）\n"
        L"  -t, --tag <名称>    给这次会话打标签，便于在同名进程间区分\n"
        L"      --truncate      本次运行前清空日志（默认追加）\n"
        L"      --raw           负载按原始字节写入，不转义 \\n 等\n"
        L"      --cp <代码页>   按代码页解码后再以 UTF-8 写日志（如 936=GBK，可读中文）\n"
        L"      --text          等价于 --cp <当前控制台代码页>，中文 Windows 上就是 936\n"
        L"      --no-echo       包装模式下不把子进程输出回显到本控制台\n"
        L"      --no-stdin      自身模式下不读标准输入（只记录启动参数）\n"
        L"      --say <文本>    自身模式：向 stdout 写一段文本（用于自测）\n"
        L"      --err <文本>    自身模式：向 stderr 写一段文本（用于自测）\n"
        L"  -q, --quiet         不打印提示信息\n"
        L"  -h, --help          显示本帮助\n"
        L"  -v, --version       显示版本\n"
        L"\n"
        L"环境变量（透明代理模式下唯一的配置途径）:\n"
        L"  SELFLOG_FILE       日志路径（优先级高于默认位置）\n"
        L"  SELFLOG_TAG        会话标签\n"
        L"  SELFLOG_CP         按代码页解码为 UTF-8，如 936\n"
        L"  SELFLOG_RAW=1      负载原样写入，不转义\n"
        L"  SELFLOG_NOECHO=1   只记日志，不回显\n"
        L"  SELFLOG_TRUNCATE=1 本次运行前清空日志\n"
        L"  SELFLOG_REAL       手工指定真身路径，覆盖 <主名>_real.exe 约定\n"
        L"\n"
        L"示例:\n"
        L"  selflog --install \"C:\\tools\\foo.exe\"     之后直接运行 foo.exe 即可，日志在 foo 同目录\n"
        L"  selflog --uninstall \"C:\\tools\\foo.exe\"   恢复原状\n"
        L"  type in.txt | selflog --log run.log\n"
        L"  selflog -- cmd /c \"dir /b\"\n",
        SELFLOG_VER);
}

static int need_value(int i, int argc, const wchar_t *name, HANDLE herr)
{
    if (i + 1 >= argc) {
        con_printf(herr, L"selflog: 选项 %ls 缺少参数值\n", name);
        return 0;
    }
    return 1;
}

static int parse_args(int argc, wchar_t **argv, Opt *o)
{
    HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
    memset(o, 0, sizeof *o);

    for (int i = 1; i < argc; i++) {
        wchar_t *a = argv[i];

        if (wcscmp(a, L"--") == 0) {            /* 显式分隔：后面全是子进程 */
            o->child      = &argv[i + 1];
            o->child_argc = argc - i - 1;
            return 1;
        }
        if (a[0] == L'-' && a[1] != L'\0') {
            if (!wcscmp(a, L"-h") || !wcscmp(a, L"--help"))          { o->help = 1; }
            else if (!wcscmp(a, L"-v") || !wcscmp(a, L"--version"))  { o->version = 1; }
            else if (!wcscmp(a, L"--raw"))                           { o->raw = 1; }
            else if (!wcscmp(a, L"--text"))                          { o->cp = (int)GetConsoleOutputCP(); }
            else if (!wcscmp(a, L"--install")) {
                if (!need_value(i, argc, a, herr)) return 0;
                o->action = 1;
                o->action_arg = argv[++i];
            }
            else if (!wcscmp(a, L"--uninstall")) {
                if (!need_value(i, argc, a, herr)) return 0;
                o->action = 2;
                o->action_arg = argv[++i];
            }
            else if (!wcscmp(a, L"--cp")) {
                if (!need_value(i, argc, a, herr)) return 0;
                o->cp = _wtoi(argv[++i]);
            }
            else if (!wcscmp(a, L"--no-echo"))                       { o->no_echo = 1; }
            else if (!wcscmp(a, L"--no-stdin"))                      { o->no_stdin = 1; }
            else if (!wcscmp(a, L"-q") || !wcscmp(a, L"--quiet"))    { o->quiet = 1; }
            else if (!wcscmp(a, L"--truncate"))                      { o->truncate = 1; }
            else if (!wcscmp(a, L"-l") || !wcscmp(a, L"--log")) {
                if (!need_value(i, argc, a, herr)) return 0;
                o->log_path = argv[++i];
            }
            else if (!wcscmp(a, L"-t") || !wcscmp(a, L"--tag")) {
                if (!need_value(i, argc, a, herr)) return 0;
                o->tag = argv[++i];
            }
            else if (!wcscmp(a, L"--say")) {
                if (!need_value(i, argc, a, herr)) return 0;
                o->say = argv[++i];
            }
            else if (!wcscmp(a, L"--err")) {
                if (!need_value(i, argc, a, herr)) return 0;
                o->errtext = argv[++i];
            }
            else {
                con_printf(herr, L"selflog: 未知选项 %ls（用 --help 查看用法）\n", a);
                return 0;
            }
        } else {                                /* 第一个非选项 → 子进程起点 */
            o->child      = &argv[i];
            o->child_argc = argc - i;
            return 1;
        }
    }
    return 1;
}

/* ================================================================= 日志文件 */
/* 递归创建父目录（--log 指向的目录可能还不存在） */
static void ensure_parent(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH * 2];
    size_t  i;

    if (!path || !*path) return;
    wcsncpy(tmp, path, MAX_PATH * 2 - 1);
    tmp[MAX_PATH * 2 - 1] = L'\0';

    for (i = (tmp[1] == L':') ? 3 : 1; tmp[i]; i++) {
        if (tmp[i] == L'\\') {
            tmp[i] = L'\0';
            CreateDirectoryW(tmp, NULL);
            tmp[i] = L'\\';
        }
    }
}

/* 统一转成绝对路径再记录：否则日志里的 log_file 只是相对当前目录的串 */
static void set_logpath(const wchar_t *p)
{
    wchar_t full[MAX_PATH * 2];
    DWORD   n;

    n = GetFullPathNameW(p, MAX_PATH * 2, full, NULL);
    if (n > 0 && n < MAX_PATH * 2) {
        wcsncpy(g_logpath, full, MAX_PATH * 2 - 1);
    } else {
        wcsncpy(g_logpath, p, MAX_PATH * 2 - 1);
    }
    g_logpath[MAX_PATH * 2 - 1] = L'\0';
}

static FILE *open_log(const wchar_t *path, int truncate)
{
    FILE *f;

    if (!path || !*path) return NULL;
    ensure_parent(path);                 /* --log 指向的目录可能还不存在 */
    f = _wfopen(path, truncate ? L"wb" : L"ab");
    return f;
}

static int log_is_empty(FILE *f)
{
    long pos;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    pos = ftell(f);
    return pos == 0;
}

/*
 * 日志文件解析顺序：
 *   --log  >  环境变量 SELFLOG_FILE  >  exe 目录  >  %TEMP%  >  当前目录
 * 每一步都真开一次文件确认可写，失败就退到下一个候选。
 */
static int resolve_log(const wchar_t *explicit_path, int truncate, int quiet)
{
    static const wchar_t *NAME = L"selflog.log";
    wchar_t cand[MAX_PATH * 2];
    HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);

    if (explicit_path && *explicit_path) {
        g_log = open_log(explicit_path, truncate);
        if (g_log) {
            set_logpath(explicit_path);
            return 1;
        }
        if (!quiet)
            con_printf(herr, L"selflog: 无法打开日志 %ls（错误 %lu），改用默认位置\n",
                       explicit_path, GetLastError());
    }

    /* 环境变量 */
    if (GetEnvironmentVariableW(L"SELFLOG_FILE", cand, MAX_PATH * 2) > 0) {
        g_log = open_log(cand, truncate);
        if (g_log) { set_logpath(cand); return 1; }
    }

    /* exe 所在目录 */
    cand[0] = L'\0';
    if (GetModuleFileNameW(NULL, cand, MAX_PATH * 2) > 0) {
        wchar_t *slash = wcsrchr(cand, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat(cand, NAME);
            g_log = open_log(cand, truncate);
            if (g_log) { set_logpath(cand); return 1; }
        }
    }

    /* %TEMP% */
    if (GetEnvironmentVariableW(L"TEMP", cand, MAX_PATH * 2) > 0) {
        if (cand[wcslen(cand) - 1] != L'\\') wcscat(cand, L"\\");
        wcscat(cand, NAME);
        g_log = open_log(cand, truncate);
        if (g_log) { set_logpath(cand); return 1; }
    }

    /* 当前目录（兜底） */
    g_log = open_log(NAME, truncate);
    if (g_log) { set_logpath(NAME); return 1; }

    con_printf(herr, L"selflog: 找不到可写的日志位置。\n");
    return 0;
}

/* ================================================================= 会话头 */
static void parent_info(DWORD *ppid_out, wchar_t *name, size_t namechars)
{
    DWORD  me = GetCurrentProcessId(), ppid = 0;
    HANDLE snap;
    PROCESSENTRY32W pe;

    *ppid_out = 0;
    if (name && namechars) name[0] = L'\0';

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == me) { ppid = pe.th32ParentProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    if (ppid) {
        pe.dwSize = sizeof pe;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == ppid) {
                    wcsncpy(name, pe.szExeFile, namechars - 1);
                    name[namechars - 1] = L'\0';
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
    }
    CloseHandle(snap);
    *ppid_out = ppid;
}

static void log_header(const wchar_t *mode, const Opt *o)
{
    char     ts[48], tu[48], pidbuf[32], ppbuf[40];
    wchar_t  exe[MAX_PATH * 2] = L"", cwd[1024] = L"";
    wchar_t  pname[MAX_PATH] = L"", user[256] = L"";
    wchar_t  pcname[256] = L"";
    DWORD    n, ppid = 0;

    fmt_local(ts, sizeof ts);
    fmt_utc(tu, sizeof tu);

    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    GetCurrentDirectoryW(1024, cwd);
    parent_info(&ppid, pname, MAX_PATH);

    n = 256; if (!GetUserNameW(user, &n)) user[0] = L'\0';
    n = 256; if (!GetComputerNameW(pcname, &n)) pcname[0] = L'\0';

    log_printf("\n================================================================================\n");
    log_printf("SESSION %s  (local)   |   %s  (UTC)\n", ts, tu);
    log_printf("================================================================================\n");

    log_kv_w("mode", mode);
    if (o->tag) log_kv_w("tag", o->tag);
    log_kv_w("exe", exe);
    log_kv_w("cwd", cwd);
    snprintf(pidbuf, sizeof pidbuf, "%lu", (unsigned long)GetCurrentProcessId());
    log_kv("pid", pidbuf);
    snprintf(ppbuf, sizeof ppbuf, "%lu", (unsigned long)ppid);
    {
        char *pn = w2u(pname[0] ? pname : L"?");
        log_printf("%-14s: %s (%s)\n", "parent", ppbuf, pn ? pn : "?");
        free(pn);
    }
    if (user[0]) {
        wchar_t who[600];
        swprintf(who, 600, L"%s\\%s", pcname, user);
        log_kv_w("user", who);
    }
    log_kv_w("log_file", g_logpath);
    {
        char desc[128];
        if (g_raw)      snprintf(desc, sizeof desc, "raw bytes (verbatim, not escaped)");
        else if (g_cp)  snprintf(desc, sizeof desc, "text (CP %u -> UTF-8, byte-exact escapes off)", g_cp);
        else            snprintf(desc, sizeof desc, "escaped (byte-exact, UTF-8 passed through)");
        log_kv("encoding", desc);
    }
    log_kv_w("cmdline", GetCommandLineW());
}

/* argv 逐条列出（命令行字符串会被引号/转义影响可读性，argv 才是真相） */
static void log_argv(const char *prefix, int argc, wchar_t **argv)
{
    char key[48], argc_key[48];

    snprintf(argc_key, sizeof argc_key, "%sargc", prefix);
    log_printf("%-14s: %d\n", argc_key, argc);

    for (int i = 0; i < argc; i++) {
        snprintf(key, sizeof key, "%sargv[%d]", prefix, i);
        log_kv_w(key, argv[i] ? argv[i] : L"(null)");
    }
}

static void log_footer(int exit_code)
{
    char ts[48], el[32];

    fmt_local(ts, sizeof ts);
    fmt_elapsed(el, sizeof el);

    log_printf("\n--------------------------------------------------------------------------------\n");
    log_printf("END %s  elapsed=%s  exit=%d\n", ts,
               (el[0] == '+') ? el + 1 : el, exit_code);
    log_printf("%-14s: stdin=%llu stdout=%llu stderr=%llu bytes\n", "totals",
               g_in_bytes, g_out_bytes, g_err_bytes);
    log_printf("================================================================================\n");
}

/* ================================================================ 自身模式 */
/*
 * 把 stdin 抄到 stdout（当 stdin 是控制台时不回显，避免同一行显示两遍），
 * 同时三路全部入账：读到的 → IN，写出的 → OUT。
 */
static int run_self(const Opt *o)
{
    HANDLE hin  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    int    hin_console = is_console(hin);
    unsigned char buf[IO_CHUNK];
    DWORD  n = 0;

    if (o->say) {
        size_t len = (size_t)WideCharToMultiByte(CP_UTF8, 0, o->say, -1, NULL, 0, NULL, NULL);
        if (len > 0) {
            char *u = (char *)malloc(len);
            if (u) {
                WideCharToMultiByte(CP_UTF8, 0, o->say, -1, u, (int)len, NULL, NULL);
                emit_stream(0, (const unsigned char *)u, strlen(u));
                free(u);
            }
        }
    }
    if (o->errtext) {
        size_t len = (size_t)WideCharToMultiByte(CP_UTF8, 0, o->errtext, -1, NULL, 0, NULL, NULL);
        if (len > 0) {
            char *u = (char *)malloc(len);
            if (u) {
                WideCharToMultiByte(CP_UTF8, 0, o->errtext, -1, u, (int)len, NULL, NULL);
                emit_stream(1, (const unsigned char *)u, strlen(u));
                free(u);
            }
        }
    }

    if (o->no_stdin || !hin || hin == INVALID_HANDLE_VALUE) return 0;

    /* 控制台里手动运行时，ReadFile 会一直等输入；先说清楚，免得像卡死 */
    if (hin_console && !g_quiet)
        con_printf(GetStdHandle(STD_ERROR_HANDLE),
                   L"[selflog] 正在读取标准输入，输入结束后按 Ctrl+Z 再回车退出\n");

    for (;;) {
        if (!ReadFile(hin, buf, sizeof buf, &n, NULL) || n == 0) break;

        log_payload("IN", buf, n);
        count_add(&g_in_bytes, n);

        if (!hin_console) {                       /* 管道/文件输入 → 真的转发 */
            DWORD w = 0;
            if (hout && hout != INVALID_HANDLE_VALUE)
                WriteFile(hout, buf, n, &w, NULL);
            log_payload("OUT", buf, n);
            count_add(&g_out_bytes, n);
        }
    }
    return 0;
}

/* ================================================================ 包装模式 */
/*
 * 三个泵各跑一个线程。管道是流式的，子进程每 write 一次我们就会被唤醒一次，
 * 如果每次都落一条记录，日志会被 "dir" 这种逐行输出切成几十段。
 * 所以 OUT/ERR 侧先攒在缓冲里，直到管道被抽干（PeekNamedPipe 看不到数据）
 * 或缓冲满 64KB 才落盘，日志里一段输出 = 一段记录。
 */
typedef struct {
    HANDLE  src;
    HANDLE  dst;
    const char *kind;
    unsigned long long *counter;
    HANDLE  done;
    unsigned char buf[IO_CHUNK * 4];
    size_t  len;
} Pump;

static void pump_flush(Pump *p)
{
    DWORD w = 0;

    if (!p->len) return;

    log_payload(p->kind, p->buf, p->len);
    if (p->counter) count_add(p->counter, p->len);

    if (p->dst && p->dst != INVALID_HANDLE_VALUE) {
        DWORD off = 0;
        while (off < p->len) {
            DWORD chunk = (DWORD)((p->len - off) > 65536 ? 65536 : (p->len - off));
            if (!WriteFile(p->dst, p->buf + off, chunk, &w, NULL) || w == 0) break;
            off += w;
        }
    }
    p->len = 0;
}

/* 子进程 stdout / stderr → 日志 + 回显 */
static DWORD WINAPI pump_read(LPVOID pv)
{
    Pump *p = (Pump *)pv;
    DWORD n = 0, avail = 0;

    for (;;) {
        if (!PeekNamedPipe(p->src, NULL, 0, NULL, &avail, NULL))
            break;                                  /* 写端已关闭 */
        if (avail == 0)
            pump_flush(p);                          /* 这一批输出结束，收尾 */

        if (!ReadFile(p->src, p->buf + p->len, (DWORD)(sizeof p->buf - p->len), &n, NULL) || n == 0)
            break;

        p->len += n;
        if (p->len >= sizeof p->buf) pump_flush(p);
    }
    pump_flush(p);
    SetEvent(p->done);
    return 0;
}

/* 我们的 stdin → 子进程 stdin（交互要即时转发，不做攒批） */
static DWORD WINAPI pump_stdin(LPVOID pv)
{
    Pump *p = (Pump *)pv;
    unsigned char buf[IO_CHUNK];
    DWORD n = 0;

    for (;;) {
        if (!p->src || p->src == INVALID_HANDLE_VALUE) break;
        if (!ReadFile(p->src, buf, sizeof buf, &n, NULL) || n == 0) break;

        log_payload(p->kind, buf, n);
        if (p->counter) count_add(p->counter, n);

        if (p->dst && p->dst != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(p->dst, buf, n, &w, NULL);
        }
    }
    /* 关掉写端，子进程才能看到 EOF 而不是永远等输入 */
    if (p->dst && p->dst != INVALID_HANDLE_VALUE) CloseHandle(p->dst);

    SetEvent(p->done);
    return 0;
}

/* 简易宽字符缓冲，用于拼子进程命令行 */
typedef struct { wchar_t *p; size_t len, cap; } WBuf;

static void wb_init(WBuf *b)
{
    b->cap = 256;
    b->p   = (wchar_t *)malloc(b->cap * sizeof(wchar_t));
    b->len = 0;
    if (b->p) b->p[0] = L'\0';
}

static void wb_reserve(WBuf *b, size_t more)
{
    size_t need = b->len + more + 1;
    if (need <= b->cap || !b->p) return;
    while (b->cap < need) b->cap *= 2;
    b->p = (wchar_t *)realloc(b->p, b->cap * sizeof(wchar_t));
}

static void wb_ch(WBuf *b, wchar_t c)
{
    wb_reserve(b, 1);
    if (!b->p) return;
    b->p[b->len++] = c;
    b->p[b->len]   = L'\0';
}

static void wb_wstr(WBuf *b, const wchar_t *s)
{
    size_t n = wcslen(s);
    wb_reserve(b, n);
    if (!b->p) return;
    memcpy(b->p + b->len, s, n * sizeof(wchar_t));
    b->len += n;
    b->p[b->len] = L'\0';
}

/*
 * CreateProcessW 只接受一整条命令行字符串，所以必须按 Windows 的解析规则
 * 反向拼装：含空格/制表/引号或为空 → 加引号；引号前的反斜杠要成对翻倍。
 */
static wchar_t *build_cmdline(wchar_t **args, int n)
{
    WBuf b;
    wb_init(&b);
    if (!b.p) return NULL;

    for (int i = 0; i < n; i++) {
        const wchar_t *a = args[i] ? args[i] : L"";
        int need_quote = (*a == L'\0');

        for (const wchar_t *q = a; *q; q++)
            if (*q == L' ' || *q == L'\t' || *q == L'"') { need_quote = 1; break; }

        if (i) wb_ch(&b, L' ');
        if (!need_quote) { wb_wstr(&b, a); continue; }

        wb_ch(&b, L'"');
        for (const wchar_t *q = a; ; q++) {
            size_t bs = 0;
            while (*q == L'\\') { bs++; q++; }

            if (*q == L'\0') {                       /* 结尾的反斜杠要翻倍 */
                for (size_t k = 0; k < bs * 2; k++) wb_ch(&b, L'\\');
                break;
            }
            if (*q == L'"') {                        /* 引号前反斜杠翻倍再转义 */
                for (size_t k = 0; k < bs * 2 + 1; k++) wb_ch(&b, L'\\');
                wb_ch(&b, L'"');
            } else {
                for (size_t k = 0; k < bs; k++) wb_ch(&b, L'\\');
                wb_ch(&b, *q);
            }
        }
        wb_ch(&b, L'"');
    }
    return b.p;
}

/* 用三个管道起子进程，并 tee 它的 stdin/stdout/stderr；args[0] 是程序路径 */
static int run_child(wchar_t **args, int n)
{
    HANDLE hin_r = NULL, hin_w = NULL;   /* 子进程 stdin */
    HANDLE hout_r = NULL, hout_w = NULL; /* 子进程 stdout */
    HANDLE herr_r = NULL, herr_w = NULL; /* 子进程 stderr */
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t *cmdline;
    Pump pumps[3];
    HANDLE threads[3];
    DWORD  exit_code = 0;
    int    ok;

    cmdline = build_cmdline(args, n);
    if (!cmdline) return 3;

    log_kv_w("child", cmdline);
    log_printf("--------------------------------------------------------------------------------\n");

    sa.nLength        = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hin_r, &hin_w, &sa, 0) ||
        !CreatePipe(&hout_r, &hout_w, &sa, 0) ||
        !CreatePipe(&herr_r, &herr_w, &sa, 0)) {
        log_printf("ERROR: CreatePipe failed (%lu)\n", GetLastError());
        free(cmdline);
        return 3;
    }
    /* 父进程这一端不该被继承，否则子进程退出后管道读端仍然不关闭 */
    SetHandleInformation(hin_w,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hout_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(herr_r, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof si);
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hin_r;
    si.hStdOutput = hout_w;
    si.hStdError  = herr_w;

    {
        wchar_t d[16];
        swprintf(d, 16, L"%d", g_depth + 1);
        SetEnvironmentVariableW(L"SELFLOG_DEPTH", d);
    }

    memset(&pi, 0, sizeof pi);
    ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableW(L"SELFLOG_DEPTH", NULL);
    if (!ok) {
        char *u = w2u(cmdline);
        log_printf("ERROR: CreateProcess failed (%lu) for: %s\n", GetLastError(), u ? u : "?");
        free(u);
        free(cmdline);
        CloseHandle(hin_r);  CloseHandle(hin_w);
        CloseHandle(hout_r); CloseHandle(hout_w);
        CloseHandle(herr_r); CloseHandle(herr_w);
        return 3;
    }
    free(cmdline);

    /* 子进程已持有自己的副本，父进程关掉不该留的端 */
    CloseHandle(hin_r);  hin_r  = NULL;
    CloseHandle(hout_w); hout_w = NULL;
    CloseHandle(herr_w); herr_w = NULL;

    log_printf("%-14s: %lu\n", "child_pid", (unsigned long)pi.dwProcessId);
    log_printf("--------------------------------------------------------------------------------\n");

    /* 三个泵：stdin 送进去，stdout/stderr 抄出来 */
    memset(pumps, 0, sizeof pumps);
    pumps[0].src = GetStdHandle(STD_INPUT_HANDLE);
    pumps[0].dst = hin_w;
    pumps[0].kind = "IN";
    pumps[0].counter = &g_in_bytes;

    pumps[1].src = hout_r;
    pumps[1].dst = g_noecho ? NULL : GetStdHandle(STD_OUTPUT_HANDLE);
    pumps[1].kind = "OUT";
    pumps[1].counter = &g_out_bytes;

    pumps[2].src = herr_r;
    pumps[2].dst = g_noecho ? NULL : GetStdHandle(STD_ERROR_HANDLE);
    pumps[2].kind = "ERR";
    pumps[2].counter = &g_err_bytes;

    for (int i = 0; i < 3; i++) {
        pumps[i].done   = CreateEventW(NULL, TRUE, FALSE, NULL);
        LPTHREAD_START_ROUTINE fn = (i == 0) ? pump_stdin : pump_read;
        threads[i] = CreateThread(NULL, 0, fn, &pumps[i], 0, NULL);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);

    /* 子进程退出后，stdout/stderr 的读端会 EOF；给 5 秒兜底防挂死。
       stdin 泵可能还阻塞在控制台读取上，不等它——进程退出即可回收。 */
    {
        HANDLE outs[2];
        outs[0] = pumps[1].done;
        outs[1] = pumps[2].done;
        WaitForMultipleObjects(2, outs, TRUE, 5000);
    }
    WaitForSingleObject(pumps[1].done, 0);
    WaitForSingleObject(pumps[2].done, 0);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hin_w)  CloseHandle(hin_w);
    if (hout_r) CloseHandle(hout_r);
    if (herr_r) CloseHandle(herr_r);
    for (int i = 0; i < 3; i++) {
        if (threads[i]) CloseHandle(threads[i]);
        if (pumps[i].done) CloseHandle(pumps[i].done);
    }

    return (int)exit_code;
}

/* ======================================================= 透明代理所需的小工具 */
static int file_exists(const wchar_t *p)
{
    DWORD a;
    if (!p || !*p) return 0;
    a = GetFileAttributesW(p);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

static int same_path(const wchar_t *a, const wchar_t *b)
{
    wchar_t fa[MAX_PATH * 2], fb[MAX_PATH * 2];
    if (!GetFullPathNameW(a, MAX_PATH * 2, fa, NULL)) return 0;
    if (!GetFullPathNameW(b, MAX_PATH * 2, fb, NULL)) return 0;
    return _wcsicmp(fa, fb) == 0;
}

/* 逐字节比较两个文件（用于确认某个文件确实是 selflog 的副本，删之前先验身） */
static int files_identical(const wchar_t *a, const wchar_t *b)
{
    FILE *fa, *fb;
    unsigned char ba[8192], bb[8192];
    size_t na, nb;
    int same = 0;

    fa = _wfopen(a, L"rb");
    if (!fa) return 0;
    fb = _wfopen(b, L"rb");
    if (!fb) { fclose(fa); return 0; }

    for (;;) {
        na = fread(ba, 1, sizeof ba, fa);
        nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || (na && memcmp(ba, bb, na) != 0)) break;
        if (na == 0) { same = 1; break; }
    }
    fclose(fa);
    fclose(fb);
    return same;
}

/* 所在目录，带尾部反斜杠；没有目录部分就是 ".\\" */
static void path_dir(const wchar_t *path, wchar_t *out, size_t cap)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *slash2 = wcsrchr(path, L'/');
    size_t n;

    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    if (!slash) {
        wcsncpy(out, L".\\", cap - 1);
        out[cap - 1] = L'\0';
        return;
    }
    n = (size_t)(slash - path) + 1;
    if (n >= cap) n = cap - 1;
    wcsncpy(out, path, n);
    out[n] = L'\0';
}

/* 文件名主干：去掉目录和扩展名。original.exe -> original */
static void path_stem(const wchar_t *path, wchar_t *out, size_t cap)
{
    const wchar_t *base = path, *p;
    wchar_t *dot;

    for (p = path; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
    wcsncpy(out, base, cap - 1);
    out[cap - 1] = L'\0';

    dot = wcsrchr(out, L'.');
    if (dot && dot != out) *dot = L'\0';
}

static int env_flag(const wchar_t *name)
{
    wchar_t v[16];
    if (GetEnvironmentVariableW(name, v, 16) == 0) return 0;
    return (v[0] == L'1' || v[0] == L'y' || v[0] == L'Y' || v[0] == L't' || v[0] == L'T');
}

static int env_int(const wchar_t *name, int def)
{
    wchar_t v[32];
    if (GetEnvironmentVariableW(name, v, 32) == 0) return def;
    return _wtoi(v);
}

/*
 * 找"真身"：把 original.exe 顶替之后，真身就叫 original_real.exe。
 * 查找顺序：SELFLOG_REAL 环境变量（用于任意命名/调试）> 同目录 <主名>_real.exe
 * > 同目录 <主名>_real（无扩展名）。
 * 主名取自本 exe 的实际文件名，而不是 argv[0]——argv[0] 可能是相对路径或被改写的名字。
 */
static int find_real(wchar_t *out, size_t cap)
{
    wchar_t env[MAX_PATH * 2], self[MAX_PATH * 2], dir[MAX_PATH * 2], stem[MAX_PATH];

    out[0] = L'\0';

    if (GetEnvironmentVariableW(L"SELFLOG_REAL", env, MAX_PATH * 2) > 0 && file_exists(env)) {
        wcsncpy(out, env, cap - 1);
        out[cap - 1] = L'\0';
        return 1;
    }

    if (!GetModuleFileNameW(NULL, self, MAX_PATH * 2)) return 0;
    path_dir(self, dir, MAX_PATH * 2);
    path_stem(self, stem, MAX_PATH);

    swprintf(out, cap, L"%s%s_real.exe", dir, stem);
    if (file_exists(out)) return 1;

    swprintf(out, cap, L"%s%s_real", dir, stem);
    if (file_exists(out)) return 1;

    out[0] = L'\0';
    return 0;
}

/*
 * 代理模式下不解析命令行（参数必须原样转发），所以配置全部来自环境变量。
 * 默认安静：不能往被代理程序的 stderr 里掺自己的提示。
 */
static void opt_from_env(Opt *o)
{
    wchar_t v[MAX_PATH * 2];

    memset(o, 0, sizeof *o);

    if (GetEnvironmentVariableW(L"SELFLOG_FILE", v, MAX_PATH * 2) > 0) o->log_path = _wcsdup(v);
    if (GetEnvironmentVariableW(L"SELFLOG_TAG", v, MAX_PATH * 2) > 0)  o->tag = _wcsdup(v);

    o->cp       = env_int(L"SELFLOG_CP", 0);
    o->raw      = env_flag(L"SELFLOG_RAW");
    o->no_echo  = env_flag(L"SELFLOG_NOECHO");
    o->truncate = env_flag(L"SELFLOG_TRUNCATE");
    o->quiet    = 1;
}

/* ----------------------------------------------------- install / uninstall */
/*
 * --install original.exe
 *     original.exe        -> original_real.exe   （真身挪开）
 *     selflog.exe         -> original.exe        （顶上去当替身）
 * --uninstall original.exe  反向恢复，删替身前先逐字节确认那确实是 selflog 的副本。
 */
static int do_install(const wchar_t *target, int uninstall)
{
    wchar_t self[MAX_PATH * 2], full[MAX_PATH * 2];
    wchar_t dir[MAX_PATH * 2], stem[MAX_PATH], real[MAX_PATH * 2];
    HANDLE  herr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD   n;

    n = GetFullPathNameW(target, MAX_PATH * 2, full, NULL);
    if (n == 0 || n >= MAX_PATH * 2) {
        con_printf(herr, L"selflog: 路径不合法: %ls\n", target);
        return 2;
    }

    GetModuleFileNameW(NULL, self, MAX_PATH * 2);

    if (!file_exists(full)) {
        con_printf(herr, L"selflog: 找不到文件 %ls\n", full);
        return 2;
    }
    if (same_path(full, self)) {
        con_printf(herr, L"selflog: 这是 selflog 自己，请指定要被顶替的原始程序\n");
        return 2;
    }

    path_dir(full, dir, MAX_PATH * 2);
    path_stem(full, stem, MAX_PATH);
    swprintf(real, MAX_PATH * 2, L"%s%s_real.exe", dir, stem);

    if (!uninstall) {
        if (file_exists(real)) {
            con_printf(herr, L"selflog: 已存在 %ls，可能已经装过；先用 --uninstall 恢复\n", real);
            return 2;
        }
        if (files_identical(full, self)) {
            con_printf(herr, L"selflog: %ls 已经是 selflog 副本了\n", full);
            return 2;
        }
        if (!MoveFileW(full, real)) {
            con_printf(herr, L"selflog: 重命名失败（错误 %lu），可能有权限或文件占用问题\n", GetLastError());
            return 3;
        }
        if (!CopyFileW(self, full, FALSE)) {
            con_printf(herr, L"selflog: 复制副本失败（错误 %lu），正在回滚\n", GetLastError());
            MoveFileW(real, full);
            return 3;
        }
        con_printf(GetStdHandle(STD_OUTPUT_HANDLE),
                   L"[selflog] 已安装代理\n  真身: %ls\n  替身: %ls\n"
                   L"  之后凡是以 %ls 启动的进程，参数与全部 I/O 都会记录到同目录的 selflog.log\n",
                   real, full, stem);
    } else {
        if (!file_exists(real)) {
            con_printf(herr, L"selflog: 找不到真身 %ls，无法恢复\n", real);
            return 2;
        }
        if (!files_identical(full, self)) {
            con_printf(herr, L"selflog: %ls 不是当前 selflog 的副本，拒绝删除（请手工处理）\n", full);
            return 2;
        }
        if (!DeleteFileW(full)) {
            con_printf(herr, L"selflog: 删除替身失败（错误 %lu），请先关闭正在运行的它\n", GetLastError());
            return 3;
        }
        if (!MoveFileW(real, full)) {
            con_printf(herr, L"selflog: 替身已删除，但恢复真身失败（错误 %lu）；原文件仍在 %ls\n",
                       GetLastError(), real);
            return 3;
        }
        con_printf(GetStdHandle(STD_OUTPUT_HANDLE),
                   L"[selflog] 已卸载代理，%ls 恢复为原始程序\n", full);
    }

    /* 顺带在日志里留一笔，日后能对上"什么时候装的" */
    if (resolve_log(NULL, 0, 1)) {
        char ts[48];
        fmt_local(ts, sizeof ts);
        char *ut = w2u(full), *ur = w2u(real);
        log_printf("\n%s  %s\n  target : %s\n  real   : %s\n",
                   uninstall ? "UNINSTALL" : "INSTALL", ts, ut ? ut : "", ur ? ur : "");
        free(ut);
        free(ur);
        fclose(g_log);
        g_log = NULL;
    }
    return 0;
}

/* ===================================================================== main */
int wmain(int argc, wchar_t **argv)
{
    Opt     opt;
    int     exit_code = 0, proxy = 0;
    wchar_t real[MAX_PATH * 2];
    HANDLE  herr = GetStdHandle(STD_ERROR_HANDLE);

    g_t0 = GetTickCount64();
    InitializeCriticalSection(&g_lock);

    /* 代理嵌套保护：_real 万一指回自己，会一层层套下去直到栈/句柄耗尽 */
    g_depth = env_int(L"SELFLOG_DEPTH", 0);
    if (g_depth > 3) {
        con_printf(herr, L"selflog: 代理嵌套过深（%d 层），疑似 _real 指回了自己，已退出\n", g_depth);
        DeleteCriticalSection(&g_lock);
        return 4;
    }

    /*
     * 先是透明代理的判定，再才是命令行解析——顺序不能反。
     * 同目录存在 <自身名>_real.exe 说明自己是顶替上去的替身，此时一个参数都不能
     * 解析：命令行必须原样交给真身，否则被代理程序会收到它不认识的选项。
     * 所以代理模式的配置只来自环境变量（SELFLOG_*）。
     */
    if (find_real(real, MAX_PATH * 2)) {
        proxy = 1;
        opt_from_env(&opt);
    } else if (!parse_args(argc, argv, &opt)) {
        DeleteCriticalSection(&g_lock);
        return 2;
    }

    if (!proxy) {
        if (opt.help) {
            usage(GetStdHandle(STD_OUTPUT_HANDLE));
            DeleteCriticalSection(&g_lock);
            return 0;
        }
        if (opt.version) {
            con_printf(GetStdHandle(STD_OUTPUT_HANDLE), L"selflog %hs\n", SELFLOG_VER);
            DeleteCriticalSection(&g_lock);
            return 0;
        }
        if (opt.action) {
            int rc = do_install(opt.action_arg, opt.action == 2);
            DeleteCriticalSection(&g_lock);
            return rc;
        }
    }

    g_raw    = opt.raw;
    g_cp     = (opt.cp > 0) ? (UINT)opt.cp : 0;
    g_noecho = opt.no_echo;
    g_quiet  = opt.quiet;

    if (!resolve_log(opt.log_path, opt.truncate, opt.quiet)) {
        DeleteCriticalSection(&g_lock);
        return 3;
    }
    if (log_is_empty(g_log)) log_bytes("\xEF\xBB\xBF", 3);   /* UTF-8 BOM，记事本友好 */

    /* 代理模式下不往被代理程序的标准错误里掺自己的提示 */
    if (!opt.quiet && !proxy)
        con_printf(herr, L"[selflog] 日志: %ls\n", g_logpath);

    if (proxy) {
        log_header(L"proxy (transparent forward)", &opt);
        log_kv_w("proxy_of", (argc > 0 && argv[0]) ? argv[0] : L"");
        log_kv_w("real_exe", real);
        log_printf("%-14s: %d\n", "fwd_argc", argc > 0 ? argc - 1 : 0);
        for (int i = 1; i < argc; i++) {              /* 原样转发给真身的参数 */
            char key[48];
            snprintf(key, sizeof key, "fwd[%d]", i);
            log_kv_w(key, argv[i]);
        }
        log_printf("--------------------------------------------------------------------------------\n");

        {
            wchar_t **args = (wchar_t **)malloc(sizeof(wchar_t *) * (size_t)(argc > 0 ? argc : 1));
            if (!args) {
                log_printf("ERROR: out of memory\n");
                log_footer(3);
                fclose(g_log);
                DeleteCriticalSection(&g_lock);
                return 3;
            }
            args[0] = real;                            /* argv[0] 换成真身路径 */
            for (int i = 1; i < argc; i++) args[i] = argv[i];
            exit_code = run_child(args, argc);
            free(args);
        }
    } else if (opt.child && opt.child_argc > 0) {
        log_header(L"wrapper (explicit child)", &opt);
        log_argv("child_", opt.child_argc, opt.child);
        exit_code = run_child(opt.child, opt.child_argc);
    } else {
        /* 名字被改成别的、却又找不到 _real：多半是替身被孤立了，得说一声，
           否则用户会以为程序卡住（自身模式在控制台下就是等输入） */
        {
            wchar_t self[MAX_PATH * 2], stem[MAX_PATH];
            if (GetModuleFileNameW(NULL, self, MAX_PATH * 2)) {
                path_stem(self, stem, MAX_PATH);
                if (_wcsicmp(stem, L"selflog") != 0 && !opt.quiet)
                    con_printf(herr,
                               L"selflog: 同目录没有 %ls_real.exe，退化为自身模式（只记录本进程）\n",
                               stem);
            }
        }
        log_header(L"self", &opt);
        log_argv("", argc, argv);
        log_printf("--------------------------------------------------------------------------------\n");
        exit_code = run_self(&opt);
    }

    log_footer(exit_code);
    if (g_log) { fclose(g_log); g_log = NULL; }

    DeleteCriticalSection(&g_lock);
    return exit_code;
}
