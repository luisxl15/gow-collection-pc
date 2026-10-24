// Controles do host (ver host_input.h).
//
// XInput: carregado em tempo de execução (xinput1_4, senão xinput9_1_0), até 4 controles.
// Sony: os dispositivos HID com fabricante 0x054C são abertos e lidos numa thread própria; o último
// relatório de entrada de cada um fica guardado e é traduzido em poll(). Layouts dos relatórios:
//   DualShock 4, relatório 0x01 (USB, e o curto do Bluetooth): [1..4] LX LY RX RY, [5] direcional no
//     nibble baixo (0 = cima, horário, 8 = solto) e quadrado/cruz/círculo/triângulo nos bits 4..7,
//     [6] L1 R1 L2 R2 share options L3 R3, [8] [9] gatilhos analógicos; 0x11 (Bluetooth completo) = +2.
//   DualSense, relatório 0x01 (USB): [1..4] analógicos, [5] [6] gatilhos, [8] direcional e botões como
//     no DS4, [9] L1 R1 L2 R2 create options L3 R3; 0x31 (Bluetooth completo) = +1. O 0x01 curto do
//     Bluetooth tem o layout do DS4.
#include "host_input.h"
#include "vitart.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hostin {
namespace {

// Bits do SceCtrlData.
// Bits do SceCtrlData no modo PS TV (controle com L2/R2): L2/R2 = 0x100/0x200 (os L/R do portátil),
// L1/R1 = 0x400/0x800, L3/R3 = 0x2/0x4. No modo portátil (VITART_HANDHELD=1) L1 e L2 viram 0x100.
constexpr uint32_t SELECT = 0x1, L3 = 0x2, R3 = 0x4, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
                   L2 = 0x100, R2 = 0x200, L1 = 0x400, R1 = 0x800, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000,
                   SQUARE = 0x8000;

// ---------------------------------------------------------------- XInput
struct XInputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XInputState {
    DWORD dwPacketNumber;
    XInputGamepad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XInputState *);

XInputGetStateFn xinput() {
    static XInputGetStateFn fn = [] {
        for (const char *dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryA(dll))
                if (auto f = (XInputGetStateFn)GetProcAddress(m, "XInputGetState")) return f;
        return (XInputGetStateFn) nullptr;
    }();
    return fn;
}

uint8_t axis_x(SHORT v) { return (uint8_t)((v + 32768) >> 8); }
uint8_t axis_y(SHORT v) { return (uint8_t)(255 - ((v + 32768) >> 8)); } // XInput: cima positivo

// Zona morta circular (como a do XInput, ~24%): dentro dela o analógico fica centrado.
void deadzone(uint8_t &x, uint8_t &y, int dz) {
    const int dx = x - 128, dy = y - 128;
    if (dx * dx + dy * dy < dz * dz) x = y = 128;
}

// ---------------------------------------------------------------- Sony por HID
struct SonyPad {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool dualsense = false;
    std::string path;
    std::mutex m;
    std::vector<uint8_t> report; // último relatório de entrada
    std::atomic<bool> alive{true};
};
std::mutex g_pads_mutex;
std::vector<std::shared_ptr<SonyPad>> g_pads;

bool sony_pid(USHORT pid, bool &dualsense) {
    switch (pid) {
    case 0x05C4: case 0x09CC: case 0x0BA0: dualsense = false; return true; // DualShock 4 (v1, v2, adaptador)
    case 0x0CE6: case 0x0DF2: dualsense = true; return true;              // DualSense, DualSense Edge
    default: return false;
    }
}

void reader(std::shared_ptr<SonyPad> pad) {
    std::vector<uint8_t> buf(128);
    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    while (pad->alive) {
        DWORD got = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(pad->h, buf.data(), (DWORD)buf.size(), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
        if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_TIMEOUT) {
            CancelIo(pad->h);
            continue;
        }
        if (!GetOverlappedResult(pad->h, &ov, &got, FALSE) || !got) break;
        std::lock_guard<std::mutex> g(pad->m);
        pad->report.assign(buf.begin(), buf.begin() + got);
    }
    CloseHandle(ov.hEvent);
    pad->alive = false; // desconectado: sai da lista na próxima varredura
}

void scan_sony() {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof ifd};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<char> mem(need);
        auto *det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A *>(mem.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, nullptr, nullptr)) continue;
        const std::string path = det->DevicePath;
        {
            std::lock_guard<std::mutex> g(g_pads_mutex);
            if (std::any_of(g_pads.begin(), g_pads.end(), [&](auto &p) { return p->path == path && p->alive; })) continue;
        }
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{sizeof at};
        bool ds = false;
        if (!HidD_GetAttributes(h, &at) || at.VendorID != 0x054C || !sony_pid(at.ProductID, ds)) {
            CloseHandle(h);
            continue;
        }
        // DualShock 4 por Bluetooth só manda o relatório completo depois de ler o recurso 0x02.
        uint8_t feat[64] = {0x02};
        HidD_GetFeature(h, feat, sizeof feat);
        auto pad = std::make_shared<SonyPad>();
        pad->h = h, pad->dualsense = ds, pad->path = path;
        {
            std::lock_guard<std::mutex> g(g_pads_mutex);
            g_pads.push_back(pad);
        }
        std::thread(reader, pad).detach();
        vt::logf("vitart: controle %s conectado (%04x:%04x)", ds ? "DualSense" : "DualShock 4", at.VendorID, at.ProductID);
    }
    SetupDiDestroyDeviceInfoList(set);
}

