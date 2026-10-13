"""Opções do PC no menu de pausa do God of War 1 (Vita): Options ganha HD Textures, PS2 Audio e Resolution.

    python tools/gow_menu_mod.py <gow1_psp2_SCEA.psarc> <saída .vtpatch> [--dump <pasta>]

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
import psarc_gxp as P  # noqa: E402

# Estado de todas as opções num número só, na variável PS2_Cheat (campo +0x2bc do objeto de opções,
# sem tratador no jogo): 0x10000 | textura | áudio << 2 | resolução << 4 | fps << 7 | proporção << 9.
# Ver runtime/vitart/game_options.cpp.
STATE = 'PS2_Cheat'
FIELDS = [('_level0/:VtDsp', 16384, 3), ('_level0/:VtCnt', 4096, 3), ('_level0/:VtAsp', 512, 7), ('_level0/:VtFps', 128, 3), ('_level0/:VtRes', 16, 7),
          ('_level0/:VtAud', 4, 3), ('_level0/:VtTex', 1, 3)]
ROWS = [  # (estado da página, nome do clipe, rótulo, variável decodificada, peso, variável do texto)
    (5, 'VtTextures', 'HD Textures', '_level0/:VtTex', 1, '_level0/:VtTexText'),
    (6, 'VtAudio', 'PS2 Audio', '_level0/:VtAud', 4, '_level0/:VtAudText'),
    (7, 'VtResolution', 'Resolution', '_level0/:VtRes', 16, '_level0/:VtResText'),
    (8, 'VtFrameRate', 'Frame Rate', '_level0/:VtFps', 128, '_level0/:VtFpsText'),
    (9, 'VtFpsCounter', 'FPS Counter', '_level0/:VtCnt', 4096, '_level0/:VtCntText'),
    (10, 'VtDisplayMode', 'Display Mode', '_level0/:VtDsp', 16384, '_level0/:VtDspText'),
]
RES_TEXT = {1: '1x  (960x544)', 2: '2x  (1920x1088)', 3: '3x  (2880x1632)', 4: '4x  (3840x2176)'}
FPS_TEXT = {1: '30 FPS', 2: '60 FPS'}
# estado -> (caminho do clipe da linha, grupo: A = ajustável com esquerda/direita, B = seleciona com X)
PATH = {1: ('PauseMenu/MusicVolume', 'A'), 2: ('PauseMenu/SFXVolume', 'A'), 5: ('PauseMenu/VtTextures', 'A'),
        6: ('PauseMenu/VtAudio', 'A'), 7: ('PauseMenu/VtResolution', 'A'), 8: ('PauseMenu/VtFrameRate', 'A'),
        9: ('PauseMenu/VtFpsCounter', 'A'), 10: ('PauseMenu/VtDisplayMode', 'A'),
        3: ('PauseMenu/Controller', 'B'), 4: ('PauseMenu/Return', 'B')}
ORDER = [1, 2, 5, 6, 7, 8, 9, 10, 3, 4]
Y = {s: -175.0 + 350.0 / (len(ORDER) - 1) * i for i, s in enumerate(ORDER)}
FIRST_NEW, LAST_NEW = 5, 10
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
    a.callframe('/:VtOptTexts').end()
    return flp.encode(f, a.build())


def texts_script(f):
    """Textos dos valores a partir das variáveis _level0/:Vt*."""
    a = flp.Asm()
    for _, _, _, var, _, txt in ROWS[:2]:
        a.getv(var).push(2).eq().not_().jumpif('x' + txt)
        a.setv(txt, 'On').jump('y' + txt)
        a.label('x' + txt)
        a.getv(var).push(3).eq().not_().jumpif('z' + txt)
        a.setv(txt, 'Unavailable').jump('y' + txt)
        a.label('z' + txt)
        a.setv(txt, 'Off')
        a.label('y' + txt)
    for (_, _, _, var, _, txt), table in ((ROWS[2], RES_TEXT), (ROWS[3], FPS_TEXT), (ROWS[4], {1: 'Off', 2: 'On'}),
                                     (ROWS[5], {1: 'Windowed', 2: 'Borderless'})):
        for v, t in table.items():
            a.getv(var).push(v).eq().not_().jumpif('t%s%d' % (txt, v))
            a.setv(txt, t)
            a.label('t%s%d' % (txt, v))
    a.end()
    return flp.encode(f, a.build())


KEY = {5: 'tex', 6: 'aud', 7: 'res', 8: 'fps', 9: 'cnt', 10: 'dsp'}


def change_script(f, step):
    """Esquerda (step -1) / direita e X (+1) nas linhas novas: muda a variável mostrada e avisa o vitart
    com fscommand 'FSCommand:VT_<opção>_<valor>' (lido em runtime/vitart/game_options.cpp)."""
    a = flp.Asm()

    def choose(s, var, v):
        a.setv(var, v).fscommand('FSCommand:VT_%s_%d' % (KEY[s], v)).jump('mostra')

    for s, _, _, var, w, _ in ROWS:
        a.getv(ST).push(s).eq().not_().jumpif('n%d' % s)
        if s in (5, 6):  # liga/desliga
            a.getv(var).push(3).eq().jumpif('trava')
            a.getv(var).push(2).eq().not_().jumpif('on%d' % s)
            choose(s, var, 1)
            a.label('on%d' % s)
            choose(s, var, 2)
        elif s == 7:  # resolução 1..4
            for v in range(1, 5):
                nv = v + step
                a.getv(var).push(v).eq().not_().jumpif('r%d' % v)
                if 1 <= nv <= 4:
                    choose(s, var, nv)
                else:
                    a.jump('trava')
                a.label('r%d' % v)
            a.jump('trava')
        else:  # 30 / 60 FPS, contador Off / On
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


def make_dyn(f, var):
    tpl = next(d for d in f.dyns if f.s(struct.unpack_from('<H', d, 0)[0]) == 'PS2_MoveName1')
    d = bytearray(tpl)  # PS2_MoveName1: texto alinhado à esquerda
    struct.pack_into('<H', d, 0, f.add_string(var))
    struct.pack_into('<H', d, 2, 0xffff)
    struct.pack_into('<H', d, 6, 760)                # altura da fonte: a das maiúsculas dos rótulos (medido)
    struct.pack_into('<I', d, 8, 0xffcccccc)         # cinza dos rótulos do menu
    # caixa x, y, largura, altura (1/16 px). O texto se apoia na base da caixa: y + altura põe a linha de
    # base na do rótulo (medido nas capturas do menu)
    struct.pack_into('<hhHH', d, 0x16, 0, -40, 12000, 900)
    return d


def xform(f, src, x=None, y=None):
    t = bytearray(f.xforms[src])
    if x is not None:
        struct.pack_into('<h', t, 0x10, round(x * 16))
    if y is not None:
        struct.pack_into('<h', t, 0x12, round(y * 16))
    f.xforms.append(t)
    return len(f.xforms) - 1


def handler(f, typ, idx):
    f.handlers.append((typ, idx))
    return len(f.handlers) - 1


def mod_flp(b):
    f = flp.FLP(b)
    pm = f.c7[obj(f, named(f.root, f, 'PauseMenu'), 7)]
    sfx = f.c7[obj(f, named(pm, f, 'SFXVolume'), 7)]  # linha SFX Volume
    sfx_label = f.statics[obj(f, struct.unpack_from('<H', sfx.anims[0][1][0], 2)[0], 4)]

    # ---- linhas novas
    rows = {}
    for s, clip_name, text, _, _, txt_var in ROWS:
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

    # ---- posições: SFX e Controller descem/sobem para abrir espaço; as novas entre eles
    def move(elem_name, y):
        for h, keys in pm.anims:
            for k in keys:
                if f.s(struct.unpack_from('<H', k, 8)[0]) == elem_name and struct.unpack_from('<H', k, 0)[0] in (7, 8):
                    struct.pack_into('<H', k, 4, xform(f, struct.unpack_from('<H', k, 4)[0], y=y))
    move('SFXVolume', Y[2])
    move('Controller', Y[3])
    sfx_key = next(k for h, keys in pm.anims for k in keys
                   if f.s(struct.unpack_from('<H', k, 8)[0]) == 'SFXVolume' and struct.unpack_from('<H', k, 0)[0] == 7)
    t_sfx = struct.unpack_from('<H', sfx_key, 4)[0]
    depth = max(struct.unpack_from('<H', h, 0)[0] for h, _ in pm.anims)
    for s, (h_row, name) in rows.items():
        depth += 1
        t = xform(f, t_sfx, y=Y[s])
        nm = f.add_string(name)
        keys = [bytearray(struct.pack('<HHHHH', 7, h_row, t, 22, nm)),
                bytearray(struct.pack('<HHHHH', 8, h_row, t, 0, nm)),
                bytearray(struct.pack('<HHHHH', 9, 0, 0, 0, 0xffff))]
        head = bytearray(pm.anims[0][0])
        struct.pack_into('<H', head, 0, depth)
        pm.anims.append([head, keys])

    # ---- contador de FPS no estilo do contador de HITS: número grande (o rótulo dinâmico do HITS) com
    # "FPS" menor embaixo, num clipe de 2 quadros em laço no canto superior direito do HUD (800x600)
    hit = next(d for d in f.dyns if f.s(struct.unpack_from('<H', d, 0)[0]) == '_level0/:HitCounter')
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
    shake = f.c7[obj(f, struct.unpack_from('<H', hit_clip.anims[0][1][0], 2)[0], 7)]
    t_num = struct.unpack_from('<H', shake.anims[0][1][0], 4)[0]
    t_unit = struct.unpack_from('<H', shake.anims[1][1][0], 4)[0]
    c = flp.Clip()
    c.h = bytearray(shake.h)
    c.frames = 2
    a0 = [bytearray(shake.anims[0][0]), [bytearray(struct.pack('<HHHHH', 0, h_num, t_num, 0, 0xffff))]]
    a1 = [bytearray(shake.anims[1][0]), [bytearray(struct.pack('<HHHHH', 0, h_unit, t_unit, 0, 0xffff))]]
    c.anims = [a0, a1]
    lab_h = bytearray(hit_clip.labels[0][0])  # cabeçalho de rótulo de quadro
    sub_h = bytearray(hit_clip.labels[0][1][0][0])
    a = fps_counter_script()
    a.gotoframe(0).play().end()
    code1 = flp.encode(f, a.build())
    labs = []
    for frame, code in ((0, flp.encode(f, fps_counter_script().end().build())), (1, code1)):
        h = bytearray(lab_h)
        struct.pack_into('<H', h, 0, frame)
        struct.pack_into('<H', h, 8, 0xffff)
        labs.append([h, [[bytearray(sub_h), code]]])
    c.labels = labs
    f.c7.append(c)
    h_clip = handler(f, 7, len(f.c7) - 1)
    t = bytearray(f.xforms[0])
    struct.pack_into('<4i', t, 0, 49152, 0, 0, 49152)  # escala 0,75
    struct.pack_into('<hh', t, 0x10, round(730 * 16), round(52 * 16))
    f.xforms.append(t)
    depth = max(struct.unpack_from('<H', h, 0)[0] for h, _ in f.root.anims) + 2
    head = bytearray(f.root.anims[0][0])
    struct.pack_into('<H', head, 0, depth)
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
        struct.pack_into('<H', h, 0, frame)
        struct.pack_into('<H', h, 8, f.add_string(name))
        sh = bytearray(proto_subs[0][0])
        k = next(i for i, (hh, _) in enumerate(root.labels) if struct.unpack_from('<H', hh, 0)[0] > frame)
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

    patch_script(f, script_of(root, f, 'PressUp'), '1', call_then_end('/:VtNavUp'))
    patch_script(f, script_of(root, f, 'PressDown'), '1', call_then_end('/:VtNavDown'))
    patch_script(f, script_of(root, f, 'PressLeft'), '1', guarded('/:VtLeft'))
    patch_script(f, script_of(root, f, 'PressRight'), '1', guarded('/:VtRight'))
    x_ev = []  # botão X (0x40) com a página Options
    for c in f.c6:
        for ev in c.events:
            if struct.unpack_from('<I', ev[0], 8)[0] == 0x40:
                try:
                    find_check(flp.decode(f, ev[1])[0], '3')
                    x_ev.append(ev)
                except KeyError:
                    pass
    assert len(x_ev) == 1, len(x_ev)
    patch_script(f, x_ev[0], '3', guarded('/:VtRight'))
    # abrir a página Options: textos dos valores atualizados
    opt = script_of(pm, f, 'Options')
    ins, _ = flp.decode(f, opt[1])
    opt[1] = flp.encode(f, flp.insert(ins, 0, flp.Asm().callframe('/:VtOptShow').build(base=0)))
    return f.write()


# ---------------------------------------------------------------- WAD e psarc
def wad_replace(w, name, body):
    out, o = bytearray(), 0
    hit = 0
    while o + 32 <= len(w):
        tag, flags, size = struct.unpack_from('<HHI', w, o)
        nm = w[o + 8:o + 32].split(b'\0')[0]
        full = 32 + (((size + 15) & ~15) if tag != 24 else 0)
        if tag == 30 and nm == name:
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


def wad_get(w, name):
    o = 0
    while o + 32 <= len(w):
        tag, flags, size = struct.unpack_from('<HHI', w, o)
        if tag == 30 and w[o + 8:o + 32].split(b'\0')[0] == name:
            return w[o + 32:o + 32 + size]
        o += 32 + (((size + 15) & ~15) if tag != 24 else 0)
    raise KeyError(name)


def build_patch(psarc, files, out):
    """files: {nome no manifesto: bytes novos}. Cada arquivo novo vai comprimido (zlib, blocos do psarc)
    para depois do fim do psarc, reaproveitando as entradas de bloco do arquivo original."""
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
    extra = bytearray()
    for name, data in files.items():
        i = names.index(name) + 1
        idx, size, off = ents[i]
        nb_old = (size + block - 1) // block
        nb_new = (len(data) + block - 1) // block
        assert nb_new <= nb_old, '%s cresceu de %d para %d blocos' % (name, nb_old, nb_new)
        e = 32 + i * ent_size
        toc[e + 20:e + 25] = len(data).to_bytes(5, 'big')
        toc[e + 25:e + 30] = (orig + len(extra)).to_bytes(5, 'big')
        for k in range(nb_old):
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
    with open(out, 'wb') as fo:
        fo.write(b'VTPP' + struct.pack('<QII', orig, len(toc), len(extra)))
        fo.write(toc)
        fo.write(extra)
    print('%s: TOC %d bytes, %d bytes novos (%d arquivos)' % (out, len(toc), len(extra), len(files)))


def main():
    psarc, out = sys.argv[1], sys.argv[2]
    dump = sys.argv[sys.argv.index('--dump') + 1] if '--dump' in sys.argv else None
    files = {}
    with open(psarc, 'rb') as fh:
        ents, zs, block = P.entries(fh)
        names = P.read_file(fh, ents[0], zs, block).decode('latin1').split('\n')
        for name in ('wad/r_perma.wad_psp2', 'wad/_VitaTV/r_perma.wad_psp2'):
            w = P.read_file(fh, ents[names.index(name) + 1], zs, block)
            new_flp = mod_flp(wad_get(w, b'FLP_HUD'))
            files[name] = wad_replace(w, b'FLP_HUD', new_flp)
            if dump:
                os.makedirs(dump, exist_ok=True)
                open(os.path.join(dump, name.replace('/', '_') + '.flp'), 'wb').write(new_flp)
    build_patch(psarc, files, out)


if __name__ == '__main__':
    main()
