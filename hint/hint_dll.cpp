// mo-hint.dll —— 在游戏屏幕中左侧显示一段由 AI 控制的提示文字
//
// 做法：注入后在游戏进程里创建一个「分层窗口」（WS_EX_LAYERED 全透明窗口），
// 用 UpdateLayeredWindow 把「半透明黑底 + 白字」画上去。不碰游戏的渲染表面，
// 所以不会拖慢游戏、也不会有画面撕裂；只要游戏是窗口化/无边框（本机 ddraw 就是
// fullscreen=false, border=True），这个窗口就浮在游戏上面。
//
// 与 AI 的接口：命名管道 \\.\pipe\<dll 文件名>（mo_hint.dll -> mo-hint）
//   say <文本>        立刻显示这段文字（清掉队列）
//   push <文本>       进队列，按顺序显示（每条 8 秒后淡出）
//   next              跳过当前这条
//   clear             清空并隐藏
//   pos <x> <y>       面板左上角；y 传 -1 表示垂直居中（默认 x=48 y=-1）
//   size <pt>         字号（默认 34）
//   info              查询当前状态
//   test              显示一条示例
//
// 文本用 UTF-8 传入，内部转 UTF-16 用微软雅黑渲染（支持中文）。

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

// ---------- 共享状态（管道线程写，UI 线程读） ----------
static CRITICAL_SECTION g_lock;
static char  g_cur[1024] = {0};     // 当前要显示的文字（UTF-8）
static int   g_has = 0;             // 有内容可显示
static DWORD g_shownAt = 0;         // 本次显示的起始 tick
static int   g_alphaPct = 0;        // 0..100，当前不透明度
static int   g_posX = 48;
static int   g_posY = -1;           // -1 = 垂直居中
static int   g_fontPt = 34;
static HWND  g_hwnd = NULL;
static volatile LONG g_stop = 0;

#define QUEUE_MAX 32
static char  g_queue[QUEUE_MAX][1024];
static int   g_qHead = 0, g_qCount = 0;

#define HOLD_MS  8000
#define FADE_MS  700

static void Lock()   { EnterCriticalSection(&g_lock); }
static void Unlock() { LeaveCriticalSection(&g_lock); }

static void Log(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strncat_s(path, sizeof(path), "mo-hint.log", _TRUNCATE);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        fprintf(f, "[%lu] %s\n", GetTickCount(), line);
        fclose(f);
    }
}

static void QueuePush(const char* s)
{
    Lock();
    if (g_qCount >= QUEUE_MAX) {            // 队列满：丢掉最前面那条
        g_qHead = (g_qHead + 1) % QUEUE_MAX;
        --g_qCount;
    }
    int slot = (g_qHead + g_qCount) % QUEUE_MAX;
    strncpy_s(g_queue[slot], sizeof(g_queue[slot]), s, _TRUNCATE);
    ++g_qCount;
    Unlock();
}

static int QueuePop(char* out, size_t cap)
{
    Lock();
    int got = 0;
    if (g_qCount > 0) {
        strncpy_s(out, cap, g_queue[g_qHead], _TRUNCATE);
        g_qHead = (g_qHead + 1) % QUEUE_MAX;
        --g_qCount;
        got = 1;
    }
    Unlock();
    return got;
}

// 显示下一条：有队列就取一条，否则隐藏
static void Advance()
{
    Lock();
    // 当前这条已结束，取队列下一条
    char next[1024] = {0};
    int got = 0;
    if (g_qCount > 0) {
        strncpy_s(next, sizeof(next), g_queue[g_qHead], _TRUNCATE);
        g_qHead = (g_qHead + 1) % QUEUE_MAX;
        --g_qCount;
        got = 1;
    }
    if (got) {
        strncpy_s(g_cur, sizeof(g_cur), next, _TRUNCATE);
        g_has = 1;
        g_shownAt = GetTickCount();
        g_alphaPct = 100;
        Log("advance -> show \"%s\" left=%d", g_cur, g_qCount);
    } else {
        g_cur[0] = 0;
        g_has = 0;
        g_alphaPct = 0;
        Log("advance -> empty, hide");
    }
    Unlock();
    if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 1, 0, 0);   // 请求重绘
}