void sony_state(const std::vector<uint8_t> &r, bool dualsense, State &s) {
    if (r.size() < 10) return;
    size_t b = 1;      // início dos dados
    bool ds_layout = false;
    if (r[0] == 0x11 && r.size() >= 12) b = 3;               // DS4 Bluetooth completo
    else if (r[0] == 0x31 && r.size() >= 12) b = 2, ds_layout = true; // DualSense Bluetooth completo
    else if (r[0] == 0x01) ds_layout = dualsense && r.size() >= 64;   // DualSense USB (o curto do BT é como o DS4)
    else return;
    const uint8_t *d = r.data() + b;
    const uint8_t hat_btn = ds_layout ? d[7] : d[4], misc = ds_layout ? d[8] : d[5];
    s.lx = d[0], s.ly = d[1], s.rx = d[2], s.ry = d[3];
    static const uint32_t hat[9] = {UP, UP | RIGHT, RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT, 0};
    s.buttons |= hat[std::min<uint8_t>(hat_btn & 15, 8)];
    if (hat_btn & 0x10) s.buttons |= SQUARE;
    if (hat_btn & 0x20) s.buttons |= CROSS;
    if (hat_btn & 0x40) s.buttons |= CIRCLE;
    if (hat_btn & 0x80) s.buttons |= TRIANGLE;
    if (misc & 0x01) s.buttons |= L1;
    if (misc & 0x02) s.buttons |= R1;
    if (misc & 0x04) s.buttons |= L2;
    if (misc & 0x08) s.buttons |= R2;
    if (misc & 0x40) s.buttons |= L3;
    if (misc & 0x80) s.buttons |= R3;
    if (misc & 0x10) s.buttons |= SELECT; // share / create
    if (misc & 0x20) s.buttons |= START;  // options
}

int deflection(const State &s) {
    const int a = s.lx - 128, b = s.ly - 128, c = s.rx - 128, e = s.ry - 128;
    return a * a + b * b + c * c + e * e;
}

} // namespace

State poll() {
    State out;
    int best = -1;
    auto take = [&](State s, uint8_t src) {
        out.buttons |= s.buttons;
        if (s.buttons || deflection(s) > 60 * 60) out.active |= src;
        const int dfl = deflection(s);
        if (dfl > best) best = dfl, out.lx = s.lx, out.ly = s.ly, out.rx = s.rx, out.ry = s.ry;
    };
    if (auto get = xinput())
        for (DWORD i = 0; i < 4; i++) {
            XInputState x{};
            static bool seen[4] = {};
            if (get(i, &x) != ERROR_SUCCESS) {
                seen[i] = false;
                continue;
            }
            if (!seen[i]) seen[i] = true, vt::logf("vitart: controle XInput %u conectado", (unsigned)i);
            const XInputGamepad &g = x.Gamepad;
            State s;
            const WORD w = g.wButtons;
            if (w & 0x0001) s.buttons |= UP;
            if (w & 0x0002) s.buttons |= DOWN;
            if (w & 0x0004) s.buttons |= LEFT;
            if (w & 0x0008) s.buttons |= RIGHT;
            if (w & 0x0010) s.buttons |= START;
            if (w & 0x0020) s.buttons |= SELECT;
            if (w & 0x0040) s.buttons |= L3;
            if (w & 0x0080) s.buttons |= R3;
            if (w & 0x0100) s.buttons |= L1; // LB
            if (w & 0x0200) s.buttons |= R1; // RB
            if (g.bLeftTrigger > 64) s.buttons |= L2;
            if (g.bRightTrigger > 64) s.buttons |= R2;
            if (w & 0x1000) s.buttons |= CROSS;    // A
            if (w & 0x2000) s.buttons |= CIRCLE;   // B
            if (w & 0x4000) s.buttons |= SQUARE;   // X
            if (w & 0x8000) s.buttons |= TRIANGLE; // Y
            s.lx = axis_x(g.sThumbLX), s.ly = axis_y(g.sThumbLY), s.rx = axis_x(g.sThumbRX), s.ry = axis_y(g.sThumbRY);
            deadzone(s.lx, s.ly, 30);
            deadzone(s.rx, s.ry, 34);
            take(s, 1);
        }
    // Controles da Sony: varredura a cada 2 s (conectar e desconectar com o jogo aberto).
    static auto last_scan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_scan > std::chrono::seconds(2)) {
        last_scan = now;
        {
            std::lock_guard<std::mutex> g(g_pads_mutex);
            g_pads.erase(std::remove_if(g_pads.begin(), g_pads.end(),
                                        [](auto &p) {
                                            if (p->alive) return false;
                                            CloseHandle(p->h);
                                            return true;
                                        }),
                         g_pads.end());
        }
        scan_sony();
    }
    std::vector<std::shared_ptr<SonyPad>> pads;
    {
        std::lock_guard<std::mutex> g(g_pads_mutex);
        pads = g_pads;
    }
    for (auto &p : pads) {
        std::vector<uint8_t> r;
        {
            std::lock_guard<std::mutex> g(p->m);
            r = p->report;
        }
        if (r.empty()) continue;
        State s;
        sony_state(r, p->dualsense, s);
        deadzone(s.lx, s.ly, 12);
        deadzone(s.rx, s.ry, 12);
        take(s, 2);
    }
    return out;
}

} // namespace hostin
