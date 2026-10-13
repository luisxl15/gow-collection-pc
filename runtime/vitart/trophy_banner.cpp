// Aviso de troféu igual ao do PS Vita (ver trophy_banner.h). Medidas do Vita3K, que reproduz o do console,
// numa tela de 960x544: faixa de 400x60 a 20 px do topo e 12 px da direita, cantos de raio 15, cinza
// claro (0,76; 0,75; 0,76), ícone de 48 px; "(Bronze) Nome" em cima e "Você conquistou um troféu!" menor
// embaixo, em cinza escuro (0,24).
#include "trophy_banner.h"

#include <algorithm>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define STBI_ONLY_PNG
#include "stb_image.h" // implementação em hle_gxm.cpp

namespace gvk {

static std::wstring widen(const std::string &s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static HFONT make_font(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

bool render_trophy_banner(const TrophyBanner &t, uint32_t screen_h, BannerImage &out) {
    const double k = std::max(0.75, screen_h / 544.0);
    const int margin = (int)(12 * k), icon = (int)(48 * k), icon_pad = (int)(6 * k);
    const int W = (int)(400 * k), H = icon + margin, r = (int)(15 * k);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W, bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return DeleteDC(dc), false;
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    uint32_t *px = static_cast<uint32_t *>(bits);
    const uint8_t bgr = 194, bgg = 191, bgb = 194; // GUI_SMOOTH_GRAY do Vita3K
    const uint32_t bg = (uint32_t)bgb | (uint32_t)bgg << 8 | (uint32_t)bgr << 16;
    std::fill(px, px + (size_t)W * H, bg);

    // ícone do troféu (bilinear, alfa sobre o fundo)
    int iw = 0, ih = 0, ic = 0;
    stbi_uc *img = t.icon_png.empty() ? nullptr
                                      : stbi_load_from_memory(t.icon_png.data(), (int)t.icon_png.size(), &iw, &ih, &ic, 4);
    if (img) {
        for (int y = 0; y < icon; y++)
            for (int x = 0; x < icon; x++) {
                const float fx = (x + 0.5f) * iw / icon - 0.5f, fy = (y + 0.5f) * ih / icon - 0.5f;
                const int x0 = std::clamp((int)fx, 0, iw - 1), y0 = std::clamp((int)fy, 0, ih - 1);
                const int x1 = std::min(x0 + 1, iw - 1), y1 = std::min(y0 + 1, ih - 1);
                const float ax = std::clamp(fx - x0, 0.f, 1.f), ay = std::clamp(fy - y0, 0.f, 1.f);
                float c[4];
                for (int ch = 0; ch < 4; ch++) {
                    auto at = [&](int xx, int yy) { return (float)img[((size_t)yy * iw + xx) * 4 + ch]; };
                    c[ch] = (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
                }
                const float a = c[3] / 255.f;
                const uint32_t R = (uint32_t)(c[0] * a + bgr * (1 - a)), G = (uint32_t)(c[1] * a + bgg * (1 - a)),
                               B = (uint32_t)(c[2] * a + bgb * (1 - a));
                px[(size_t)(icon_pad + y) * W + margin + x] = B | G << 8 | R << 16;
            }
        stbi_image_free(img);
    }

    // "(Bronze) Nome" e "Você conquistou um troféu!"
    const char *tier = t.tier == 'P' ? "Platina" : t.tier == 'G' ? "Ouro" : t.tier == 'S' ? "Prata" : "Bronze";
    const std::wstring line1 = widen(std::string("(") + tier + ") " + t.name);
    const int tx = icon + margin * 2, f1px = (int)(15 * k), f2px = (int)(12 * k);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(61, 61, 61));
    HFONT f1 = make_font(f1px, FW_SEMIBOLD), f2 = make_font(f2px, FW_NORMAL);
    HGDIOBJ of = SelectObject(dc, f1);
    RECT r1{tx, margin, W - margin, margin + f1px * 3 / 2};
    DrawTextW(dc, line1.c_str(), -1, &r1, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, f2);
    RECT r2{tx, margin + f1px * 3 / 2, W - margin, H};
    DrawTextW(dc, L"Você conquistou um troféu!", -1, &r2, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    DeleteObject(f1), DeleteObject(f2);
    GdiFlush();

    out.w = (uint32_t)W, out.h = (uint32_t)H, out.radius = (uint32_t)r;
    out.px.assign(px, px + (size_t)W * H);
    for (uint32_t &p : out.px) p |= 0xFF000000u;
    // cantos arredondados: só o trecho de cada linha dentro da faixa é copiado para a tela
    out.row_from.assign(H, 0), out.row_to.assign(H, (uint32_t)W);
    for (int y = 0; y < r && y < H / 2; y++) {
        const double dy = r - y - 0.5;
        const uint32_t cut = (uint32_t)std::ceil(r - std::sqrt(std::max(0.0, (double)r * r - dy * dy)));
        out.row_from[y] = out.row_from[H - 1 - y] = cut;
        out.row_to[y] = out.row_to[H - 1 - y] = (uint32_t)W - cut;
    }
    SelectObject(dc, oldbmp), DeleteObject(bmp), DeleteDC(dc);
    return true;
}

} // namespace gvk
