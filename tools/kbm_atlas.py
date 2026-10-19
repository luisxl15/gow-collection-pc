"""Atlas da fonte do HUD com os ícones de teclado e mouse (texpack_kbm dos dois GoW).

    python tools/kbm_atlas.py <atlas original .rgba 256x256> <pasta vitart-data>

Os ícones dos botões do HUD são malhas sem textura própria; a única textura do HUD para coisas pequenas é
o atlas da fonte (o mesmo nos dois jogos, chave dd387c3eceaf6c63). As letras acentuadas que o jogo em
inglês nunca mostra (glifos 87..106) viram ícones: este script desenha cada ícone (tools/kbm_icons.py) na
célula do glifo, num atlas 4x maior, e grava texpack_kbm/<jogo>/index.tsv. O vitart troca o atlas pelo
de teclado só com o teclado/mouse em uso (runtime/vitart/hle_gxm.cpp) e os ícones do HUD passam a
desenhar esses glifos (tools/kbm_flp.py, runtime/vitart/kbm_icons.cpp).

Células medidas no jogo (texels do atlas 256x256): com o atlas trocado por um "codificado" (vermelho = x,
verde = y), os glifos foram desenhados grandes e a cor nas bordas de cada quadro deu a região.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kbm_icons  # noqa: E402

FONT_KEY = 'dd387c3eceaf6c63'
CELLS = {  # glifo: (u0, u1, v0, v1)
    87: (159.2, 181.5, 57.5, 80.4), 88: (186.3, 208.1, 57.5, 80.3), 89: (2.0, 24.7, 29.9, 54.3),
    90: (28.7, 51.6, 29.9, 54.4), 91: (132.9, 153.7, 30.7, 53.2), 92: (53.1, 71.3, 84.6, 106.8),
    93: (74.8, 94.6, 84.7, 106.8), 94: (156.5, 176.0, 30.5, 54.0), 95: (178.1, 198.1, 30.4, 53.7),
    100: (212.9, 235.0, 57.3, 80.2), 101: (102.5, 127.5, 57.5, 80.4), 102: (129.8, 154.6, 57.4, 80.4),
    103: (157.2, 183.5, 0.8, 26.0), 104: (186.0, 211.6, 0.9, 25.7), 105: (2.6, 24.7, 84.5, 106.9),
    106: (28.9, 49.0, 83.6, 106.3),
}
# ícone de cada rótulo (os outros rótulos, como "1-4", ficam com as letras da fonte em cima do disco)
ICON_GLYPH = {'LMB': 87, 'RMB': 88, 'F': 89, 'Q': 90, 'E': 100, 'R': 101, 'SPACE': 102, 'CTRL': 103, 'TAB': 104,
              'ESC': 105, 'WASD': 106, 'SHIFT': 94, '1': 95, '2': 91, '3': 92, '4': 93}
SCALE = 4


def draw_icon(label, w, h):
    if label in ('LMB', 'RMB'):
        return kbm_icons.mouse_icon(w, h, 'left' if label == 'LMB' else 'right')
    return kbm_icons.key_icon(w, h, label)


def build(src_rgba):
    a = np.frombuffer(open(src_rgba, 'rb').read()[:256 * 256 * 4], np.uint8).reshape(256, 256, 4)
    atlas = Image.fromarray(a.copy(), 'RGBA').resize((256 * SCALE, 256 * SCALE), Image.LANCZOS)
    for label, g in ICON_GLYPH.items():
        u0, u1, v0, v1 = CELLS[g]
        # apaga a letra acentuada com 1 texel de folga (a borda filtrada deixava um traço) e desenha o ícone por dentro
        atlas.paste((0, 0, 0, 0), (round((u0 - 1) * SCALE), round((v0 - 1) * SCALE), round((u1 + 1) * SCALE), round((v1 + 1) * SCALE)))
        x0, x1 = round(u0 * SCALE + 1), round(u1 * SCALE - 1)
        y0, y1 = round(v0 * SCALE + 1), round(v1 * SCALE - 1)
        atlas.alpha_composite(draw_icon(label, x1 - x0, y1 - y0), (x0, y0))
    return atlas


def main():
    src, data = sys.argv[1], sys.argv[2]
    atlas = build(src)
    for game in ('gow1', 'gow2'):
        d = os.path.join(data, 'texpack_kbm', game)
        os.makedirs(d, exist_ok=True)
        atlas.save(os.path.join(d, 'font_kbm.png'))
        with open(os.path.join(d, 'index.tsv'), 'w', encoding='utf-8') as fo:
            fo.write('# atlas da fonte do HUD com os ícones de teclado e mouse (tools/kbm_atlas.py)\n')
            fo.write('%s\tfont_kbm.png\n' % FONT_KEY)
    print('atlas %dx%d, %d ícones' % (atlas.width, atlas.height, len(ICON_GLYPH)))


if __name__ == '__main__':
    main()
