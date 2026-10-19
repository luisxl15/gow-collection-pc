"""Ícones de teclado e mouse no HUD dos GoW do Vita (chamado por gow_menu_mod.mod_flp).

Os ícones dos botões não são texturas: cada um é um clipe do FLP com um disco de fundo (malha com
textura) e o símbolo em vetor. Ficam numa "biblioteca" de quadros da raiz (GoW1 49..93, GoW2 42..58),
achada pelo nome da instância ("XButton", "L1Button"...); o texto "[XButton]" desenha esse clipe.

O motor desenha esses clipes sempre no quadro 0 e sem rodar scripts (medido: um script que trocava de
quadro não teve efeito no texto do tutorial), então a troca é feita pelo vitart na memória do jogo: cada
elemento do quadro 0 de um ícone tem um par de chaves de animação (frame, alça, transformação, -, nome) --
a original e a de teclado (disco escuro do XButton / nome da tecla na fonte do jogo / texto vazio). O FLP
sai com a original; o vitart acha o FLP carregado pela assinatura e escreve uma ou outra conforme o último
controle usado (runtime/vitart/kbm_icons.cpp, arquivo <psarc>.kbm.tsv ao lado do .vtpatch).
"""
import struct

import flp
import kbm_atlas

# rótulo de cada ícone. E e R trocam entre os jogos: E = interagir (R2 no GoW1, R1 no GoW2).
LABELS = {
    'gow1': {'XButton': 'SPACE', 'TriangleButton': 'RMB', 'CircleButton': 'F', 'SquareButton': 'LMB',
             'XButtonGreek': 'SPACE', 'TriangleButtonGreek': 'RMB', 'CircleButtonGreek': 'F',
             'SquareButtonGreek': 'LMB', 'XButtonPress': 'SPACE', 'R2ButtonPress': 'E', 'R2ButtonStatic': 'E',
             'L1Button': 'Q', 'L2Button': 'CTRL', 'R1Button': 'R', 'R2Button': 'E', 'StartButton': 'TAB',
             'SelectButton': 'ESC', 'Joystick': 'WASD', 'DpadButton': '1-4', 'UpButton': '1',
             'RightButton': '2', 'DownButton': '3', 'LeftButton': '4'},
    'gow2': {'XButton': 'SPACE', 'TriangleButton': 'RMB', 'CircleButton': 'F', 'SquareButton': 'LMB',
             'L1Button': 'Q', 'L2Button': 'CTRL', 'R1Button': 'E', 'R2Button': 'R',
             'Joystick': 'WASD', 'JoystickL': 'WASD', 'JoystickR': 'SHIFT', 'DpadButton': '1-4',
             'UpButton': '1', 'RightButton': '2', 'DownButton': '3', 'LeftButton': '4'},
}
# tamanho da letra pelo número de caracteres (escala da fonte; os textos do menu usam 400)
SCALE = {1: 440, 2: 360, 3: 300, 4: 240, 5: 200}
PROBE = None   # {ícone: texto} para medir as células do atlas (ver kbm_atlas.py)
GLYPH_K = 1.0   # ajuste do tamanho das letras (medido nas capturas)
ICON_H = 62.0  # altura do ícone desenhado, em unidades do HUD na escala 1 (o disco tem ~58)
BASE_K = 0.35  # linha de base abaixo do centro, em tamanhos da fonte


def _xf(f, t):
    a, b, c, d = struct.unpack_from('<4i', f.xforms[t], 0)
    x, y = struct.unpack_from('<hh', f.xforms[t], 0x10)
    return a / 65536, d / 65536, x / 16, y / 16


def _new_xf(f, sx, sy, x, y):
    t = bytearray(f.xforms[0])
    struct.pack_into('<4i', t, 0, round(sx * 65536), 0, 0, round(sy * 65536))
    struct.pack_into('<hh', t, 0x10, round(x * 16), round(y * 16))
    f.xforms.append(t)
    return len(f.xforms) - 1


def _first_mesh(f, h, sx=1.0, sy=1.0, x=0.0, y=0.0, depth=0):
    """Primeira malha (o fundo) descendo pelos primeiros elementos: (handler, escala, posição acumuladas)."""
    t, i = f.handlers[h]
    if t == 1:
        return h, sx, sy, x, y
    if t != 7 or depth > 4:
        return None
    for _, keys in f.c7[i].anims:
        k = keys[0]
        hh, tt = struct.unpack_from('<HH', k, 2)
        if hh >= len(f.handlers):
            continue
        a, d, tx, ty = _xf(f, tt) if tt < len(f.xforms) else (1, 1, 0, 0)
        r = _first_mesh(f, hh, sx * a, sy * d, x + tx * sx, y + ty * sy, depth + 1)
        if r:
            return r
    return None


