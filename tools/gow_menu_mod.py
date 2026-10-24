"""Opções do PC no menu de pausa do God of War 1 e 2 (Vita): a página Options ganha as linhas do vitart.

    python tools/gow_menu_mod.py <gow1_psp2_SCEA.psarc | gow2_psp2_SCEA.psarc> <saída .vtpatch> [--dump <pasta>]

GoW1: HD Textures, PS2 Audio, Resolution, Frame Rate, VSync, FPS Counter, Display Mode, Auto QTE (estados 5..12,
FLP_HUD, tag 30, canal PS2_Cheat). GoW2: HD Textures, Resolution, Frame Rate, VSync, FPS Counter, Display Mode, Auto QTE (7..13; o
FLP_HUDA, tag 1, é a versão 0x1b do formato; rótulos das linhas são textos dinâmicos; canal PS2_OverUnder).

Edita o FLP_HUD de wad/r_perma.wad_psp2 e wad/_VitaTV/r_perma.wad_psp2 (o menu de pausa, em inglês como o
resto do jogo) e grava um patch do psarc que o vitart aplica na leitura (runtime/vitart/hle_io.cpp). O
vitart lê os valores escolhidos no objeto de opções do jogo (runtime/vitart/game_options.cpp).

Página Options original (clipe7[213], quadros 7-8; PauseOptions_State): 1 Music Volume, 2 SFX Volume,
3 Controller Display, 4 Return. Linhas novas, copiadas da do SFX (rótulo + valor que cresce ao selecionar),
com o valor em texto (rótulo dinâmico) no lugar da barra:
    5 HD Textures  (PS2_WideScreen: 1 Off, 2 On, 3 indisponível)
    6 PS2 Audio    (PS2_DeFlicker:  1 Off, 2 On, 3 indisponível)
    7 Resolution   (PS2_SoundMode:  1..4 = escala interna)
Ordem na tela: 1, 2, 5, 6, 7, 3, 4. Navegação nova em quadros livres da raiz (49..53), chamada por
callframe a partir de PressUp/PressDown/PressLeft/PressRight, do X e da abertura da página.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flp  # noqa: E402
import kbm_flp  # noqa: E402
import psarc_gxp as P  # noqa: E402

# Estado de todas as opções num número só, na variável PS2_Cheat (campo +0x2bc do objeto de opções,
# sem tratador no jogo): 0x10000 | textura | áudio << 2 | resolução << 4 | fps << 7 | vsync << 9 | QTE automático << 10 |
# contador << 11 | modo << 12 | cheats: vida << 13, magia << 14, fúria << 15 | FPS medidos << 17. As alternâncias têm um
# bit (0/1) e o menu as lê como 1/2.
# Ver runtime/vitart/game_options.cpp.
FIELDS = [('_level0/:VtRage', 32768, 1), ('_level0/:VtMp', 16384, 1), ('_level0/:VtHp', 8192, 1), ('_level0/:VtDsp', 4096, 1), ('_level0/:VtCnt', 2048, 1), ('_level0/:VtQte', 1024, 1),
          ('_level0/:VtVsync', 512, 1), ('_level0/:VtFps', 128, 3), ('_level0/:VtRes', 16, 7),
          ('_level0/:VtAud', 4, 3), ('_level0/:VtTex', 1, 3)]
RES_TEXT = {1: '1x  (960x544)', 2: '2x  (1920x1088)', 3: '3x  (2880x1632)', 4: '4x  (3840x2176)'}
FPS_TEXT = {1: '30 FPS', 2: '60 FPS', 3: 'Max FPS'}  # Max: taxa do monitor; VSync Off: sem limite (patch_gow*_dt.py)
# linha: (chave do fscommand, nome do clipe, rótulo, variável decodificada, variável do texto, tipo, textos)
LINES = {
    'tex': ('VtTextures', 'HD Textures', '_level0/:VtTex', '_level0/:VtTexText', 'avail', None),
    'aud': ('VtAudio', 'PS2 Audio', '_level0/:VtAud', '_level0/:VtAudText', 'avail', None),
    'res': ('VtResolution', 'Resolution', '_level0/:VtRes', '_level0/:VtResText', 'list', RES_TEXT),
    'fps': ('VtFrameRate', 'Frame Rate', '_level0/:VtFps', '_level0/:VtFpsText', 'list', FPS_TEXT),
    'vsync': ('VtVsync', 'VSync', '_level0/:VtVsync', '_level0/:VtVsyncText', 'toggle', {1: 'Off', 2: 'On'}),
    'cnt': ('VtFpsCounter', 'FPS Counter', '_level0/:VtCnt', '_level0/:VtCntText', 'toggle', {1: 'Off', 2: 'On'}),
    'dsp': ('VtDisplayMode', 'Display Mode', '_level0/:VtDsp', '_level0/:VtDspText', 'toggle', {1: 'Windowed', 2: 'Borderless'}),
    'qte': ('VtAutoQte', 'Auto QTE', '_level0/:VtQte', '_level0/:VtQteText', 'toggle', {1: 'Off', 2: 'On'}),
    # cheats (runtime/vitart/game_options.cpp, cheats_tick) e o desbloqueio de todos os troféus (ação: X uma vez)
    'hp': ('VtInfHealth', 'Infinite Health', '_level0/:VtHp', '_level0/:VtHpText', 'toggle', {1: 'Off', 2: 'On'}),
    'mp': ('VtInfMagic', 'Infinite Magic', '_level0/:VtMp', '_level0/:VtMpText', 'toggle', {1: 'Off', 2: 'On'}),
    'rage': ('VtInfRage', 'Infinite Rage', '_level0/:VtRage', '_level0/:VtRageText', 'toggle', {1: 'Off', 2: 'On'}),
    'trophy': ('VtTrophies', 'Unlock Trophies', '_level0/:VtTrophy', '_level0/:VtTrophyText', 'action',
               {1: 'Press X', 2: 'Done'}),
}
GAMES = {
    'gow1': dict(
        state='PS2_Cheat', wad=(30, b'FLP_HUD'), shell=(30, b'FLP_Shell'), nobody=(24,),
        new=[(5, 'tex'), (6, 'aud'), (7, 'res'), (8, 'fps'), (11, 'vsync'), (9, 'cnt'), (10, 'dsp'), (12, 'qte'),
             (13, 'hp'), (14, 'mp'), (15, 'rage'), (16, 'trophy')],
        old={1: ('MusicVolume', 'A'), 2: ('SFXVolume', 'A'), 3: ('Controller', 'B'), 4: ('Return', 'B')},
        order=[1, 2, 5, 6, 7, 8, 11, 9, 10, 12, 13, 14, 15, 16, 3, 4], y=(-200.0, 215.0), row_k=0.82, frames=(7, 8, 9), kx=22, x_state='3',
        dyn_label=False),
    'gow2': dict(
        state='PS2_OverUnder', wad=(1, b'FLP_HUDA'), shell=(1, b'FLP_ShellA'), nobody=(0, 24),  # tag 0: parâmetro (o tamanho é o valor)
        new=[(7, 'res'), (8, 'fps'), (9, 'vsync'), (10, 'cnt'), (11, 'dsp'), (12, 'qte'), (13, 'tex'), (14, 'aud'),
             (15, 'hp'), (16, 'mp'), (17, 'rage'), (18, 'trophy')],
        old={1: ('MessageTips', 'A'), 2: ('MusicVolume', 'A'), 3: ('SFXVolume', 'A'), 4: ('Controller', 'B'),
             5: ('FlightControls', 'A'), 6: ('Return', 'B')},
        order=[1, 2, 3, 4, 5, 13, 14, 7, 8, 9, 10, 11, 12, 15, 16, 17, 18, 6], y=(-205.0, 228.0), row_k=0.82, frames=(9, 10, 11), kx=19, x_state='4',
        dyn_label=True),
}


def configure(game):
    """Põe nas variáveis do módulo a configuração do jogo (as funções abaixo as usam)."""
    global GAME, CFG, STATE, ROWS, PATH, ORDER, Y, FIRST_NEW, LAST_NEW, KEY, LO
    GAME, CFG = game, GAMES[game]
    STATE = CFG['state']
    # ROWS: (estado, nome do clipe, rótulo, variável decodificada, peso, variável do texto) nas linhas novas
    weights = {name: w for name, w, _ in FIELDS}
    ROWS = []
    for st, key in sorted(CFG['new']):
        clip, text, var, txt, _, _ = LINES[key]
        ROWS.append((st, clip, text, var, weights.get(var, 0), txt))  # a ação (troféus) não está na palavra
    KEY = {st: key for st, key in CFG['new']}
    PATH = {st: ('PauseMenu/' + name, grp) for st, (name, grp) in CFG['old'].items()}
    for st, key in CFG['new']:
        PATH[st] = ('PauseMenu/' + LINES[key][0], 'A')
    ORDER = CFG['order']
    y0, y1 = CFG['y']
    Y = {st: y0 + (y1 - y0) / (len(ORDER) - 1) * i for i, st in enumerate(ORDER)}
    FIRST_NEW, LAST_NEW = min(KEY), max(KEY)
    LO = 4 if game == 'gow2' else 0  # quadro dos rótulos e profundidade das animações (versão 0x1b do FLP)


configure('gow1')
LAST_KBM = []  # trocas dos ícones de teclado do último FLP escrito (mod_flp)
VALUE_RISE = 25.0  # unidades da linha
ST = 'PauseOptions_State'


def find_label(clip, f, name):
    for k, (h, subs) in enumerate(clip.labels):
        if f.s(struct.unpack_from('<H', h, 8)[0]) == name:
            return k
    raise KeyError(name)


def script_of(clip, f, name):
    return clip.labels[find_label(clip, f, name)][1][0]  # [cabeçalho, código]


def find_check(ins, value):
    """Índice de 'push PauseOptions_State; getvar; push <value>; eq' (a 1ª checagem desse estado)."""
    for i in range(len(ins) - 3):
        if (ins[i] == [0x96, ST] and ins[i + 1][0] == 0x1c and ins[i + 2] == [0x96, value] and ins[i + 3][0] == 0x0e):
            return i
    raise KeyError(value)


def patch_script(f, sub, value, make_block):
    """Insere um bloco antes da checagem 'PauseOptions_State == value' do script (sub = [cabeçalho, código]);
    o bloco pode saltar para 'fim' = a última instrução (o end do script)."""
    ins, _ = flp.decode(f, sub[1])
    assert ins[-1][0] == 0
    at = find_check(ins, value)
    block = make_block(at, len(ins) - 1)
    sub[1] = flp.encode(f, flp.insert(ins, at, block))


def named(clip, f, name):
    """Alça do elemento com esse nome de instância no clipe."""
    for h, keys in clip.anims:
        for k in keys:
            if f.s(struct.unpack_from('<H', k, 8)[0]) == name:
                return struct.unpack_from('<H', k, 2)[0]
    raise KeyError(name)


def obj(f, h, typ):
    t, i = f.handlers[h]
    assert t == typ, (t, typ)
    return i


def nav_script(f, step):
    a = flp.Asm()
    for i, s in enumerate(ORDER):
        n = ORDER[(i + step) % len(ORDER)]
        a.getv(ST).push(s).eq().not_().jumpif('n%d' % s)
        a.settarget(PATH[s][0]).gotolabel('Off').play().settarget('')
        a.settarget(PATH[n][0]).gotolabel('On').play().settarget('')
        if PATH[s][1] != PATH[n][1]:
            on_a = PATH[n][1] == 'A'
            a.settarget('/PauseMenu/ButtonBack').gotolabel('On' if on_a else 'Off').play().settarget('')
            a.settarget('/PauseMenu/ButtonSelectBack').gotolabel('Off' if on_a else 'On').play().settarget('')
        a.setv(ST, n).jump('fim')
        a.label('n%d' % s)
    a.label('fim').end()
    return flp.encode(f, a.build())


def push_fps(a, state):
    """Empilha os FPS medidos que o vitart pôs em PS2_Cheat (bits 17+): int(estado / 131072 - 0,5).
    O resto do estado é >= 65536 (bit 16), então a parte fracionária fica em [0, 0,5): truncar ou
    arredondar dá o mesmo número."""
    a.getv(state).push(131072).div().push(0.5).sub().toint()


def fps_counter_script():
    """Quadro do clipe do contador: número e "FPS" (vazios com o contador desligado = FPS 0)."""
    a = flp.Asm()
    st = '_level0/:' + STATE
    num, unit = '_level0/:VtFpsNum', '_level0/:VtFpsUnit'
    # sombra escura dos textos deste clipe (o mesmo recurso que o menu usa em 'TextPause'), para ler
    # sobre o céu claro
    a.setv('PS2_DropShadowOffset', '.09').setv('PS2_DropShadowColor', '0xFF000000')
    push_fps(a, st)
    a.push(0).eq().not_().jumpif('liga')
    a.setv(num, '').setv(unit, '').jump('fim')
    a.label('liga')
    a.push(num)
    push_fps(a, st)
    a.setvar()
    a.setv(unit, 'FPS')
    a.label('fim')
    return a


def show_script(f):
    """Decodifica PS2_Cheat nas variáveis _level0/:Vt* e escreve os textos dos valores."""
    a = flp.Asm()
    X = '_level0/:VtX'
    a.push(X).getv(STATE)
    push_fps(a, STATE)
    a.push(131072).mul().sub().push(65536).sub().setvar()
    for name, w, maxv in FIELDS:
        for v in range(maxv, 0, -1):
            a.getv(X).push(v * w).lt().jumpif('m%s%d' % (name, v))  # X < v*w -> tenta o menor
            a.setv(name, v)
            a.push(X).getv(X).push(v * w).sub().setvar()
            a.jump('f' + name)
            a.label('m%s%d' % (name, v))
        a.setv(name, 0)
        a.label('f' + name)
    # alternâncias: um bit (0/1) na palavra -> 1/2 como o menu usa
    for q in ('_level0/:VtQte', '_level0/:VtVsync', '_level0/:VtCnt', '_level0/:VtDsp', '_level0/:VtHp', '_level0/:VtMp',
              '_level0/:VtRage'):
        a.getv(q).push(1).eq().not_().jumpif('q0' + q)
        a.setv(q, 2).jump('q1' + q)
        a.label('q0' + q)
        a.setv(q, 1)
        a.label('q1' + q)
    if any(k == 'trophy' for _, k in CFG['new']):
        a.setv('_level0/:VtTrophy', 1)  # a ação volta a "Press X" a cada abertura da página
    a.callframe('/:VtOptTexts').end()
    return flp.encode(f, a.build())


def texts_script(f):
    """Textos dos valores a partir das variáveis _level0/:Vt* (e, no GoW2, os rótulos das linhas)."""
    a = flp.Asm()
    for st, key in CFG['new']:
        clip, text, var, txt, kind, table = LINES[key]
        if CFG['dyn_label']:
            a.setv(label_var(key), text)
        if kind == 'avail':
            a.getv(var).push(2).eq().not_().jumpif('x' + txt)
            a.setv(txt, 'On').jump('y' + txt)
            a.label('x' + txt)
            a.getv(var).push(3).eq().not_().jumpif('z' + txt)
            a.setv(txt, 'Unavailable').jump('y' + txt)
            a.label('z' + txt)
            a.setv(txt, 'Off')
            a.label('y' + txt)
        else:
            for v, t in table.items():
                a.getv(var).push(v).eq().not_().jumpif('t%s%d' % (txt, v))
                a.setv(txt, t)
                a.label('t%s%d' % (txt, v))
    a.end()
    return flp.encode(f, a.build())


def label_var(key):
    return '_level0/:Vt%sLabel' % key.capitalize()


def change_script(f, step):
    """Esquerda (step -1) / direita e X (+1) nas linhas novas: muda a variável mostrada e avisa o vitart
    com fscommand 'FSCommand:VT_<opção>_<valor>' (lido em runtime/vitart/game_options.cpp)."""
    a = flp.Asm()

    def choose(s, var, v):
        a.setv(var, v).fscommand('FSCommand:VT_%s_%d' % (KEY[s], v)).jump('mostra')

    for s, _, _, var, w, _ in ROWS:
        kind, table = LINES[KEY[s]][4], LINES[KEY[s]][5]
        a.getv(ST).push(s).eq().not_().jumpif('n%d' % s)
        if kind == 'avail':  # liga/desliga, travado se indisponível
            a.getv(var).push(3).eq().jumpif('trava')
            a.getv(var).push(2).eq().not_().jumpif('on%d' % s)
            choose(s, var, 1)
            a.label('on%d' % s)
            choose(s, var, 2)
        elif kind == 'action':  # desbloquear troféus: X (ou direita) uma vez -> "Done"
            a.getv(var).push(2).eq().jumpif('trava')
            choose(s, var, 2)
        elif kind == 'list':  # resolução 1..4, FPS 1..3 (30, 60, máximo)
            top = len(table)
            for v in range(1, top + 1):
                nv = v + step
                a.getv(var).push(v).eq().not_().jumpif('r%d_%d' % (s, v))
                if 1 <= nv <= top:
                    choose(s, var, nv)
                else:
                    a.jump('trava')
                a.label('r%d_%d' % (s, v))
            a.jump('trava')
        else:  # VSync, contador, modo de exibição: alterna
            a.getv(var).push(2).eq().not_().jumpif('dois%d' % s)
            choose(s, var, 1)
            a.label('dois%d' % s)
            choose(s, var, 2)
        a.label('n%d' % s)
    a.jump('fim')
    a.label('trava')
    a.setv('PS2_Sound1', 'SND_MM_LOCKED').jump('fim')
    a.label('mostra')
    a.setv('PS2_Sound1', 'SND_MM_NAV_LR')
    a.callframe('/:VtOptTexts')
    a.label('fim').end()
    return flp.encode(f, a.build())


def glyph_advances(f, font_h):
    """Avanço por glifo (escala 720) medido nos rótulos do próprio jogo; o resto pela largura da fonte."""
    from collections import Counter
    seen = {}
    for st in f.statics:
        cur = None
        for c in st.cmds:
            if 'font' in c:
                cur = (c['font'], c['scale'])
            if cur == (font_h, 720):
                for g, w in c['glyphs']:
                    seen.setdefault(g, Counter())[w] += 1
    return {g: c.most_common(1)[0][0] for g, c in seen.items()}


def make_label(f, ref, text):
    """Rótulo estático novo com a mesma fonte, cor e altura de ref, alinhado à direita como ref."""
    font = f.fonts[0]
    cmd0 = ref.cmds[0]
    adv = glyph_advances(f, cmd0['font'])
    glyphs = []
    for ch in text:
        g = font.cmap[ord(ch)]
        assert g >= 0, 'fonte sem %r' % ch
        glyphs.append((g, adv.get(g, round(font.widths[g] * 0.73))))
    st = flp.Static()
    st.h = bytearray(ref.h)
    st.cmds = [dict(cmd0, glyphs=glyphs)]
    w_ref = sum(w for c in ref.cmds for _, w in c['glyphs'])
    w_new = sum(w for _, w in glyphs)
    x_ref = struct.unpack_from('<h', ref.h, 0x10)[0]
    struct.pack_into('<h', st.h, 0x10, x_ref + w_ref - w_new)  # mesma borda direita (colado ao valor)
    return st


def make_dyn(f, var, tpl=None):
    if tpl is None:
        tpl = next(d for d in f.dyns if f.s(struct.unpack_from('<H', d, 0)[0]) == 'PS2_MoveName1')
    d = bytearray(tpl)  # PS2_MoveName1 (GoW1): texto alinhado à esquerda
    d[0x1e] = 0         # alinhamento à esquerda (no GoW2 o molde é o rótulo da linha, alinhado à direita)
    struct.pack_into('<H', d, 0, f.add_string(var))
    struct.pack_into('<H', d, 2, 0xffff)
    struct.pack_into('<H', d, 6, 760)                # altura da fonte: a das maiúsculas dos rótulos (medido)
    struct.pack_into('<I', d, 8, 0xffcccccc)         # cinza dos rótulos do menu
    # caixa x, y, largura, altura (1/16 px). O texto se apoia na base da caixa: y + altura põe a linha de
    # base na do rótulo (medido nas capturas do menu)
    struct.pack_into('<hhHH', d, 0x16, 0, -40, 12000, 900)
    return d


def xform(f, src, x=None, y=None, k=None):
    t = bytearray(f.xforms[src])
    if k is not None:  # escala (a e d da matriz) multiplicada por k
        a_, b_, c_, d_ = struct.unpack_from('<4i', t, 0)
        struct.pack_into('<4i', t, 0, round(a_ * k), b_, c_, round(d_ * k))
    if x is not None:
        struct.pack_into('<h', t, 0x10, round(x * 16))
    if y is not None:
        struct.pack_into('<h', t, 0x12, round(y * 16))
    f.xforms.append(t)
    return len(f.xforms) - 1


def handler(f, typ, idx):
    f.handlers.append((typ, idx))
    return len(f.handlers) - 1


# GoW1, HUD com a barra do pack ("Huds and Icon", texpack_gow1_extras.tsv): as fendas da lâmina do pack ficam noutro
# lugar. Medido com uma textura codificada (cor = coordenada) no lugar da barra: x_HUD = 123,3 + (u - 0,312) * 315,
# y_HUD = 48,5 + (v - 0,152) * 160,8. Vida u 0,312-0,633, v 0,20 -> fenda de cima do pack u 0,37-0,785, v 0,25; magia
# u 0,34-0,63, v 0,344 -> fenda de baixo, v 0,325. Os fundos pretos do Vita (com as pontas) somem: a fenda do pack já
# tem fundo. (nome, borda esquerda em u, nova borda, escala da largura, deslocamento em v; None = esconder)
# Os três medidores começam no mesmo x (~ -50 no MainMeter) e crescem para a direita com os upgrades: ficam alinhados
# ao canto esquerdo da fenda do pack (u 0,319), largura x1,293, e as três linhas redistribuídas na altura da fenda.
HUD_PACK = {'HealthMeter': (0.312, 0.319, 1.293, 0.035), 'MagicMeter': (0.312, 0.319, 1.293, 0.012),
            'PowerUpMeter': (0.312, 0.319, 1.293, -0.009), 'HealthMeterBlackBar': None, 'MagicMeterBlackBar': None}


def hud_pack_layout(f):
    mm_h = named(f.root, f, 'MainMeter')
    mm = f.c7[obj(f, mm_h, 7)]
    mm_x = next(struct.unpack_from('<h', f.xforms[struct.unpack_from('<H', k, 4)[0]], 0x10)[0] / 16
                for h, keys in f.root.anims for k in keys if struct.unpack_from('<H', k, 2)[0] == mm_h)
    hud_x = lambda u: 123.3 + (u - 0.312) * 315  # noqa: E731
    for h, keys in mm.anims:
        names = {f.s(struct.unpack_from('<H', k, 8)[0]) for k in keys if struct.unpack_from('<H', k, 8)[0] != 0xffff}
        hit = names & set(HUD_PACK)
        if not hit:
            continue
        spec = HUD_PACK[hit.pop()]
        for k in keys:  # todas as chaves do elemento (o nome só vem na primeira)
            if struct.unpack_from('<H', k, 2)[0] == 0:
                continue  # chave de remoção
            t = bytearray(f.xforms[struct.unpack_from('<H', k, 4)[0]])
            a_, b_, c_, d_ = struct.unpack_from('<4i', t, 0)
            x, y = struct.unpack_from('<hh', t, 0x10)
            if spec is None:
                struct.pack_into('<4i', t, 0, 0, 0, 0, 0)
            else:
                u0, u1, sc, dv = spec
                tx = mm_x + x / 16
                left, left2 = hud_x(u0), hud_x(u1)
                ntx = tx + (left2 - left) - (sc - 1) * (left - tx)
                struct.pack_into('<4i', t, 0, round(a_ * sc), b_, c_, d_)
                struct.pack_into('<hh', t, 0x10, round((ntx - mm_x) * 16), round(y + dv * 160.8 * 16))
            f.xforms.append(t)
            struct.pack_into('<H', k, 4, len(f.xforms) - 1)
    # fundo preto do medidor de Power Up (malhas 26, 28 e 32: as pontas e a faixa esticada) some; o preenchimento fica
    pu = f.c7[obj(f, next(struct.unpack_from('<H', keys[0], 2)[0] for h, keys in mm.anims
                         if any(struct.unpack_from('<H', k, 8)[0] != 0xffff and f.s(struct.unpack_from('<H', k, 8)[0]) == 'PowerUpMeter'
                                for k in keys)), 7)]
    zero = len(f.xforms)
    f.xforms.append(bytearray(f.xforms[0]))
    struct.pack_into('<4i', f.xforms[zero], 0, 0, 0, 0, 0)
    for h, keys in pu.anims:
        for k in keys:
            hd = struct.unpack_from('<H', k, 2)[0]
            if hd and hd < len(f.handlers) and f.handlers[hd][0] == 1 and f.handlers[hd][1] in (26, 28, 32):
                struct.pack_into('<H', k, 4, zero)


# HUD sem esticar (como o do GoW3): o FLP é um quadro 800x600 (4:3) esticado na tela 16:9 -> os elementos da raiz que
# aparecem desde o quadro 0 (medidores, contadores, menus) encolhem na horizontal por 0,75 (= (4/3)/(16/9)), cada um
# ancorado no lado mais perto: à esquerda de 300 fica preso à borda esquerda, à direita de 500 à direita; os do meio
# (menus com fundo de tela inteira) ficam como estão. Os da biblioteca de ícones (quadros 49+, desenhados dentro dos textos) não mudam.
HUD_ASPECT = 0.75


def hud_unstretch(f):
    seen = {}
    for h, keys in f.root.anims:
        if not keys or struct.unpack_from('<H', keys[0], 0)[0] != 0:
            continue
        for k in keys:
            hd, t = struct.unpack_from('<HH', k, 2)
            if not hd or t >= len(f.xforms):
                continue
            if t not in seen:
                x = bytearray(f.xforms[t])
                a_, b_, c_, d_ = struct.unpack_from('<4i', x, 0)
                px = struct.unpack_from('<h', x, 0x10)[0] / 16
                if 300 <= px <= 500:  # centro: menus com fundo de tela inteira, ficam como estão
                    seen[t] = t
                    continue
                anchor = 0.0 if px < 300 else 800.0
                struct.pack_into('<4i', x, 0, round(a_ * HUD_ASPECT), round(b_ * HUD_ASPECT), c_, d_)
                struct.pack_into('<h', x, 0x10, round((anchor + (px - anchor) * HUD_ASPECT) * 16))
                f.xforms.append(x)
                seen[t] = len(f.xforms) - 1
            struct.pack_into('<H', k, 4, seen[t])


def mod_flp(b, icons=True):
    f = flp.FLP(b)
    pm = f.c7[obj(f, named(f.root, f, 'PauseMenu'), 7)]
    sfx = f.c7[obj(f, named(pm, f, 'SFXVolume'), 7)]  # linha SFX Volume
    if CFG['dyn_label']:  # GoW2: o rótulo da linha é um texto dinâmico (variável de texto traduzido)
        sfx_label_dyn = f.dyns[obj(f, struct.unpack_from('<H', sfx.anims[0][1][0], 2)[0], 5)]
    else:
        sfx_label = f.statics[obj(f, struct.unpack_from('<H', sfx.anims[0][1][0], 2)[0], 4)]
    F0, F1, F2 = CFG['frames']

    # ---- linhas novas
    rows = {}
    for s, clip_name, text, _, _, txt_var in ROWS:
        if CFG['dyn_label']:
            d = bytearray(sfx_label_dyn)
            struct.pack_into('<H', d, 0, f.add_string(label_var(KEY[s])))
            f.dyns.append(d)
            h_lab = handler(f, 5, len(f.dyns) - 1)
            f.dyns.append(make_dyn(f, txt_var, sfx_label_dyn))
        else:
            st = make_label(f, sfx_label, text)
            f.statics.append(st)
            h_lab = handler(f, 4, len(f.statics) - 1)
            f.dyns.append(make_dyn(f, txt_var))
        h_dyn = handler(f, 5, len(f.dyns) - 1)
        c = flp.Clip()
        c.h = bytearray(sfx.h)
        c.frames = sfx.frames
        c.anims = [[bytearray(h), [bytearray(k) for k in keys]] for h, keys in sfx.anims]
        c.labels = [[bytearray(h), [[bytearray(sh), code] for sh, code in subs]] for h, subs in sfx.labels]
        for k in c.anims[0][1]:
            struct.pack_into('<H', k, 2, h_lab)
        for k in c.anims[1][1]:
            struct.pack_into('<H', k, 2, h_dyn)
            struct.pack_into('<H', k, 8, 0xffff)
            # o texto dinâmico desce a partir da origem: sobe VALUE_RISE (na escala do quadro) para a linha
            # de base coincidir com a do rótulo (medido nas capturas do menu)
            t = f.xforms[struct.unpack_from('<H', k, 4)[0]]
            scale = struct.unpack_from('<i', t, 12)[0] / 65536.0
            y = struct.unpack_from('<h', t, 0x12)[0] / 16.0
            struct.pack_into('<H', k, 4, xform(f, struct.unpack_from('<H', k, 4)[0], y=y - VALUE_RISE * scale))
        f.c7.append(c)
        rows[s] = (handler(f, 7, len(f.c7) - 1), clip_name)

    # ---- posições: as linhas originais redistribuídas pela ordem nova (as das pontas ficam onde estão)
    def move(elem_name, y):
        for h, keys in pm.anims:
            for k in keys:
                if f.s(struct.unpack_from('<H', k, 8)[0]) == elem_name and struct.unpack_from('<H', k, 0)[0] in (F0, F1):
                    struct.pack_into('<H', k, 4, xform(f, struct.unpack_from('<H', k, 4)[0], y=y, k=CFG['row_k']))
    for st, (name, _) in CFG['old'].items():
        move(name, Y[st])
    sfx_key = next(k for h, keys in pm.anims for k in keys
                   if f.s(struct.unpack_from('<H', k, 8)[0]) == 'SFXVolume' and struct.unpack_from('<H', k, 0)[0] == F0)
    t_sfx = struct.unpack_from('<H', sfx_key, 4)[0]
    depth = max(struct.unpack_from('<H', h, LO)[0] for h, _ in pm.anims)
    for s, (h_row, name) in rows.items():
        depth += 1
        t = xform(f, t_sfx, y=Y[s], k=CFG['row_k'])
        nm = f.add_string(name)
        keys = [bytearray(struct.pack('<HHHHH', F0, h_row, t, CFG['kx'], nm)),
                bytearray(struct.pack('<HHHHH', F1, h_row, t, 0, nm)),
                bytearray(struct.pack('<HHHHH', F2, 0, 0, 0, 0xffff))]
        head = bytearray(pm.anims[0][0])
        struct.pack_into('<H', head, LO, depth)
        pm.anims.append([head, keys])

    # ---- contador de FPS no estilo do contador de HITS: número grande (o rótulo dinâmico do HITS) com
    # "FPS" menor embaixo, num clipe de 2 quadros em laço no canto superior direito do HUD (800x600)
    hit = next(d for d in f.dyns if f.s(struct.unpack_from('<H', d, 0)[0]) in ('_level0/:HitCounter', '/:HitCounter'))
    d_num = bytearray(hit)
    struct.pack_into('<H', d_num, 0, f.add_string('_level0/:VtFpsNum'))
    f.dyns.append(d_num)
    h_num = handler(f, 5, len(f.dyns) - 1)
    d_unit = bytearray(hit)
    struct.pack_into('<H', d_unit, 0, f.add_string('_level0/:VtFpsUnit'))
    struct.pack_into('<H', d_unit, 6, 400)  # menor, como o "Hits"
    f.dyns.append(d_unit)
    h_unit = handler(f, 5, len(f.dyns) - 1)
    hit_clip = f.c7[obj(f, named(f.root, f, 'HitCounter'), 7)]
    c = flp.Clip()
    c.frames = 2
    if GAME == 'gow1':  # o clipe que treme do contador de HITS: número e "Hits" nas posições dele
        shake = f.c7[obj(f, struct.unpack_from('<H', hit_clip.anims[0][1][0], 2)[0], 7)]
        t_num = struct.unpack_from('<H', shake.anims[0][1][0], 4)[0]
        t_unit = struct.unpack_from('<H', shake.anims[1][1][0], 4)[0]
        c.h = bytearray(shake.h)
        a0 = [bytearray(shake.anims[0][0]), [bytearray(struct.pack('<HHHHH', 0, h_num, t_num, 0, 0xffff))]]
        a1 = [bytearray(shake.anims[1][0]), [bytearray(struct.pack('<HHHHH', 0, h_unit, t_unit, 0, 0xffff))]]
    else:  # GoW2: clipes aninhados de outro jeito; as mesmas posições do GoW1, (74, -493) e (7, 113) em 1/16
        t_num, t_unit = len(f.xforms), len(f.xforms) + 1
        for x, y in ((74, -493), (7, 113)):
            t = bytearray(f.xforms[0])
            struct.pack_into('<4ihh', t, 0, 65536, 0, 0, 65536, x, y)
            f.xforms.append(t)
        c.h = bytearray(hit_clip.h)
        anim_h = bytearray(hit_clip.anims[0][0])
        a0 = [bytearray(anim_h), [bytearray(struct.pack('<HHHHH', 0, h_num, t_num, 0, 0xffff))]]
        a1 = [bytearray(anim_h), [bytearray(struct.pack('<HHHHH', 0, h_unit, t_unit, 0, 0xffff))]]
        struct.pack_into('<H', a0[0], LO, 1)
        struct.pack_into('<H', a1[0], LO, 2)
    c.anims = [a0, a1]
    lab_h = bytearray(hit_clip.labels[0][0])  # cabeçalho de rótulo de quadro
    sub_h = bytearray(hit_clip.labels[0][1][0][0])
    a = fps_counter_script()
    a.gotoframe(0).play().end()
    code1 = flp.encode(f, a.build())
    labs = []
    for frame, code in ((0, flp.encode(f, fps_counter_script().end().build())), (1, code1)):
        h = bytearray(lab_h)
        struct.pack_into('<H', h, LO, frame)
        struct.pack_into('<H', h, 8, 0xffff)
        labs.append([h, [[bytearray(sub_h), code]]])
    c.labels = labs
    f.c7.append(c)
    h_clip = handler(f, 7, len(f.c7) - 1)
    t = bytearray(f.xforms[0])
    struct.pack_into('<4i', t, 0, 49152, 0, 0, 49152)  # escala 0,75
    struct.pack_into('<hh', t, 0x10, round(730 * 16), round(52 * 16))
    f.xforms.append(t)
    depth = max(struct.unpack_from('<H', h, LO)[0] for h, _ in f.root.anims) + 2
    head = bytearray(f.root.anims[0][0])
    struct.pack_into('<H', head, LO, depth)
    f.root.anims.append([head, [bytearray(struct.pack('<HHHHH', 0, h_clip, len(f.xforms) - 1, 0,
                                                       f.add_string('VtFpsCounter')))]])

    # ---- funções novas na raiz (quadros 49..53, fora da linha do tempo: a raiz para no quadro 1)
    root = f.root
    proto_h, proto_subs = root.labels[find_label(root, f, 'PauseReset')]
    new = [(49, 'VtNavUp', nav_script(f, -1)), (50, 'VtNavDown', nav_script(f, +1)),
           (51, 'VtLeft', change_script(f, -1)), (52, 'VtRight', change_script(f, +1)),
           (53, 'VtOptShow', show_script(f)), (54, 'VtOptTexts', texts_script(f))]
    for frame, name, code in new:
        h = bytearray(proto_h)
        struct.pack_into('<H', h, LO, frame)
        struct.pack_into('<H', h, 8, f.add_string(name))
        sh = bytearray(proto_subs[0][0])
        k = next(i for i, (hh, _) in enumerate(root.labels) if struct.unpack_from('<H', hh, LO)[0] > frame)
        root.labels.insert(k, [h, [[sh, code]]])

    # ---- ganchos nos scripts do jogo
    def call_then_end(fn):
        # cima/baixo com a página aberta (estado 1..4 originais e 5..7 novos): navegação nova inteira
        def mk(at, end):
            a = flp.Asm().callframe(fn).jump('fim')
            return a.build(base=at, extern={'fim': end + len(a.ins)})
        return mk

    def guarded(fn):
        # estado 5..7 -> fn; senão, o código original
        def mk(at, end):
            a = flp.Asm()
            a.getv(ST).push(FIRST_NEW).lt().jumpif('orig')
            a.push(LAST_NEW).getv(ST).lt().jumpif('orig')
            a.callframe(fn).jump('fim')
            n = len(a.ins)
            return a.build(base=at, extern={'orig': at + n, 'fim': end + n})
        return mk

    # ---- ícones dos botões com as teclas quando o teclado/mouse está em uso
    swaps = kbm_flp.add_kbm_icons(f, GAME, LO) if icons else []
    if GAME == 'gow1':
        hud_pack_layout(f)
    if os.environ.get('VT_HUD_STRETCH') != '1':
        hud_unstretch(f)

    patch_script(f, script_of(root, f, 'PressUp'), '1', call_then_end('/:VtNavUp'))
    patch_script(f, script_of(root, f, 'PressDown'), '1', call_then_end('/:VtNavDown'))
    patch_script(f, script_of(root, f, 'PressLeft'), '1', guarded('/:VtLeft'))
    patch_script(f, script_of(root, f, 'PressRight'), '1', guarded('/:VtRight'))
    x_ev = []  # botão X (0x40) com a página Options
    for c in f.c6:
        for ev in c.events:
            if struct.unpack_from('<I', ev[0], 8)[0] == 0x40:
                try:
                    find_check(flp.decode(f, ev[1])[0], CFG['x_state'])
                    x_ev.append(ev)
                except KeyError:
                    pass
    assert len(x_ev) == 1, len(x_ev)
    patch_script(f, x_ev[0], CFG['x_state'], guarded('/:VtRight'))
    # abrir a página Options: textos dos valores atualizados
    opt = script_of(pm, f, 'Options')
    ins, _ = flp.decode(f, opt[1])
    opt[1] = flp.encode(f, flp.insert(ins, 0, flp.Asm().callframe('/:VtOptShow').build(base=0)))
    out = f.write()
    global LAST_KBM
    LAST_KBM = kbm_flp.locate(out, swaps)  # [(nome, posição no FLP, chave original, chave de teclado)]
    return out


# ---------------------------------------------------------------- WAD e psarc
def wad_replace(w, name, body, rtag=30):
    out, o = bytearray(), 0
    hit = 0
    while o + 32 <= len(w):
        tag, flags, size = struct.unpack_from('<HHI', w, o)
        nm = w[o + 8:o + 32].split(b'\0')[0]
        full = 32 + (((size + 15) & ~15) if tag not in CFG['nobody'] else 0)
        if tag == rtag and nm == name:
            hd = bytearray(w[o:o + 32])
            struct.pack_into('<I', hd, 4, len(body))
            out += hd + body + bytes(((len(body) + 15) & ~15) - len(body))
            hit += 1
        else:
            out += w[o:o + full]
        o += full
    out += w[o:]
    assert hit == 1, name
    return bytes(out)


def wad_get(w, name, rtag=30):
    o = 0
    while o + 32 <= len(w):
        tag, flags, size = struct.unpack_from('<HHI', w, o)
        if tag == rtag and w[o + 8:o + 32].split(b'\0')[0] == name:
            return w[o + 32:o + 32 + size]
        o += 32 + (((size + 15) & ~15) if tag not in CFG['nobody'] else 0)
    raise KeyError(name)


def build_patch(psarc, files, out):
    """files: {nome no manifesto: bytes novos}. Cada arquivo novo vai comprimido (zlib, blocos do psarc)
    para depois do fim do psarc. Cabendo nos blocos do original, reaproveita as entradas da tabela de blocos;
    crescendo, ganha entradas novas no fim da tabela (a TOC cresce: o manifesto, que vem logo depois da TOC no
    psarc e ficaria coberto por ela, é copiado para os extras e a entrada 0 aponta para a cópia)."""
    import zlib
    with open(psarc, 'rb') as fh:
        hdr = fh.read(32)
        magic, ver, comp, toc_len, ent_size, n, block, flags = struct.unpack('>4sI4sIIIII', hdr)
        fh.seek(0)
        toc = bytearray(fh.read(toc_len))
        fh.seek(0)
        ents, zs, _ = P.entries(fh)
        names = P.read_file(fh, ents[0], zs, block).decode('latin1').split('\n')
        orig = os.path.getsize(psarc)
        w = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
        tab_at = 32 + n * ent_size
        grow = any((len(data) + block - 1) // block > (ents[names.index(name) + 1][1] + block - 1) // block
                   for name, data in files.items())
        extra = bytearray()
        if grow:  # manifesto (entrada 0) para os extras, cópia crua dos blocos comprimidos
            idx0, size0, off0 = ents[0]
            nb0 = (size0 + block - 1) // block
            raw = sum(zs[idx0 + k] or min(block, size0 - k * block) for k in range(nb0))
            fh.seek(off0)
            extra += fh.read(raw)
            toc[32 + 25:32 + 30] = orig.to_bytes(5, 'big')
    for name, data in files.items():
        i = names.index(name) + 1
        idx, size, off = ents[i]
        nb_old = (size + block - 1) // block
        nb_new = (len(data) + block - 1) // block
        e = 32 + i * ent_size
        if nb_new > nb_old:  # entradas novas no fim da tabela
            idx = (len(toc) - tab_at) // w
            toc += bytes(nb_new * w)
            toc[e + 16:e + 20] = idx.to_bytes(4, 'big')
            print('%s: cresceu de %d para %d blocos (entradas novas na tabela)' % (name, nb_old, nb_new))
        toc[e + 20:e + 25] = len(data).to_bytes(5, 'big')
        toc[e + 25:e + 30] = (orig + len(extra)).to_bytes(5, 'big')
        for k in range(max(nb_old, nb_new) if nb_new <= nb_old else nb_new):
            chunk = data[k * block:(k + 1) * block]
            if not chunk:
                z = 0
            else:
                c = zlib.compress(chunk, 9)
                if len(c) < len(chunk) and len(c) < (1 << (8 * w)):
                    extra += c
                    z = len(c)
                else:
                    extra += chunk + bytes(block - len(chunk))  # cru = bloco cheio
                    z = 0
            toc[tab_at + (idx + k) * w:tab_at + (idx + k + 1) * w] = z.to_bytes(w, 'big')
    struct.pack_into('>I', toc, 12, len(toc))
    with open(out, 'wb') as fo:
        fo.write(b'VTPP' + struct.pack('<QII', orig, len(toc), len(extra)))
        fo.write(toc)
        fo.write(extra)
    print('%s: TOC %d bytes, %d bytes novos (%d arquivos)' % (out, len(toc), len(extra), len(files)))


def main():
    psarc, out = sys.argv[1], sys.argv[2]
    dump = sys.argv[sys.argv.index('--dump') + 1] if '--dump' in sys.argv else None
    configure('gow2' if 'gow2' in os.path.basename(psarc).lower() else 'gow1')
    rtag, rname = CFG['wad']
    files = {}
    kbm = []  # trocas dos ícones por arquivo (runtime/vitart/kbm_icons.cpp)
    with open(psarc, 'rb') as fh:
        ents, zs, block = P.entries(fh)
        names = P.read_file(fh, ents[0], zs, block).decode('latin1').split('\n')
        for name in ('wad/r_perma.wad_psp2', 'wad/_VitaTV/r_perma.wad_psp2'):
            w = P.read_file(fh, ents[names.index(name) + 1], zs, block)
            new_flp = mod_flp(wad_get(w, rname, rtag))
            files[name] = wad_replace(w, rname, new_flp, rtag)
            if LAST_KBM:
                kbm.append((name, new_flp, LAST_KBM))
            if dump:
                os.makedirs(dump, exist_ok=True)
                open(os.path.join(dump, name.replace('/', '_') + '.flp'), 'wb').write(new_flp)
        # menu principal (Shell): só os ícones de teclado
        stag, sname = CFG['shell']
        for name in ('wad/r_shella.wad_psp2', 'wad/_VitaTV/r_shella.wad_psp2'):
            w = P.read_file(fh, ents[names.index(name) + 1], zs, block)
            f = flp.FLP(wad_get(w, sname, stag))
            swaps = kbm_flp.add_kbm_icons(f, GAME, LO)
            new_flp = f.write()
            files[name] = wad_replace(w, sname, new_flp, stag)
            kbm.append((name, new_flp, kbm_flp.locate(new_flp, swaps)))
    build_patch(psarc, files, out)
    write_kbm(kbm, out[:-len('.vtpatch')] + '.kbm.tsv' if out.endswith('.vtpatch') else out + '.kbm.tsv')


def write_kbm(kbm, path):
    """Tabela dos ícones de teclado para o vitart: por FLP, uma assinatura (32 bytes antes da 1ª chave + ela) e
    as trocas 'posição chave_original chave_de_teclado nome' (posições relativas ao início do FLP)."""
    with open(path, 'w', encoding='utf-8') as fo:
        fo.write('# ícones de teclado e mouse do HUD (tools/kbm_flp.py, lido por runtime/vitart/kbm_icons.cpp)\n')
        for name, data, swaps in kbm:
            first = min(o for _, o, _, _ in swaps)
            a = max(0, first - 32)
            fo.write('flp %s %d %s\n' % (name, a, data[a:first + 10].hex()))
            for nm, o, ps, kb in swaps:
                fo.write('%d %s %s %s\n' % (o, ps.hex(), kb.hex(), nm))
    print('%s: %d trocas de ícones' % (path, sum(len(x[2]) for x in kbm)))


if __name__ == '__main__':
    main()
