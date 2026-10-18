"""Desenho dos ícones de teclado e mouse (tecla com rótulo, mouse com o botão aceso) para o
texpack_kbm (ver runtime/vitart/hle_gxm.cpp e tools/kbm_glyphs.py).

    key_icon(size, 'Q')        tecla quadrada com a letra
    key_icon(size, 'SPACE')    rótulo longo: a fonte diminui para caber
    mouse_icon(size, 'left')   mouse com o botão esquerdo / direito / do meio aceso
"""
from PIL import Image, ImageDraw, ImageFont

FONT = 'C:/Windows/Fonts/arialbd.ttf'
SS = 4  # superamostragem: desenha 4x maior e reduz (bordas suaves)


def _font(px):
    return ImageFont.truetype(FONT, max(4, int(px)))


def key_icon(w, h, label, fill=(30, 30, 34), edge=(225, 225, 230), text=(245, 245, 245)):
    """Tecla arredondada ocupando w x h (com margem), rótulo centrado no maior tamanho que couber."""
    W, H = w * SS, h * SS
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = max(1, round(min(W, H) * 0.06))
    r = round(min(W, H) * 0.22)
    lw = max(SS, round(min(W, H) * 0.075))
    d.rounded_rectangle((m, m, W - 1 - m, H - 1 - m), radius=r, fill=fill + (255,), outline=edge + (255,), width=lw)
    # face da tecla um pouco mais clara em cima (relevo discreto)
    inner = (m + lw * 2, m + lw * 2, W - 1 - m - lw * 2, round(H * 0.55))
    if inner[2] > inner[0] and inner[3] > inner[1]:
        ov = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(ov).rounded_rectangle(inner, radius=max(1, r // 2), fill=(255, 255, 255, 22))
        im = Image.alpha_composite(im, ov)
        d = ImageDraw.Draw(im)
    box_w, box_h = W - 2 * (m + lw * 2), H - 2 * (m + lw * 2)
    size = box_h * (0.78 if len(label) == 1 else 0.6)
    while size > 4:
        f = _font(size)
        l, t, rr, b = d.textbbox((0, 0), label, font=f)
        if rr - l <= box_w * 0.92 and b - t <= box_h * 0.9:
            break
        size *= 0.92
    f = _font(size)
    l, t, rr, b = d.textbbox((0, 0), label, font=f)
    d.text(((W - (rr - l)) / 2 - l, (H - (b - t)) / 2 - t), label, font=f, fill=text + (255,))
    return im.resize((w, h), Image.LANCZOS)


def mouse_icon(w, h, button, body=(30, 30, 34), edge=(225, 225, 230), lit=(235, 60, 50)):
    """Mouse de frente; o botão pedido aceso (vermelho)."""
    W, H = w * SS, h * SS
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    bw = min(W * 0.66, H * 0.62)
    bh = bw * 1.45
    if bh > H * 0.92:
        bh = H * 0.92
        bw = bh / 1.45
    x0, y0 = (W - bw) / 2, (H - bh) / 2
    x1, y1 = x0 + bw, y0 + bh
    lw = max(SS, round(bw * 0.07))
    d.rounded_rectangle((x0, y0, x1, y1), radius=bw * 0.48, fill=body + (255,), outline=edge + (255,), width=lw)
    split = y0 + bh * 0.42  # fim dos botões
    mid = (x0 + x1) / 2
    # botão aceso: recorte do corpo
    if button in ('left', 'right'):
        mask = Image.new('L', (W, H), 0)
        md = ImageDraw.Draw(mask)
        md.rounded_rectangle((x0 + lw, y0 + lw, x1 - lw, y1 - lw), radius=bw * 0.44, fill=255)
        side = Image.new('L', (W, H), 0)
        sd = ImageDraw.Draw(side)
        if button == 'left':
            sd.rectangle((0, 0, mid - lw / 2, split - lw / 2), fill=255)
        else:
            sd.rectangle((mid + lw / 2, 0, W, split - lw / 2), fill=255)
        from PIL import ImageChops
        m2 = ImageChops.multiply(mask, side)
        im.paste(Image.new('RGBA', (W, H), lit + (255,)), (0, 0), m2)
        d = ImageDraw.Draw(im)
    d.line((x0 + lw, split, x1 - lw, split), fill=edge + (255,), width=lw)
    d.line((mid, y0 + lw, mid, split), fill=edge + (255,), width=lw)
    # roda
    rw, rh = bw * 0.16, bh * 0.2
    wy0 = y0 + bh * 0.1
    d.rounded_rectangle((mid - rw / 2, wy0, mid + rw / 2, wy0 + rh), radius=rw / 2,
                        fill=(lit if button == 'middle' else body) + (255,), outline=edge + (255,), width=max(SS, lw * 2 // 3))
    return im.resize((w, h), Image.LANCZOS)


if __name__ == '__main__':
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else 'kbm_preview.png'
    tiles = [key_icon(64, 64, c) for c in ('Q', 'E', 'R', 'F', 'ESC', 'TAB', 'CTRL')]
    tiles += [key_icon(128, 64, 'SPACE')] + [mouse_icon(64, 64, b) for b in ('left', 'right', 'middle')]
    W = sum(t.width for t in tiles) + 8 * len(tiles)
    sheet = Image.new('RGBA', (W, 72), (90, 60, 40, 255))
    x = 4
    for t in tiles:
        sheet.alpha_composite(t, (x, 4))
        x += t.width + 8
    sheet.save(out)
    print(out)
