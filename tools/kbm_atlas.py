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


# Ícones de controle nas mesmas células dos de teclado, com o botão que corresponde ao do PlayStation (no GoW1
# E = R2, no GoW2 E = R1: um atlas por jogo):
#   --xbox <Xbox Series/Double do Kenney Input Prompts (CC0)>: controle XInput -> texpack_xbox, no estilo do controle
#     do Xbox Series (botão escuro, letra colorida em A/B/X/Y, símbolo branco no resto).
#   --ps5 <PlayStation Series/Double do Kenney Input Prompts (CC0)>: controle da Sony -> texpack_ps5, no estilo do
#     PS5 (símbolo branco em disco escuro).
PAD = {
    'gow1': {'SPACE': 'CROSS', 'RMB': 'TRIANGLE', 'F': 'CIRCLE', 'LMB': 'SQUARE', 'E': 'R2', 'Q': 'L1', 'CTRL': 'L2',
             'R': 'R1', 'TAB': 'START', 'ESC': 'SELECT', 'WASD': 'LS', 'SHIFT': 'RS', '1': 'UP', '2': 'RIGHT',
             '3': 'DOWN', '4': 'LEFT'},
}
PAD['gow2'] = dict(PAD['gow1'], E='R1', R='R2')
XSERIES = {'CROSS': ('xbox_button_color_a', (96, 178, 40)), 'CIRCLE': ('xbox_button_color_b', (232, 64, 52)),
           'SQUARE': ('xbox_button_color_x', (30, 150, 230)), 'TRIANGLE': ('xbox_button_color_y', (250, 180, 20)),
           'L1': ('xbox_lb', None), 'L2': ('xbox_lt', None), 'R1': ('xbox_rb', None), 'R2': ('xbox_rt', None),
           'START': ('xbox_button_menu', None), 'SELECT': ('xbox_button_view', None), 'LS': ('xbox_stick_l', None),
           'RS': ('xbox_stick_r', None), 'UP': ('xbox_dpad_round_up', None), 'RIGHT': ('xbox_dpad_round_right', None),
           'DOWN': ('xbox_dpad_round_down', None), 'LEFT': ('xbox_dpad_round_left', None)}
PS5 = {'CROSS': 'playstation_button_cross', 'CIRCLE': 'playstation_button_circle', 'SQUARE': 'playstation_button_square',
       'TRIANGLE': 'playstation_button_triangle', 'L1': 'playstation_trigger_l1_alternative',
       'L2': 'playstation_trigger_l2_alternative', 'R1': 'playstation_trigger_r1_alternative',
       'R2': 'playstation_trigger_r2_alternative', 'START': 'playstation5_button_options',
       'SELECT': 'playstation5_button_create', 'LS': 'playstation_stick_l', 'RS': 'playstation_stick_r',
       'UP': 'playstation_dpad_up', 'RIGHT': 'playstation_dpad_right', 'DOWN': 'playstation_dpad_down',
       'LEFT': 'playstation_dpad_left'}


def _crop(im):
    a = im.getchannel('A')
    if a.getextrema()[1] <= 130:  # alfa do PS2 (128 = opaco)
        im.putalpha(a.point(lambda v: min(255, v * 2)))
    return im.crop(im.getchannel('A').point(lambda v: 255 if v > 24 else 0).getbbox())


def dark_style(im, sym=(245, 245, 245)):
    """Ícone do Kenney (branco ou colorido, símbolo vazado) -> botão escuro com o símbolo na cor sym e aro branco."""
    from scipy import ndimage
    a = np.asarray(im, np.float32)[..., 3] / 255
    solid = ndimage.binary_fill_holes(a > 0.5)
    sil = np.asarray(Image.fromarray((solid * 255).astype(np.uint8)).filter(__import__('PIL.ImageFilter').ImageFilter.GaussianBlur(0.8)), np.float32) / 255
    hole = np.clip(sil - a, 0, 1)                      # o símbolo (vazado no original)
    ring = sil - np.asarray(Image.fromarray((ndimage.binary_erosion(solid, iterations=max(2, im.width // 40)) * 255).astype(np.uint8)), np.float32) / 255
    white = np.clip(hole + np.clip(ring, 0, 1), 0, 1)
    out = np.zeros(a.shape + (4,), np.float32)
    col = np.where(hole[..., None] > np.clip(ring, 0, 1)[..., None], np.array(sym, np.float32), 245.0)
    out[..., :3] = 34 + white[..., None] * (col - 34)
    out[..., 3] = sil * 255
    return _crop(Image.fromarray(out.astype(np.uint8), 'RGBA'))


def ps5_png(folder, name):
    im = Image.open(os.path.join(folder, PS5[name] + '.png')).convert('RGBA')
    if name in ('UP', 'DOWN', 'LEFT', 'RIGHT'):  # direcional: o original já tem o braço vermelho; fica como é
        return _crop(im)
    return dark_style(im)


def xbox_png(folder, name):
    fn, col = XSERIES[name]
    im = Image.open(os.path.join(folder, fn + '.png')).convert('RGBA')
    if name in ('UP', 'DOWN', 'LEFT', 'RIGHT'):
        return _crop(im)
    return dark_style(im, col or (245, 245, 245))


def fit(im, w, h):
    k = min(w / im.width, h / im.height)
    im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    out.alpha_composite(im, ((w - im.width) // 2, (h - im.height) // 2))
    return out


def build_pad(src_rgba, game, icon):
    a = np.frombuffer(open(src_rgba, 'rb').read()[:256 * 256 * 4], np.uint8).reshape(256, 256, 4)
    atlas = Image.fromarray(a.copy(), 'RGBA').resize((256 * SCALE, 256 * SCALE), Image.LANCZOS)
    for label, g in ICON_GLYPH.items():
        u0, u1, v0, v1 = CELLS[g]
        atlas.paste((0, 0, 0, 0), (round((u0 - 1) * SCALE), round((v0 - 1) * SCALE), round((u1 + 1) * SCALE), round((v1 + 1) * SCALE)))
        x0, x1 = round(u0 * SCALE + 1), round(u1 * SCALE - 1)
        y0, y1 = round(v0 * SCALE + 1), round(v1 * SCALE - 1)
        atlas.alpha_composite(fit(icon(PAD[game][label]), x1 - x0, y1 - y0), (x0, y0))
    return atlas


def main():
    src, data = sys.argv[1], sys.argv[2]
    for flag, sub, png in (('--xbox', 'texpack_xbox', 'font_xbox.png'), ('--ps5', 'texpack_ps5', 'font_ps5.png')):
        if flag not in sys.argv:
            continue
        k = sys.argv.index(flag)
        folder = sys.argv[k + 1]
        icon = (lambda n: xbox_png(folder, n)) if flag == '--xbox' else (lambda n: ps5_png(folder, n))
        for game in ('gow1', 'gow2'):
            d = os.path.join(data, sub, game)
            os.makedirs(d, exist_ok=True)
            build_pad(src, game, icon).save(os.path.join(d, png))
            with open(os.path.join(d, 'index.tsv'), 'w', encoding='utf-8') as fo:
                fo.write('# atlas da fonte do HUD com os botões do controle (tools/kbm_atlas.py %s)\n' % flag)
                fo.write('%s\t%s\n' % (FONT_KEY, png))
        print(sub, 'gow1 e gow2')
        return
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