// ---------- 绘制 ----------
static void Utf8ToWide(const char* s, wchar_t* out, int outCap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outCap);
    if (n <= 0) out[0] = 0;
}

// 把「文字掩膜」和「面板掩膜」合成为预乘 alpha 的 BGRA 位图并贴到分层窗口
static void Paint()
{
    if (!g_hwnd) return;

    char text[1024] = {0};
    int alpha = 0, px = 0, py = 0, pt = 0;
    Lock();
    strncpy_s(text, sizeof(text), g_cur, _TRUNCATE);
    alpha = g_alphaPct;
    px = g_posX; py = g_posY; pt = g_fontPt;
    Unlock();

    if (!text[0] || alpha <= 0) {
        ShowWindow(g_hwnd, SW_HIDE);
        return;
    }

    wchar_t wtext[1024];
    Utf8ToWide(text, wtext, 1024);

    HDC screen = GetDC(NULL);
    int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);

    // --- 先量文字尺寸 ---
    HDC measure = CreateCompatibleDC(screen);
    HFONT font = CreateFontW(-pt, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(measure, font);
    RECT rc = { 0, 0, scrW - 2 * px, 0 };
    DrawTextW(measure, wtext, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int textW = rc.right - rc.left, textH = rc.bottom - rc.top;
    SelectObject(measure, oldFont);
    DeleteObject(font);
    DeleteDC(measure);

    const int padX = 22, padY = 16;
    int w = textW + padX * 2;
    int h = textH + padY * 2;
    if (w < 120) w = 120;
    if (w > scrW - 2 * px) w = scrW - 2 * px;
    int x = px;
    int y = (py < 0) ? (scrH - h) / 2 : py;
    if (x + w > scrW) x = scrW - w;
    if (y < 0) y = 0;

    // --- 画文字掩膜（白字黑底） ---
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                 // 顶朝下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bitsText = NULL;
    HDC mdc = CreateCompatibleDC(screen);
    HBITMAP dibText = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bitsText, NULL, 0);
    HGDIOBJ oldBmp = SelectObject(mdc, dibText);
    memset(bitsText, 0, (size_t)w * h * 4);
    font = CreateFontW(-pt, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    oldFont = SelectObject(mdc, font);
    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, RGB(255, 255, 255));
    RECT tr = { padX, padY, padX + textW, padY + textH };
    DrawTextW(mdc, wtext, -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(mdc, oldFont);
    DeleteObject(font);
    SelectObject(mdc, oldBmp);
    DeleteDC(mdc);

    // --- 画面板掩膜（圆角矩形白块） ---
    void* bitsPanel = NULL;
    HDC pdc = CreateCompatibleDC(screen);
    HBITMAP dibPanel = CreateDIBSection(pdc, &bi, DIB_RGB_COLORS, &bitsPanel, NULL, 0);
    oldBmp = SelectObject(pdc, dibPanel);
    memset(bitsPanel, 0, (size_t)w * h * 4);
    HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldBr = SelectObject(pdc, br);
    HGDIOBJ oldPen = SelectObject(pdc, GetStockObject(NULL_PEN));
    RoundRect(pdc, 0, 0, w, h, 18, 18);
    SelectObject(pdc, oldPen);
    SelectObject(pdc, oldBr);
    DeleteObject(br);
    SelectObject(pdc, oldBmp);
    DeleteDC(pdc);

    // --- 合成到最终 32bpp 预乘位图 ---
    void* bitsOut = NULL;
    HDC odc = CreateCompatibleDC(screen);
    HBITMAP dibOut = CreateDIBSection(odc, &bi, DIB_RGB_COLORS, &bitsOut, NULL, 0);
    oldBmp = SelectObject(odc, dibOut);

    unsigned char* pt8 = (unsigned char*)bitsText;
    unsigned char* pp8 = (unsigned char*)bitsPanel;
    unsigned char* po8 = (unsigned char*)bitsOut;
    const int panelA = 165;            // 面板不透明度（0..255）
    const int fade = alpha;            // 全局淡出系数（0..100）

    for (int i = 0; i < w * h; ++i) {
        int aText = pt8[i * 4 + 1];                       // 文字掩膜亮度
        int aPanel = pp8[i * 4 + 1] * panelA / 255;        // 面板覆盖 x 不透明度
        int aOut = aText + aPanel * (255 - aText) / 255;   // 叠加后不透明度
        if (aOut <= 0) { po8[i * 4 + 0] = po8[i * 4 + 1] = po8[i * 4 + 2] = po8[i * 4 + 3] = 0; continue; }
        // 颜色：白字 + 深灰面板，按覆盖率加权
        int wr = 255, wg = 255, wb = 255;
        int pr = 0x10, pg = 0x12, pb = 0x14;
        int cr = (wr * aText + pr * aPanel * (255 - aText) / 255) / aOut;
        int cg = (wg * aText + pg * aPanel * (255 - aText) / 255) / aOut;
        int cb = (wb * aText + pb * aPanel * (255 - aText) / 255) / aOut;
        int a = aOut * fade / 100;
        po8[i * 4 + 0] = (unsigned char)(cb * a / 255);   // B
        po8[i * 4 + 1] = (unsigned char)(cg * a / 255);   // G
        po8[i * 4 + 2] = (unsigned char)(cr * a / 255);   // R
        po8[i * 4 + 3] = (unsigned char)a;                // A
    }
    SelectObject(odc, oldBmp);
    DeleteDC(odc);

    // --- 贴到分层窗口 ---
    POINT dst = { x, y };
    SIZE  sz = { w, h };
    POINT src = { 0, 0 };
    HDC sdc = GetDC(NULL);
    HDC mem = CreateCompatibleDC(sdc);
    HGDIOBJ old2 = SelectObject(mem, dibOut);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, sdc, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old2);
    DeleteDC(mem);
    ReleaseDC(NULL, sdc);

    DeleteObject(dibOut);
    DeleteObject(dibText);
    DeleteObject(dibPanel);
    ReleaseDC(NULL, screen);

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
}

