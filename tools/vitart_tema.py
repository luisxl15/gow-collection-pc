"""Tema visual dos programas do vitart (Troféus e Mod Studio), no espírito do God of War III: pedra escura, ouro
envelhecido, vermelho de sangue, o ômega e o friso grego (meandro).

As peças são desenhadas pelo PIL (texto dourado em relevo, frisos, molduras, anéis de progresso) e mostradas em
widgets do Tk. A arte de fundo é a do próprio jogo (sce_sys/pic0.png da pasta do app), lida ao abrir; sem ela,
fica só a pedra.
"""
import os
import random
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONTS = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')

# Direção (revisada com a skill UI/UX Pro Max): quase-preto com ouro só como acento, texto marfim sólido,
# vermelho apenas para o estado ativo/progresso, filetes de 1 px no lugar de molduras, um único friso grego
# discreto. Nada de degradê dourado em tudo, auréolas ou cantos ornamentados.
# Cores (hex para o Tk, tuplas para o PIL)
BG = '#0c0908'          # obsidiana
STONE = '#17110e'       # painel de pedra
STONE_HI = '#231a15'
LINE = '#3a2a1c'        # filete de bronze escuro
GOLD = '#d6a84e'
GOLD_HI = '#f3d98e'
GOLD_DIM = '#8c6a2e'
RED = '#a3170f'
RED_HI = '#d22a1c'
RED_DARK = '#4a0b07'
TEXT = '#e8dfcf'        # marfim
DIM = '#9a8e7e'

TYPE_RGB = {'P': (176, 196, 222), 'G': (226, 186, 78), 'S': (196, 200, 206), 'B': (192, 122, 70)}


