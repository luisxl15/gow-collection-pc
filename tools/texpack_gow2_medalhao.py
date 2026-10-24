"""Meio-medalhões do menu Power Up do GoW2 em HD, com o miolo dourado do Vita.

O pack repintou o miolo dos medalhões de vermelho-escuro: os símbolos que o jogo desenha por cima (o "?" e os
ícones das armas e magias, escuros) somem. Aqui o miolo vermelho vira dourado (matiz e brilho do medalhão do
Vita); a moldura preta e dourada do pack fica como está.

    python tools/texpack_gow2_medalhao.py <pasta replacements do pack> <saída.png> <a|b>
"""
import os
import sys

import numpy as np
from PIL import Image

SRC = {'a': '767671d68185223e-139d752a0b43ec82-0000559b.png', 'b': '19552fa11f1e853c-718f5c7ca1991c6a-0000559b.png'}


def find(root, name):
    for dp, _, fs in os.walk(root):
        if name in fs:
            return os.path.join(dp, name)
    raise SystemExit('não achei %s em %s' % (name, root))


def main():
    root, out, which = sys.argv[1:4]
    im = Image.open(find(root, SRC[which])).convert('RGBA')
    a = np.asarray(im, dtype=np.float32)
    rgb = a[..., :3] / 255.0
    mx, mn = rgb.max(-1), rgb.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1e-3)
    # vermelho: R domina e G e B baixos (o dourado da moldura tem G alto e fica de fora)
    red = np.clip((rgb[..., 0] - np.maximum(rgb[..., 1], rgb[..., 2]) * 1.6) / 0.15, 0, 1) * (sat > 0.5)
    # dourado do Vita: brilho do vermelho levantado (o miolo do pack é escuro) e matiz ~38 graus
    v = np.clip(mx * 2.8 + 0.18, 0, 1)
    gold = np.stack([v, v * 0.72, v * 0.32], -1)
    rgb = rgb * (1 - red[..., None]) + gold * red[..., None]
    a[..., :3] = rgb * 255.0
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGBA').save(out)
    print(out, im.size, 'pixels vermelhos: %d' % int((red > 0.5).sum()))


if __name__ == '__main__':
    main()