// ---------- 窗口 ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP + 1:          // 内容变化 → 重绘
        Paint();
        return 0;
    case WM_TIMER: {
        DWORD now = GetTickCount();
        int changed = 0, done = 0;
        Lock();
        if (g_has) {
            DWORD el = now - g_shownAt;
            int want;
            if (el < HOLD_MS) want = 100;
            else if (el < HOLD_MS + FADE_MS) want = (int)(100 - (el - HOLD_MS) * 100 / FADE_MS);
            else { want = 0; done = 1; }
            if (want != g_alphaPct) { g_alphaPct = want; changed = 1; }
            if (done) g_has = 0;
        }
        Unlock();
        if (done) { Log("timer: text expired"); Advance(); }
        else if (changed) Paint();
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI UiThread(LPVOID)
{
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MoHintOverlay";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"MoHintOverlay", L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) return 1;

    SetTimer(g_hwnd, 1, 40, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

// ---------- 管道 ----------
static void Handle(char* line, char* resp, size_t cap)
{
    while (*line == ' ') ++line;
    if (!*line) { _snprintf_s(resp, cap, _TRUNCATE, "err empty"); return; }

    if (_stricmp(line, "ping") == 0) {
        _snprintf_s(resp, cap, _TRUNCATE, "ok pong");
    } else if (_strnicmp(line, "say ", 4) == 0) {
        const char* t = line + 4;
        Lock();
        strncpy_s(g_cur, sizeof(g_cur), t, _TRUNCATE);
        g_has = 1;
        g_shownAt = GetTickCount();
        g_alphaPct = 100;
        g_qHead = 0; g_qCount = 0;      // say 覆盖队列
        Unlock();
        if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 1, 0, 0);
        _snprintf_s(resp, cap, _TRUNCATE, "ok say len=%d", (int)strlen(t));
    } else if (_strnicmp(line, "push ", 5) == 0) {
        QueuePush(line + 5);
        int q;
        Lock(); q = g_qCount; Unlock();
        if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 1, 0, 0);
        _snprintf_s(resp, cap, _TRUNCATE, "ok push queued=%d", q);
    } else if (_stricmp(line, "next") == 0) {
        Lock(); g_shownAt = 0; g_alphaPct = 0; g_has = 0; Unlock();
        Advance();
        _snprintf_s(resp, cap, _TRUNCATE, "ok next");
    } else if (_stricmp(line, "clear") == 0) {
        Lock();
        g_cur[0] = 0; g_has = 0; g_alphaPct = 0; g_qHead = 0; g_qCount = 0;
        Unlock();
        if (g_hwnd) { ShowWindow(g_hwnd, SW_HIDE); }
        _snprintf_s(resp, cap, _TRUNCATE, "ok clear");
    } else if (_strnicmp(line, "pos ", 4) == 0) {
        int x = 48, y = -1;
        sscanf_s(line + 4, "%d %d", &x, &y);
        Lock(); g_posX = x; g_posY = y; Unlock();
        if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 1, 0, 0);
        _snprintf_s(resp, cap, _TRUNCATE, "ok pos=%d,%d", x, y);
    } else if (_strnicmp(line, "size ", 5) == 0) {
        int pt = 34;
        sscanf_s(line + 5, "%d", &pt);
        if (pt < 10) pt = 10;
        if (pt > 120) pt = 120;
        Lock(); g_fontPt = pt; Unlock();
        if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 1, 0, 0);
        _snprintf_s(resp, cap, _TRUNCATE, "ok size=%d", pt);
    } else if (_stricmp(line, "test") == 0) {
        Lock();
        strncpy_s(g_cur, sizeof(g_cur), "测试：你要建造 发电厂", _TRUNCATE);
        g_has = 1; g_shownAt = GetTickCount(); g_alphaPct = 100;
        Unlock();
        if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 1, 0, 0);
        _snprintf_s(resp, cap, _TRUNCATE, "ok test");
    } else if (_stricmp(line, "info") == 0) {
        Lock();
        _snprintf_s(resp, cap, _TRUNCATE, "ok hwnd=%p text=\"%s\" queued=%d alpha=%d pos=%d,%d size=%d",
                    (void*)g_hwnd, g_cur, g_qCount, g_alphaPct, g_posX, g_posY, g_fontPt);
        Unlock();
    } else {
        _snprintf_s(resp, cap, _TRUNCATE, "err unknown command");
    }
}