def rgb(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def resource(name):
    """Arquivo que acompanha o programa (no exe do PyInstaller, dentro do pacote)."""
    base = getattr(sys, '_MEIPASS', os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


def font(size, kind='title'):
    """Constantia (serifa clássica, a mais próxima das letras gregas do GoW entre as do Windows) para títulos;
    Segoe UI para texto corrido."""
    names = {'title': ('constanb.ttf', 'georgiab.ttf', 'timesbd.ttf'),
             'serif': ('constan.ttf', 'georgia.ttf', 'times.ttf'),
             'italic': ('constani.ttf', 'georgiai.ttf', 'timesi.ttf'),
             'text': ('segoeui.ttf', 'arial.ttf'),
             'bold': ('segoeuib.ttf', 'arialbd.ttf')}[kind]
    for n in names:
        p = os.path.join(FONTS, n)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


TK_TITLE = ('Constantia', 13, 'bold')
TK_TEXT = ('Segoe UI', 10)


# ------------------------------------------------------------------ texturas
_stone_cache = {}


def stone(w, h, tint=(23, 17, 14), seed=7):
    """Pedra: ruído em várias escalas, tingido de marrom-acinzentado, com veios escuros."""
    key = (w, h, tint, seed)
    if key in _stone_cache:
        return _stone_cache[key].copy()
    rnd = random.Random(seed)
    acc = Image.new('L', (w, h), 128)
    for scale, weight in ((64, 0.45), (16, 0.3), (4, 0.18), (1, 0.07)):
        sw, sh = max(2, w // scale + 2), max(2, h // scale + 2)
        n = Image.frombytes('L', (sw, sh), bytes(rnd.randrange(256) for _ in range(sw * sh)))
        n = n.resize((w, h), Image.BICUBIC)
        acc = Image.blend(acc, n, weight)
    # veios: linhas finas e escuras com desvio aleatório
    veins = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(veins)
    for _ in range(max(2, w * h // 90000)):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        pts = [(x, y)]
        for _ in range(12):
            x += rnd.uniform(-40, 40)
            y += rnd.uniform(-25, 25)
            pts.append((x, y))
        d.line(pts, fill=rnd.randint(40, 90), width=1)
    acc = ImageChops.subtract(acc, veins.filter(ImageFilter.GaussianBlur(0.6)))
    lo = [max(0, c - 12) for c in tint]
    hi = [min(255, c + 22) for c in tint]
    img = Image.merge('RGB', [acc.point(lambda v, a=a, b=b: a + (b - a) * v // 255) for a, b in zip(lo, hi)])
    _stone_cache[key] = img
    return img.copy()


def vgradient(w, h, stops):
    """Degradê vertical; stops = [(posição 0..1, (r, g, b)), ...]."""
    col = Image.new('RGB', (1, h))
    px = col.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                k = (t - p0) / max(1e-6, p1 - p0)
                px[0, y] = tuple(int(a + (b - a) * k) for a, b in zip(c0, c1))
                break
    return col.resize((w, h))


GOLD_STOPS = [(0, (255, 244, 196)), (0.32, (240, 204, 120)), (0.5, (190, 136, 52)), (0.62, (226, 180, 92)),
              (1, (104, 64, 20))]
RED_STOPS = [(0, (240, 80, 60)), (0.45, (178, 24, 16)), (1, (70, 6, 4))]


# ------------------------------------------------------------------ texto em relevo
def metal_text(text, size, stops=GOLD_STOPS, kind='title', spacing=0.08, outline=(26, 14, 6), shadow=True):
    """Texto metálico (ouro por padrão): degradê com faixa de brilho, contorno escuro e sombra."""
    f = font(size, kind)
    gap = int(size * spacing)
    widths = [f.getlength(c) for c in text]
    pad = max(4, size // 6)
    w = int(sum(widths) + gap * max(0, len(text) - 1)) + pad * 2
    asc, desc = f.getmetrics()
    h = asc + desc + pad * 2
    mask = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(mask)
    x = pad
    for c, cw in zip(text, widths):
        d.text((x, pad), c, font=f, fill=255)
        x += cw + gap
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    if shadow:
        sh = mask.filter(ImageFilter.GaussianBlur(max(1, size // 14)))
        sh = ImageChops.offset(sh, max(1, size // 22), max(1, size // 16))
        out.paste((0, 0, 0, 230), (0, 0), sh)
    edge = mask.filter(ImageFilter.MaxFilter(3 if size < 40 else 5))
    out.paste(outline + (255,), (0, 0), edge)
    out.paste(vgradient(w, h, stops), (0, 0), mask)
    # relevo: borda de cima clara, de baixo escura
    hi = ImageChops.subtract(mask, ImageChops.offset(mask, 0, 1))
    out.paste((255, 250, 225, 160), (0, 0), hi)
    return out


def text_size(text, f):
    b = f.getbbox(text or ' ')
    return b[2] - b[0], b[3] - b[1]


def wrap(text, f, width):
    """Quebra por largura em pixels (palavra a palavra, respeitando quebras de linha)."""
    lines = []
    for para in text.split('\n'):
        cur = ''
        for word in para.split(' '):
            t = word if not cur else cur + ' ' + word
            if f.getlength(t) <= width or not cur:
                cur = t
            else:
                lines.append(cur)
                cur = word
        lines.append(cur)
    return lines


# ------------------------------------------------------------------ texto sólido
def plain_text(text, size, color=TEXT, kind='title', spacing=0.0, shadow=True):
    """Texto de cor sólida com sombra curta (sem degradê): a base da tipografia."""
    f = font(size, kind)
    gap = size * spacing
    w = int(sum(f.getlength(c) for c in text) + gap * max(0, len(text) - 1)) + 8
    asc, desc = f.getmetrics()
    h = asc + desc + 6
    mask = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(mask)
    x = 3
    for c in text:
        d.text((x, 2), c, font=f, fill=255)
        x += f.getlength(c) + gap
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    if shadow:
        out.paste((0, 0, 0, 200), (0, 0), ImageChops.offset(mask.filter(ImageFilter.GaussianBlur(1.2)), 0, 2))
    out.paste((rgb(color) if isinstance(color, str) else color) + (255,), (0, 0), mask)
    return out


def magnifier(size=16, color=GOLD_DIM):
    """Lupa desenhada (nada de emoji como ícone)."""
    s = 4
    S = size * s
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = rgb(color) + (255,)
    r = S * 0.3
    d.ellipse([S * 0.12, S * 0.12, S * 0.12 + 2 * r, S * 0.12 + 2 * r], outline=c, width=int(S * 0.1))
    d.line([(S * 0.62, S * 0.62), (S * 0.9, S * 0.9)], fill=c, width=int(S * 0.12))
    return img.resize((size, size), Image.LANCZOS)


def line_bar(w, frac, h=3, color=RED, track='#2a211b'):
    img = Image.new('RGBA', (w, h), rgb(track) + (255,))
    k = int(w * max(0.0, min(1.0, frac)))
    if k:
        ImageDraw.Draw(img).rectangle([0, 0, k - 1, h - 1], fill=rgb(color) + (255,))
    return img


def dot(size, ttype):
    """Marca do tipo de troféu: um ponto na cor do metal, com aro escuro."""
    s = 4
    S = size * s
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(img).ellipse([s, s, S - s, S - s], fill=TYPE_RGB[ttype] + (255,), outline=(12, 9, 8, 255),
                                width=s)
    return img.resize((size, size), Image.LANCZOS)


# ------------------------------------------------------------------ ornamentos
def meander(w, h=22, color=GOLD, bg=None):
    """Friso grego: filetes em cima e embaixo e as espirais do meandro entre eles."""
    s = 4  # supersample
    W, H = w * s, h * s
    img = Image.new('RGBA', (W, H), bg + (255,) if bg else (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    lw = max(s, H // 11)
    top, bot = lw, H - lw
    d.rectangle([0, 0, W, lw - 1], fill=rgb(color))
    d.rectangle([0, H - lw, W, H], fill=rgb(color))
    inner = bot - top
    u = inner / 6.0
    period = int(u * 7)
    x0 = -(period // 3)
    pts = lambda gx, gy, x: (x + gx * u, top + gy * u)  # noqa: E731
    while x0 < W:
        path = [pts(0.5, 6, x0), pts(0.5, 1, x0), pts(5.5, 1, x0), pts(5.5, 4.8, x0), pts(2.5, 4.8, x0),
                pts(2.5, 3, x0), pts(3.8, 3, x0)]
        d.line(path, fill=rgb(color), width=lw, joint='curve')
        x0 += period
    img = img.resize((w, h), Image.LANCZOS)
    shade = vgradient(w, h, [(0, (255, 236, 170)), (0.5, rgb(color)), (1, (120, 80, 30))]).convert('RGBA')
    shade.putalpha(img.getchannel('A'))
    return shade if not bg else Image.alpha_composite(Image.new('RGBA', (w, h), bg + (255,)), shade)


def omega(size, stops=RED_STOPS):
    """O ômega do GoW: Ω vermelho com contorno dourado."""
    f_size = int(size * 1.05)
    core = metal_text('Ω', f_size, stops=stops, kind='title', spacing=0, outline=(214, 168, 78), shadow=True)
    return core


def frame(img, color=GOLD, inset=0, corners=True):
    """Moldura dupla de ouro com cantos em losango, sobre a imagem (RGBA ou RGB)."""
    img = img.convert('RGBA')
    w, h = img.size
    d = ImageDraw.Draw(img)
    g, gd = rgb(color), rgb(GOLD_DIM)
    i = inset
    d.rectangle([i, i, w - 1 - i, h - 1 - i], outline=(10, 6, 4, 255), width=1)
    d.rectangle([i + 1, i + 1, w - 2 - i, h - 2 - i], outline=g + (255,), width=2)
    d.rectangle([i + 5, i + 5, w - 6 - i, h - 6 - i], outline=gd + (255,), width=1)
    if corners:
        r = 6
        for cx, cy in ((i + 3, i + 3), (w - 4 - i, i + 3), (i + 3, h - 4 - i), (w - 4 - i, h - 4 - i)):
            d.polygon([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)], fill=g + (255,), outline=(40, 24, 8, 255))
            d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=rgb(RED) + (255,))
    return img


def panel(w, h, tint=(23, 17, 14), seed=7, framed=True):
    img = stone(w, h, tint, seed).convert('RGBA')
    # vinheta: bordas mais escuras
    vig = Image.new('L', (w, h), 0)
    ImageDraw.Draw(vig).rectangle([w // 10, h // 10, w - w // 10, h - h // 10], fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(min(w, h) // 6 + 1))
    dark = Image.new('RGBA', (w, h), (0, 0, 0, 255))
    img = Image.composite(img, Image.blend(img, dark, 0.55), vig)
    return frame(img) if framed else img


def ring(size, frac, color=RED, track=(40, 28, 22), width_frac=0.13):
    """Anel de progresso: trilha escura, arco vermelho com borda de ouro."""
    s = 4
    S = size * s
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    lw = int(S * width_frac)
    box = [lw // 2 + s, lw // 2 + s, S - lw // 2 - s, S - lw // 2 - s]
    d.ellipse([s, s, S - s, S - s], outline=rgb(GOLD_DIM) + (255,), width=s * 2)
    d.arc(box, 0, 360, fill=track + (255,), width=lw)
    if frac > 0:
        d.arc(box, -90, -90 + 360 * min(1.0, frac), fill=rgb(color) + (255,), width=lw)
    inner = lw + s * 2
    d.ellipse([inner, inner, S - inner, S - inner], outline=rgb(GOLD) + (255,), width=s * 2)
    return img.resize((size, size), Image.LANCZOS)


def bar(w, h, frac):
    """Barra de progresso: trilha de pedra, enchimento vermelho com brilho, moldura de ouro."""
    img = Image.new('RGBA', (w, h), (18, 12, 10, 255))
    fill = int((w - 4) * max(0.0, min(1.0, frac)))
    if fill > 0:
        img.paste(vgradient(fill, h - 4, RED_STOPS), (2, 2))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], outline=rgb(GOLD), width=1)
    return img


def medal(size, ttype):
    """Medalhão do tipo de troféu (platina, ouro, prata, bronze)."""
    s = 4
    S = size * s
    c = TYPE_RGB[ttype]
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    base = vgradient(S, S, [(0, tuple(min(255, v + 70) for v in c)), (0.5, c), (1, tuple(v // 3 for v in c))])
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).ellipse([s, s, S - s, S - s], fill=255)
    img.paste(base, (0, 0), m)
    d = ImageDraw.Draw(img)
    d.ellipse([s, s, S - s, S - s], outline=(30, 18, 8, 255), width=s * 2)
    k = S // 4
    d.ellipse([k, k, S - k, S - k], outline=(30, 18, 8, 200), width=s)
    # estrela de 4 pontas no centro, como o brilho dos troféus
    c, r, q = S / 2, S * 0.17, S * 0.05
    d.polygon([(c, c - r), (c + q, c - q), (c + r, c), (c + q, c + q), (c, c + r), (c - q, c + q), (c - r, c),
               (c - q, c - q)], fill=(255, 250, 230, 230))
    return img.resize((size, size), Image.LANCZOS)


# ------------------------------------------------------------------ arte do jogo
def game_art(app_dir):
    """A arte do jogo (pic0 ou o fundo do LiveArea); None se não achar."""
    for rel in (('sce_sys', 'pic0.png'), ('sce_sys', 'livearea', 'contents', 'backgrounds', 'bg0.png')):
        p = os.path.join(app_dir or '', *rel)
        if os.path.exists(p):
            try:
                return Image.open(p).convert('RGB')
            except OSError:
                pass
    return None


def cover(img, w, h, focus_x=0.5):
    """Recorta e escala para cobrir w x h (como background-size: cover)."""
    iw, ih = img.size
    k = max(w / iw, h / ih)
    nw, nh = int(iw * k + 0.5), int(ih * k + 0.5)
    img = img.resize((nw, nh), Image.LANCZOS)
    x = int((nw - w) * focus_x)
    y = (nh - h) // 3
    return img.crop((x, y, x + w, y + h))


def banner(w, h, title, subtitle, art=None):
    """Cabeçalho: arte do jogo à direita, apagada para a esquerda; sobretítulo pequeno, título marfim e um
    friso grego fino e discreto no pé (o único ornamento)."""
    base = Image.new('RGBA', (w, h), rgb(BG) + (255,))
    if art is not None:
        a = cover(art, w, h, focus_x=0.85).convert('RGBA')
        a = Image.blend(a, Image.new('RGBA', a.size, (0, 0, 0, 255)), 0.25)
        fade = Image.linear_gradient('L').rotate(90).resize((w, h))
        fade = fade.point(lambda v: min(255, max(0, int((v - 95) * 1.6))))
        base = Image.composite(a, base, fade)
    x = 40
    over = plain_text('VITART', 12, GOLD_DIM, kind='bold', spacing=0.35, shadow=False)
    base.alpha_composite(over, (x, int(h * 0.24)))
    t = plain_text(title, int(h * 0.3), TEXT, kind='title', spacing=0.02)
    base.alpha_composite(t, (x - 3, int(h * 0.24) + over.height + 2))
    st = plain_text(subtitle, 13, DIM, kind='text', spacing=0.12, shadow=False)
    base.alpha_composite(st, (x, int(h * 0.24) + over.height + t.height + 2))
    strip = meander(w, 12)
    strip.putalpha(strip.getchannel('A').point(lambda v: v * 45 // 100))
    base.alpha_composite(strip, (0, h - strip.height))
    return base


def button(text, w, h, state='normal'):
    """Botão de bronze com texto em ouro; estados normal, hover, active (selecionado) e disabled."""
    if state == 'active':
        stops = [(0, (150, 30, 20)), (0.5, (110, 18, 12)), (1, (60, 8, 6))]
    elif state == 'hover':
        stops = [(0, (92, 66, 40)), (0.5, (62, 44, 28)), (1, (34, 24, 16))]
    else:
        stops = [(0, (64, 46, 30)), (0.5, (42, 30, 21)), (1, (24, 17, 12))]
    img = vgradient(w, h, stops).convert('RGBA')
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], outline=rgb(GOLD if state != 'disabled' else GOLD_DIM), width=1)
    d.line([(2, 1), (w - 3, 1)], fill=(255, 230, 160, 70))
    t = metal_text(text.upper(), max(10, int(h * 0.42)), spacing=0.12, shadow=False,
                   stops=GOLD_STOPS if state != 'disabled' else [(0, (120, 110, 95)), (1, (80, 70, 60))])
    img.alpha_composite(t, ((w - t.width) // 2, (h - t.height) // 2))
    return img


def app_icon(size=256):
    """Ícone dos programas: ômega sobre medalhão de pedra com aro de ouro."""
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).ellipse([4, 4, size - 4, size - 4], fill=255)
    img.paste(stone(size, size, (30, 22, 18), seed=11), (0, 0), m)
    d = ImageDraw.Draw(img)
    d.ellipse([4, 4, size - 4, size - 4], outline=rgb(GOLD) + (255,), width=max(3, size // 24))
    d.ellipse([size // 9, size // 9, size - size // 9, size - size // 9], outline=rgb(GOLD_DIM) + (255,),
              width=max(1, size // 64))
    om = omega(int(size * 0.62))
    img.alpha_composite(om, ((size - om.width) // 2, (size - om.height) // 2 - size // 30))
    return img


# ------------------------------------------------------------------ ttk
def style_ttk(root):
    """Estilo ttk escuro com ouro, para as telas feitas com widgets comuns (Mod Studio)."""
    from tkinter import ttk
    root.configure(bg=BG)
    for opt, val in (('*Text.background', '#120d0b'), ('*Text.foreground', TEXT), ('*Text.insertBackground', GOLD_HI),
                     ('*Text.selectBackground', RED_DARK), ('*Text.relief', 'flat'), ('*Text.highlightThickness', 1),
                     ('*Text.highlightColor', GOLD_DIM), ('*Text.highlightBackground', LINE),
                     ('*Listbox.background', '#120d0b'), ('*Listbox.foreground', TEXT),
                     ('*Listbox.selectBackground', RED_DARK), ('*Canvas.background', '#0f0b09'),
                     ('*Canvas.highlightThickness', 0), ('*Menu.background', STONE), ('*Menu.foreground', TEXT),
                     ('*TCombobox*Listbox.background', '#120d0b'), ('*TCombobox*Listbox.foreground', TEXT),
                     ('*TCombobox*Listbox.selectBackground', RED_DARK)):
        root.option_add(opt, val)
    st = ttk.Style(root)
    st.theme_use('clam')
    st.configure('.', background=BG, foreground=TEXT, fieldbackground='#120d0b', bordercolor=LINE,
                 lightcolor=STONE_HI, darkcolor='#070504', troughcolor='#100c0a', focuscolor=GOLD_DIM,
                 selectbackground=RED_DARK, selectforeground=GOLD_HI, insertcolor=GOLD_HI, font=TK_TEXT)
    st.configure('TFrame', background=BG)
    st.configure('Card.TFrame', background=STONE)
    st.configure('TLabel', background=BG, foreground=TEXT)
    st.configure('Title.TLabel', foreground=GOLD, font=TK_TITLE)
    st.configure('Dim.TLabel', foreground=DIM)
    st.configure('TLabelframe', background=BG, bordercolor=LINE)
    st.configure('TLabelframe.Label', background=BG, foreground=GOLD_DIM, font=('Segoe UI Semibold', 9))
    st.configure('TButton', background='#1d1713', foreground=TEXT, bordercolor=LINE, lightcolor='#1d1713',
                 darkcolor='#1d1713', padding=(14, 7), font=('Segoe UI Semibold', 10))
    st.map('TButton', background=[('pressed', RED_DARK), ('active', '#281f19'), ('disabled', '#15110e')],
           foreground=[('disabled', '#6b5e4c'), ('active', GOLD_HI)],
           bordercolor=[('active', GOLD_DIM), ('focus', GOLD_DIM)])
    st.configure('Accent.TButton', background='#6e140e', foreground=GOLD_HI, bordercolor=GOLD)
    st.map('Accent.TButton', background=[('pressed', '#3e0a07'), ('active', '#8e1c13')])
    st.configure('TCheckbutton', background=BG, foreground=TEXT, indicatorbackground='#120d0b',
                 indicatorforeground=GOLD_HI)
    st.map('TCheckbutton', background=[('active', BG)], indicatorbackground=[('selected', RED)])
    st.configure('TRadiobutton', background=BG, foreground=TEXT, indicatorbackground='#120d0b')
    st.map('TRadiobutton', background=[('active', BG)], indicatorbackground=[('selected', RED)])
    st.configure('TEntry', fieldbackground='#120d0b', foreground=TEXT, bordercolor=GOLD_DIM, insertcolor=GOLD_HI)
    st.map('TEntry', bordercolor=[('focus', GOLD)])
    st.configure('TCombobox', fieldbackground='#120d0b', background='#2b2017', foreground=TEXT, arrowcolor=GOLD)
    st.map('TCombobox', fieldbackground=[('readonly', '#120d0b')], foreground=[('readonly', TEXT)])
    st.configure('TSpinbox', fieldbackground='#120d0b', foreground=TEXT, arrowcolor=GOLD)
    st.configure('TNotebook', background=BG, bordercolor=GOLD_DIM, tabmargins=(10, 6, 10, 0))
    st.configure('TNotebook.Tab', background=BG, foreground=DIM, bordercolor=LINE, lightcolor=BG, padding=(18, 8),
                 font=('Segoe UI Semibold', 10))
    st.map('TNotebook.Tab', background=[('selected', '#1d1713'), ('active', '#15110e')],
           foreground=[('selected', TEXT), ('active', GOLD)], lightcolor=[('selected', RED_HI)])
    st.configure('Treeview', background='#120d0b', fieldbackground='#120d0b', foreground=TEXT, rowheight=28,
                 bordercolor=LINE)
    st.map('Treeview', background=[('selected', '#5e120c')], foreground=[('selected', GOLD_HI)])
    st.configure('Treeview.Heading', background='#15110e', foreground=GOLD_DIM, bordercolor=LINE,
                 font=('Segoe UI Semibold', 9), relief='flat')
    st.map('Treeview.Heading', background=[('active', '#3e2d1e')])
    st.configure('Vertical.TScrollbar', background='#2b2017', troughcolor='#0f0b09', bordercolor=LINE,
                 arrowcolor=GOLD, lightcolor='#3e2d1e', darkcolor='#140e0a')
    st.configure('Horizontal.TScrollbar', background='#2b2017', troughcolor='#0f0b09', bordercolor=LINE,
                 arrowcolor=GOLD, lightcolor='#3e2d1e', darkcolor='#140e0a')
    for o in ('Vertical.TScrollbar', 'Horizontal.TScrollbar'):
        st.configure(o, gripcount=0, arrowsize=12)
        st.map(o, background=[('pressed', '#6e140e'), ('active', '#4a3220'), ('!disabled', '#2b2017')],
               arrowcolor=[('disabled', GOLD_DIM), ('!disabled', GOLD)])
    st.configure('Horizontal.TProgressbar', background=RED, troughcolor='#120d0b', bordercolor=GOLD_DIM,
                 lightcolor=RED_HI, darkcolor=RED_DARK)
    st.configure('TPanedwindow', background=BG)
    st.configure('Sash', sashthickness=6, gripcount=0, background=LINE)
    return st


def set_icon(root):
    """Ícone da janela (o mesmo do exe)."""
    from PIL import ImageTk
    try:
        root._vt_icon = ImageTk.PhotoImage(app_icon(64))
        root.iconphoto(True, root._vt_icon)
    except Exception:  # noqa: BLE001 - ícone é enfeite
        pass
