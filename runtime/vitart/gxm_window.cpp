// a janelinha onde o Kratos mora
#include "gxm_window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace gvk::win {
namespace {

HWND g_hwnd = nullptr;
std::wstring g_title;
bool g_closed = false;
std::atomic<bool> g_resized{false};
bool g_fullscreen = false;
WINDOWPLACEMENT g_place{sizeof(WINDOWPLACEMENT)};
// pedido de tela cheia que chegou de outra thread, fica guardado até a hora certa
std::atomic<int> g_want_borderless{-1};

// F11 / Alt+Enter, vai e volta
void toggle_fullscreen(HWND h) {
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        if (!GetWindowPlacement(h, &g_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_place);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
    g_resized = true;
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE:
        g_closed = true;
        return 0;
    case WM_SIZE:
        g_resized = true;
        break;
    case WM_KEYDOWN:
        if (w == VK_F11) return toggle_fullscreen(h), 0;
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) return toggle_fullscreen(h), 0; // Alt+Enter
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN) return 0; // cala a boca do bipe
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

bool g_show_pending = false;
uint32_t g_presented = 0;

std::mutex g_notice_lock;
std::wstring g_notice;
std::chrono::steady_clock::time_point g_notice_end;
bool g_notice_shown = false;
void notice_tick() {
    const std::lock_guard<std::mutex> lock(g_notice_lock);
    if (!g_notice.empty() && !g_notice_shown) {
        SetWindowTextW(g_hwnd, g_notice.c_str());
        g_notice_shown = true;
    } else if (g_notice_shown && std::chrono::steady_clock::now() > g_notice_end) {
        SetWindowTextW(g_hwnd, g_title.c_str());
        g_notice.clear();
        g_notice_shown = false;
    }
}

} // namespace

bool create(const wchar_t *title) {
    g_title = title;
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vitart";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 1440, 816}; // 2x o framebuffer de 720x408
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = r.right - r.left, h = r.bottom - r.top;
    // truque pra não piscar quando troca do menu pro jogo
    const char *wr = getenv("VITART_WINRECT");
    const bool handoff = wr && std::sscanf(wr, "%d,%d,%d,%d", &x, &y, &w, &h) == 4;
    g_show_pending = handoff;
    g_hwnd = CreateWindowExW(0, L"vitart", title, WS_OVERLAPPEDWINDOW | (handoff ? 0 : WS_VISIBLE), x, y, w, h, nullptr,
                             nullptr, wc.hInstance, nullptr);
    return g_hwnd != nullptr;
}

void *hwnd() { return g_hwnd; }

void pump() {
    if (!g_hwnd) return;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    notice_tick();
    if (const int want = g_want_borderless.exchange(-1); want >= 0 && (want == 1) != g_fullscreen) toggle_fullscreen(g_hwnd);
}

bool take_resized() { return g_resized.exchange(false); }
void mark_resized() { g_resized = true; }

void presented() {
    if (!g_show_pending || ++g_presented < 2) return;
    g_show_pending = false;
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (const char *ev = getenv("VITART_HANDOFF"))
        if (HANDLE e = OpenEventA(EVENT_MODIFY_STATE, FALSE, ev)) SetEvent(e), CloseHandle(e);
}

bool closed() { return g_closed; }

bool rect(int &x, int &y, int &w, int &h) {
    RECT r;
    if (!g_hwnd || !GetWindowRect(g_hwnd, &r)) return false;
    x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
    return true;
}

bool pointer(float &nx, float &ny) {
    if (!g_hwnd || GetForegroundWindow() != g_hwnd || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return false;
    POINT p;
    RECT r;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p) || !GetClientRect(g_hwnd, &r)) return false;
    if (p.x < 0 || p.y < 0 || p.x >= r.right || p.y >= r.bottom || r.right <= 0 || r.bottom <= 0) return false;
    nx = (p.x + 0.5f) / (float)r.right, ny = (p.y + 0.5f) / (float)r.bottom;
    return true;
}

bool focused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

void notify(const std::string &text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
    for (wchar_t &c : w)
        if (c == L'\n') c = L' ';
    const std::lock_guard<std::mutex> lock(g_notice_lock);
    g_notice = w;
    g_notice_end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    g_notice_shown = false;
}

void request_borderless(bool on) { g_want_borderless = on ? 1 : 0; }
bool borderless() { return g_fullscreen; }

} // namespace gvk::win