// 管道名由 DLL 文件名派生：mo_hint.dll -> \\.\pipe\mo-hint
static const wchar_t* PipeName()
{
    static wchar_t name[256];
    if (name[0]) return name;
    HMODULE hm = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&PipeName, &hm);
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(hm, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    char stem[200] = {0};
    strncpy_s(stem, sizeof(stem), base, _TRUNCATE);
    char* dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    for (char* p = stem; *p; ++p) if (*p == '_') *p = '-';
    _snwprintf_s(name, 256, _TRUNCATE, L"\\\\.\\pipe\\%hs", stem);
    return name;
}

static DWORD WINAPI PipeThread(LPVOID)
{
    const wchar_t* pn = PipeName();
    for (;;) {
        HANDLE h = CreateNamedPipeW(pn, PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        BOOL ok = ConnectNamedPipe(h, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!ok) { CloseHandle(h); continue; }

        static char buf[64 * 1024];
        static char resp[64 * 1024];
        DWORD got = 0;
        if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
            buf[got] = 0;
            char* nl = strpbrk(buf, "\r\n");
            if (nl) *nl = 0;
            __try {
                Handle(buf, resp, sizeof(resp));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                _snprintf_s(resp, sizeof(resp), _TRUNCATE, "err exception in handler");
            }
            strncat_s(resp, sizeof(resp), "\n", _TRUNCATE);
            DWORD wrote = 0;
            WriteFile(h, resp, (DWORD)strlen(resp), &wrote, NULL);
        }
        FlushFileBuffers(h);
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    return 0;
}

static DWORD WINAPI Boot(LPVOID)
{
    InitializeCriticalSection(&g_lock);
    CreateThread(NULL, 0, PipeThread, NULL, 0, NULL);
    UiThread(NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        CreateThread(NULL, 0, Boot, NULL, 0, NULL);
    }
    return TRUE;
}
