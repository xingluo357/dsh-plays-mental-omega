// 测试宿主：一个最小 32 位窗口程序，用来验证 mo_hint.dll 的分层覆盖窗口
// 编译: cl /nologo /utf-8 /Fe:build\host.exe tools\host.cpp user32.lib gdi32.lib
#include <windows.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HintHost";
    wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, L"HintHost", L"mo-hint test host", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             40, 40, 1600, 900, NULL, NULL, inst, NULL);
    ShowWindow(h, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