def _icon_clips(f, names):
    """{nome: handler do clipe} das instâncias com esses nomes (raiz e clipes)."""
    out = {}
    for c in [f.root] + f.c7:
        for _, keys in c.anims:
            for k in keys:
                nm = struct.unpack_from('<H', k, 8)[0]
                if nm == 0xffff:
                    continue
                n = f.s(nm)
                if n in names and n not in out:
                    h = struct.unpack_from('<H', k, 2)[0]
                    if h < len(f.handlers) and f.handlers[h][0] == 7:
                        out[n] = h
    return out


def _glyph_meshes(f):
    """Função caractere -> alça de uma malha com o glifo da fonte, em branco.
    Os glifos são malhas do mesmo modelo dos ícones (mesmo formato de referência), então viram malhas comuns."""
    font = f.fonts[0]
    cache = {}

    def mesh_for(ch):
        key = ch
        if key not in cache:
            g = font.cmap[ord(ch)]
            assert g >= 0, 'fonte sem %r' % ch
            m = flp.Mesh()
            m.h = bytearray(font.meshes[g].h)
            m.mats = [bytearray(x) for x in font.meshes[g].mats]
            for x in m.mats:
                x[0:4] = bytes((255, 255, 255, 255))
            f.meshes.append(m)
            f.handlers.append((1, len(f.meshes) - 1))
            cache[key] = len(f.handlers) - 1
        return cache[key]
    return mesh_for


