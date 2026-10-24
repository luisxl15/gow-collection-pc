"""Lâminas de Atena (GoW2, r_weapon0_5 blade5) em HD a partir do pack do PS2.

O pack redesenhou o blade5 como outra arma (lâmina verde, recorte diferente do Vita: no jogo aparece um quadrado
em volta da lâmina). Monta-se então o blade5 com o corpo HD do blade3 do mesmo pack (mesmo recorte do Vita) e as
runas vermelhas viradas azuis, e a faixa de runas, o brilho e a corrente do blade5 do pack, que lá estão nos
mesmos lugares do Vita.

    python tools/texpack_gow2_blade5.py <pasta replacements do pack> <saída.png>
"""
import os
import sys

import numpy as np
from PIL import Image

BLADE3 = '19e2679496596b52-f32134334084aa71-000059d3.png'  # corpo HD (recorte do Vita)
BLADE5 = 'd20471b6ac425b8d-ebd6ac667266cfef-000059d3.png'  # faixa de runas azuis, brilho e corrente
# regiões do blade5 do Vita (128x64) que não são o corpo da lâmina: (x0, y0, x1, y1, fundo preto opaco, azular).
# A faixa de runas e o brilho são preto opaco no Vita (no pack, transparente) e o brilho do pack é amarelo.
FROM_BLADE5 = [(35, 0, 96, 12, True, False), (96, 0, 128, 24, True, True), (99, 48, 128, 64, False, False)]


def find(root, name):
    for dp, _, fs in os.walk(root):
        if name in fs:
            return os.path.join(dp, name)
    raise SystemExit('não achei %s em %s' % (name, root))


def main():
    root, out = sys.argv[1:3]
    body = Image.open(find(root, BLADE3)).convert('RGBA')
    extra = Image.open(find(root, BLADE5)).convert('RGBA').resize(body.size, Image.LANCZOS)
    w, h = body.size
    a = np.asarray(body, dtype=np.float32)
    # alfa do PS2 (128 = opaco) para a escala do Vita
    a[..., 3] = np.clip(a[..., 3] * (255.0 / 128.0), 0, 255) if a[..., 3].max() <= 130 else a[..., 3]
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    # veios vermelho/laranja (r bem acima de g e b) viram azuis trocando R e B; a máscara é suave para não serrilhar
    m = np.clip((r / np.maximum(g, 1.0) - 1.5) / 0.5, 0, 1) * (r > 90) * (r > 1.8 * b)
    m = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).filter(__import__('PIL.ImageFilter').ImageFilter.GaussianBlur(1.5)),
                   dtype=np.float32)[..., None] / 255.0
    swapped = a.copy()
    swapped[..., 0], swapped[..., 2] = b, r
    a = a * (1 - m) + swapped * m
    e = np.asarray(extra, dtype=np.float32)
    sx, sy = w / 128.0, h / 64.0
    for x0, y0, x1, y1, black, blue in FROM_BLADE5:
        X0, Y0, X1, Y1 = int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy)
        reg = e[Y0:Y1, X0:X1].copy()
        if blue:
            reg[..., 0], reg[..., 2] = reg[..., 2].copy(), reg[..., 0].copy()
        if black:
            reg[..., :3] *= reg[..., 3:] / 255.0
            reg[..., :3] *= 1.8  # as runas do Vita são ciano vivo; as do pack, finas e escuras
            reg[..., 3] = 255
        a[Y0:Y1, X0:X1] = reg
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGBA').save(out)
    print(out, w, h)


if __name__ == '__main__':
    main()