def add_kbm_icons(f, game, lo):
    """Prepara os ícones e devolve a lista de trocas [(nome, chave original, chave de teclado)] (10 bytes cada)."""
    labels = LABELS[game]
    clips = _icon_clips(f, set(labels))
    xb = clips.get('XButton')
    disc = _first_mesh(f, xb) if xb is not None else None  # disco do XButton: o fundo neutro dos botões
    assert disc, 'XButton sem malha de fundo'
    font = f.fonts[0]
    mesh_for = _glyph_meshes(f)
    # escondido = um glifo com escala zero (o motor não respeita o alfa da cor do material)
    h_hidden = mesh_for('E')
    t_zero = _new_xf(f, 0, 0, 0, 0)
    swaps = []
    sym_label = {}  # malha do símbolo -> rótulo
    done_clips = {f.handlers[h][1] for h in clips.values()}
    for name, h in sorted(clips.items()):
        c = f.c7[f.handlers[h][1]]
        if c.frames != 1 or c.labels or not c.anims:
            continue  # só os de um quadro, sem scripts (os animados ficam como estão)
        text = labels[name]
        if PROBE and name in PROBE:  # medição das células do atlas da fonte (glifos acentuados)
            text = PROBE[name]
        bg = _first_mesh(f, h)
        if not bg:
            continue
        _, bsx, bsy, cx, cy = bg
        k = (bsx + bsy) / 2
        t_disc = _new_xf(f, k, k, cx, cy)  # o disco no lugar (e na escala) do fundo deste ícone
        # letras: malhas dos glifos, com vértices em 1/16 px nas unidades da fonte (como as larguras): escala da
        # malha = escala do rótulo / tamanho da fonte; avanço em px = largura * essa escala * 0,86 / 16
        sm = SCALE[min(len(text), 5)] * k / font.size * GLYPH_K
        advs = [font.widths[font.cmap[ord(ch)]] * sm * 0.86 / 16 for ch in text]
        x = cx - sum(advs) / 2
        letters = []
        if PROBE and name in PROBE:  # grade de glifos grandes (4x), 5 por linha, bem afastados
            big = sm * 4
            step = font.size * big / 16 * 1.3
            for n, ch in enumerate(text):
                letters.append((mesh_for(ch), _new_xf(f, big, big, cx + (n % 5 - 2) * step, cy + (n // 5 - 1) * step)))
        else:
            for ch, adv in zip(text, advs):
                letters.append((mesh_for(ch), _new_xf(f, sm, sm, x, cy + font.size * sm * BASE_K / 16)))
                x += adv
        targets = [(disc[0], t_disc)] + letters
        g = kbm_atlas.ICON_GLYPH.get(text)
        if g is not None and not (PROBE and name in PROBE):
            # ícone desenhado (atlas da fonte de teclado, kbm_atlas.py): o glifo no lugar do disco, um pouco maior.
            # Medido: o quadro do glifo mede ~2,16 unidades do HUD por texel do atlas na escala 1 e vai da
            # origem para a direita e para cima.
            u0, u1, v0, v1 = kbm_atlas.CELLS[g]
            m = ICON_H * k / ((v1 - v0) * 2.16)
            wq, hq = (u1 - u0) * 2.16 * m, (v1 - v0) * 2.16 * m
            ch = next(chr(c) for c, gg in enumerate(font.cmap) if gg == g)
            targets = [(mesh_for(ch), _new_xf(f, m, m, cx - wq / 2, cy + hq / 2))]
        while len(c.anims) < len(targets):  # elementos a mais: escondidos (escala zero) no modo controle
            hd = bytearray(c.anims[0][0])
            struct.pack_into('<H', hd, lo, max(struct.unpack_from('<H', a[0], lo)[0] for a in c.anims) + 1)
            c.anims.append([hd, [bytearray(struct.pack('<HHHHH', 0, h_hidden, t_zero, 0, 0xffff))]])
        for n, (_, keys) in enumerate(c.anims):
            k0 = next((kk for kk in keys if struct.unpack_from('<H', kk, 0)[0] == 0), None)
            if k0 is None:
                continue
            # transformação própria em cada chave: a chave original fica única no arquivo (o vitart a acha)
            t_own = struct.unpack_from('<H', k0, 4)[0]
            f.xforms.append(bytearray(f.xforms[t_own if t_own < len(f.xforms) else 0]))
            struct.pack_into('<H', k0, 4, len(f.xforms) - 1)
            kb = bytearray(k0)
            hh, tt = targets[n] if n < len(targets) else (h_hidden, t_zero)
            struct.pack_into('<HH', kb, 2, hh, tt)
            swaps.append((name, bytes(k0), bytes(kb)))
        if len(c.anims) >= 2:  # símbolo deste ícone (2º elemento) -> rótulo, para achar as cópias soltas abaixo
            hs = struct.unpack_from('<H', c.anims[1][1][0], 2)[0]
            if hs < len(f.handlers) and f.handlers[hs][0] == 1:
                sym_label.setdefault(hs, text)
    # cópias soltas dos ícones (GoW1: as dicas "X = Select" da pausa são clipes com o disco, o símbolo e o texto,
    # sem usar o clipe da biblioteca): o disco vira o ícone desenhado e o símbolo some
    for ci, c in enumerate(f.c7):
        if ci in done_clips or c.frames != 1 or c.labels:
            continue
        k0s = [next((kk for kk in keys if struct.unpack_from('<H', kk, 0)[0] == 0), None) for _, keys in c.anims]
        hds = [struct.unpack_from('<H', kk, 2)[0] if kk is not None else None for kk in k0s]
        for n in range(len(hds) - 1):
            if hds[n] != disc[0] or hds[n + 1] not in sym_label:
                continue
            g = kbm_atlas.ICON_GLYPH.get(sym_label[hds[n + 1]])
            if g is None:
                continue
            a_, d_, cx, cy = _xf(f, struct.unpack_from('<H', k0s[n], 4)[0])
            k = (a_ + d_) / 2
            u0, u1, v0, v1 = kbm_atlas.CELLS[g]
            m = ICON_H * k / ((v1 - v0) * 2.16)
            wq, hq = (u1 - u0) * 2.16 * m, (v1 - v0) * 2.16 * m
            ch = next(chr(cc) for cc, gg in enumerate(font.cmap) if gg == g)
            for kk, (hh, tt) in ((k0s[n], (mesh_for(ch), _new_xf(f, m, m, cx - wq / 2, cy + hq / 2))),
                                 (k0s[n + 1], (h_hidden, t_zero))):
                t_own = struct.unpack_from('<H', kk, 4)[0]
                f.xforms.append(bytearray(f.xforms[t_own if t_own < len(f.xforms) else 0]))
                struct.pack_into('<H', kk, 4, len(f.xforms) - 1)
                kb = bytearray(kk)
                struct.pack_into('<HH', kb, 2, hh, tt)
                swaps.append(('clipe %d' % ci, bytes(kk), bytes(kb)))
    return swaps


def locate(flp_bytes, swaps):
    """Posições das chaves originais no FLP escrito: [(nome, posição, original, teclado)]; cada uma é única."""
    out = []
    for name, ps, kb in swaps:
        i = flp_bytes.find(ps)
        assert i >= 0 and flp_bytes.find(ps, i + 1) < 0, 'chave de %s não é única' % name
        out.append((name, i, ps, kb))
    return out
