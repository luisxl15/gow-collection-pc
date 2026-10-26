"""vitart Troféus: progresso e troféus do God of War Collection no vitart.

    python tools/vitart_trofeus.py        (ou o "vitart Troféus.exe", tools/build_exes.py)

Mostra, para cada jogo, quanto foi feito (porcentagem como a do PlayStation, pelos pontos de cada troféu:
bronze 15, prata 30, ouro 90, platina 180), quais troféus foram pegos e quais faltam, com a descrição e a
dica de como pegar (tools/trofeus_guia.json). A lista vem do TROP.SFM de dentro do TROPHY.TRP do jogo; os
desbloqueados, do <vitart-data>/trophy/<NPWR>/unlocked.txt que o vitart grava. A janela se atualiza
sozinha enquanto o jogo roda. Visual em tools/vitart_tema.py.
"""
import io
import json
import os
import re
import struct
import sys
import tkinter as tk
from tkinter import ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vitart_modkit as K  # noqa: E402  (caminhos do jogo e do vitart-data)
import vitart_tema as T  # noqa: E402

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageOps, ImageTk  # noqa: E402

GAMES = [('NPWR05244_00', 'God of War'), ('NPWR05283_00', 'God of War II')]
POINTS = {'B': 15, 'S': 30, 'G': 90, 'P': 180}
TYPE_NAME = {'P': 'Platina', 'G': 'Ouro', 'S': 'Prata', 'B': 'Bronze'}
GUIDE = T.resource('trofeus_guia.json')

ROW_H = 84
DETAIL_W = 450
CARD_W, CARD_H = 400, 104


def read_trp(path):
    """{nome: bytes} das entradas do TROPHY.TRP (cabeçalho big-endian, entradas de 64 bytes a partir de 0x40)."""
    d = open(path, 'rb').read()
    if d[:4] != b'\xdc\xa2\x4d\x00':
        raise ValueError('não é TROPHY.TRP: %s' % path)
    n, esz = struct.unpack_from('>II', d, 16)
    out = {}
    for i in range(n):
        e = 0x40 + i * esz
        name = d[e:e + 32].split(b'\0')[0].decode('latin-1')
        off, size = struct.unpack_from('>QQ', d, e + 32)
        out[name] = d[off:off + size]
    return out


def load_set(cfg, comm, guide):
    files = read_trp(os.path.join(cfg['app'], 'sce_sys', 'trophy', comm, 'TROPHY.TRP'))
    xml = files['TROP.SFM'].decode('utf-8', 'replace')
    trophies = []
    for m in re.finditer(r'<trophy id="(\d+)" hidden="(\w+)" ttype="(\w)".*?<name>(.*?)</name>\s*<detail>(.*?)</detail>',
                         xml, re.S):
        tid, hidden, ttype, name, detail = m.groups()
        g = guide.get(comm, {}).get(tid, {})
        icon = files.get('TROP%s.PNG' % tid)
        trophies.append(dict(id=int(tid), hidden=hidden == 'yes', type=ttype, name=name, detail=detail,
                             desc=g.get('desc', detail), tip=g.get('dica', ''),
                             icon=Image.open(io.BytesIO(icon)).convert('RGBA') if icon else None))
    return trophies


def unlocked_path(cfg, comm):
    return os.path.join(cfg['data'], 'trophy', comm, 'unlocked.txt')


def read_unlocked(cfg, comm):
    p = unlocked_path(cfg, comm)
    if not os.path.exists(p):
        return set()
    return {int(x) for x in open(p, encoding='utf-8', errors='replace').read().split() if x.isdigit()}


def progress(trophies, got):
    total = sum(POINTS[t['type']] for t in trophies)
    have = sum(POINTS[t['type']] for t in trophies if t['id'] in got)
    return (100.0 * have / total) if total else 0.0


def gray(img):
    g = ImageOps.grayscale(img).convert('RGBA')
    g.putalpha(img.getchannel('A'))
    return ImageEnhance.Brightness(g).enhance(0.45)


def ellipsize(text, f, width):
    if f.getlength(text) <= width:
        return text
    while text and f.getlength(text + '…') > width:
        text = text[:-1]
    return text.rstrip() + '…'


def framed_icon(img, size, got):
    """Ícone do troféu numa moldura de ouro (opaca se falta)."""
    pic = (img or Image.new('RGBA', (size, size), (50, 40, 34, 255))).resize((size - 8, size - 8), Image.LANCZOS)
    if not got:
        pic = gray(pic)
    out = Image.new('RGBA', (size, size), (12, 8, 6, 255))
    out.alpha_composite(pic, (4, 4))
    d = ImageDraw.Draw(out)
    gold = T.rgb(T.GOLD if got else T.GOLD_DIM)
    d.rectangle([0, 0, size - 1, size - 1], outline=(8, 5, 3, 255), width=1)
    d.rectangle([1, 1, size - 2, size - 2], outline=gold + (255,), width=2)
    return out


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('vitart Troféus')
        self.geometry('1320x860')
        self.minsize(1080, 680)
        self.configure(bg=T.BG)
        T.set_icon(self)
        T.style_ttk(self)
        self.cfg = K.load_config()
        guide = json.load(open(GUIDE, encoding='utf-8')) if os.path.exists(GUIDE) else {}
        self.sets, self.errors = {}, []
        for comm, title in GAMES:
            try:
                self.sets[comm] = load_set(self.cfg, comm, guide)
            except Exception as e:  # noqa: BLE001 - um jogo sem TRP não impede o outro
                self.errors.append('%s: %s' % (title, e))
        self.got = {comm: read_unlocked(self.cfg, comm) for comm in self.sets}
        self.mtimes = {}
        self.art = T.game_art(self.cfg.get('app'))
        recent = sorted(self.sets, key=lambda c: os.path.getmtime(unlocked_path(self.cfg, c))
                        if os.path.exists(unlocked_path(self.cfg, c)) else 0)
        self.game = recent[-1] if recent else None
        self.filter = 'todos'
        self.search = tk.StringVar()
        self.rows = []         # troféus visíveis, na ordem da lista
        self.sel = None        # id selecionado
        self.hover = None      # índice sob o mouse
        self.photos = {}       # PhotoImages vivas por chave
        self.row_items = []
        self._resize_job = None
        self._build()
        self.refresh()
        self.after(2000, self.poll)

    # ------------------------------------------------------------------ montagem
    def _photo(self, key, img):
        self.photos[key] = ImageTk.PhotoImage(img)
        return self.photos[key]

    def _build(self):
        self.head = tk.Label(self, bg=T.BG, bd=0)
        self.head.pack(fill='x')

        bar = tk.Frame(self, bg=T.BG)
        bar.pack(fill='x', padx=18, pady=(14, 8))
        self.cards = {}
        for comm, _title in GAMES:
            if comm not in self.sets:
                continue
            c = tk.Label(bar, bg=T.BG, bd=0, cursor='hand2')
            c.pack(side='left', padx=(0, 14))
            c.bind('<Button-1>', lambda _e, cm=comm: self.select_game(cm))
            self.cards[comm] = c

        right = tk.Frame(bar, bg=T.BG)
        right.pack(side='right', fill='y')
        self.fbtns = {}
        fb = tk.Frame(right, bg=T.BG)
        fb.pack(side='top', anchor='e', pady=(8, 8))
        for val, label in (('todos', 'Todos'), ('obtidos', 'Obtidos'), ('faltando', 'Faltando')):
            b = tk.Label(fb, bg=T.BG, bd=0, cursor='hand2')
            b.pack(side='left', padx=4)
            b.bind('<Button-1>', lambda _e, v=val: self.set_filter(v))
            b.bind('<Enter>', lambda _e, v=val: self._paint_filters(hover=v))
            b.bind('<Leave>', lambda _e: self._paint_filters())
            self.fbtns[val] = (b, label)
        sb = tk.Frame(right, bg=T.GOLD_DIM, padx=1, pady=1)
        sb.pack(side='top', anchor='e', fill='x', padx=4)
        inner = tk.Frame(sb, bg='#120d0b')
        inner.pack(fill='x')
        tk.Label(inner, text='⌕', bg='#120d0b', fg=T.GOLD, font=('Segoe UI Symbol', 12)).pack(side='left', padx=(8, 2))
        ent = tk.Entry(inner, textvariable=self.search, bg='#120d0b', fg=T.TEXT, insertbackground=T.GOLD_HI,
                       relief='flat', font=('Segoe UI', 11), width=30, highlightthickness=0)
        ent.pack(side='left', fill='x', expand=True, ipady=6, padx=(2, 8))
        self.search.trace_add('write', lambda *_: self.fill())

        self.summary = tk.Label(self, bg=T.BG, bd=0)
        self.summary.pack(fill='x', padx=18, pady=(0, 6))

        body = tk.Frame(self, bg=T.BG)
        body.pack(fill='both', expand=True, padx=18, pady=(0, 6))
        self.detail = tk.Label(body, bg=T.BG, bd=0)
        self.detail.pack(side='right', fill='y', padx=(14, 0))
        holder = tk.Frame(body, bg=T.GOLD_DIM, padx=1, pady=1)
        holder.pack(side='left', fill='both', expand=True)
        self.list = tk.Canvas(holder, bg='#0f0b09', highlightthickness=0, bd=0)
        self.list.pack(side='left', fill='both', expand=True)
        self.scroll = ttk.Scrollbar(holder, command=self.list.yview)
        self.scroll.pack(side='right', fill='y')
        self.list.configure(yscrollcommand=self.scroll.set)
        self.list.bind('<Configure>', lambda _e: self._schedule_resize())
        self.list.bind('<Motion>', self._on_motion)
        self.list.bind('<Leave>', lambda _e: self._set_hover(None))
        self.list.bind('<Button-1>', self._on_click)
        self.bind_all('<MouseWheel>', lambda e: self.list.yview_scroll(-e.delta // 120 * 2, 'units'))
        self.bind('<Up>', lambda _e: self._step(-1))
        self.bind('<Down>', lambda _e: self._step(1))

        self.foot = tk.Label(self, bg=T.BG, bd=0)
        self.foot.pack(fill='x', side='bottom')
        self.bind('<Configure>', lambda e: self._schedule_resize() if e.widget is self else None)

    def _schedule_resize(self):
        if self._resize_job:
            self.after_cancel(self._resize_job)
        self._resize_job = self.after(120, self._relayout)

    def _relayout(self):
        self._resize_job = None
        w = max(800, self.winfo_width())
        if getattr(self, '_head_w', None) != w:
            self._head_w = w
            self.head.config(image=self._photo('head', T.banner(w, 190, 'TROFÉUS', 'GOD OF WAR COLLECTION', self.art)))
            self.foot.config(image=self._photo('foot', T.meander(w, 18)))
        self._paint_rows()
        self.show()

    # ------------------------------------------------------------------ dados
    def poll(self):
        """Relê o unlocked.txt quando muda (o vitart grava a cada troféu)."""
        changed = False
        for comm in self.sets:
            p = unlocked_path(self.cfg, comm)
            mt = os.path.getmtime(p) if os.path.exists(p) else 0
            if self.mtimes.get(comm) != mt:
                self.mtimes[comm] = mt
                new = read_unlocked(self.cfg, comm)
                if new != self.got[comm]:
                    self.got[comm] = new
                    changed = True
        if changed:
            self.refresh(keep=True)
        self.after(2000, self.poll)

    def select_game(self, comm):
        if comm != self.game:
            self.game = comm
            self.sel = None
            self.list.yview_moveto(0)
            self.refresh()

    def set_filter(self, val):
        self.filter = val
        self._paint_filters()
        self.fill()

    def refresh(self, keep=False):
        for comm, lab in self.cards.items():
            lab.config(image=self._photo(('card', comm), self._card(comm)))
        self._paint_filters()
        self.fill(keep)

    def fill(self, keep=False):
        if not self.game:
            return
        tr, got = self.sets[self.game], self.got[self.game]
        q = self.search.get().strip().lower()
        rows = []
        for t in tr:
            has = t['id'] in got
            if (self.filter == 'obtidos' and not has) or (self.filter == 'faltando' and has):
                continue
            if q and q not in (t['name'] + ' ' + t['desc'] + ' ' + t['detail']).lower():
                continue
            rows.append(t)
        self.rows = rows
        if not (keep and any(t['id'] == self.sel for t in rows)):
            self.sel = rows[0]['id'] if rows else None
        self._paint_summary()
        self._paint_rows()
        self.show()

    # ------------------------------------------------------------------ desenho
    def _card(self, comm):
        tr, got = self.sets[comm], self.got[comm]
        pct = progress(tr, got)
        on = comm == self.game
        img = T.panel(CARD_W, CARD_H, tint=(40, 14, 11) if on else (23, 17, 14), seed=5 if on else 7, framed=False)
        img = T.frame(img, color=T.GOLD if on else T.GOLD_DIM, corners=on)
        rg = T.ring(78, pct / 100)
        img.alpha_composite(rg, (14, (CARD_H - 78) // 2))
        p = T.metal_text('%d%%' % int(pct), 20 if pct < 99.5 else 16, spacing=0, shadow=False)
        img.alpha_composite(p, (14 + (78 - p.width) // 2, (CARD_H - p.height) // 2))
        name = dict(GAMES)[comm].upper()
        t = T.metal_text(name, 22, spacing=0.1)
        img.alpha_composite(t, (106, 12))
        n = sum(1 for x in tr if x['id'] in got)
        d = ImageDraw.Draw(img)
        d.text((110, 46), '%d de %d troféus' % (n, len(tr)), font=T.font(14, 'serif'), fill=T.rgb(T.TEXT))
        x = 110
        for ty in 'PGSB':
            tot = [x_ for x_ in tr if x_['type'] == ty]
            if not tot:
                continue
            img.alpha_composite(T.medal(18, ty), (x, 72))
            s = '%d/%d' % (sum(1 for x_ in tot if x_['id'] in got), len(tot))
            d.text((x + 22, 72), s, font=T.font(13, 'text'), fill=T.rgb(T.DIM))
            x += 30 + int(T.font(13, 'text').getlength(s)) + 8
        return img

    def _paint_filters(self, hover=None):
        for val, (b, label) in self.fbtns.items():
            st = 'active' if val == self.filter else 'hover' if val == hover else 'normal'
            b.config(image=self._photo(('f', val), T.button(label, 128, 34, st)))

    def _paint_summary(self):
        tr, got = self.sets[self.game], self.got[self.game]
        w = max(600, self.winfo_width() - 36)
        img = Image.new('RGBA', (w, 30), T.rgb(T.BG) + (255,))
        title = T.metal_text('%s  ·  %d troféus na lista' % (dict(GAMES)[self.game], len(self.rows)), 17,
                             kind='serif', spacing=0.04,
                             stops=[(0, (236, 220, 190)), (1, (160, 140, 110))], shadow=False)
        img.alpha_composite(title, (0, (30 - title.height) // 2))
        self.summary.config(image=self._photo('summary', img))

    def _row(self, t, idx, w):
        has = t['id'] in self.got[self.game]
        sel, hov = t['id'] == self.sel, idx == self.hover
        base = T.stone(w, ROW_H, (22, 16, 13), seed=100 + idx % 7).convert('RGBA')
        tint = (60, 12, 8, 150) if sel else (60, 44, 30, 60) if hov else (0, 0, 0, 90 if idx % 2 else 50)
        base.alpha_composite(Image.new('RGBA', (w, ROW_H), tint))
        if sel:  # brilho vermelho vindo da esquerda
            glow = Image.linear_gradient('L').rotate(90).resize((w, ROW_H)).point(lambda v: 255 - v)
            red = Image.new('RGBA', (w, ROW_H), (170, 26, 16, 0))
            red.putalpha(glow.point(lambda v: v * 110 // 255))
            base.alpha_composite(red)
        d = ImageDraw.Draw(base)
        d.line([(0, ROW_H - 1), (w, ROW_H - 1)], fill=T.rgb(T.LINE) + (255,))
        if sel:
            d.rectangle([0, 0, w - 1, ROW_H - 1], outline=T.rgb(T.GOLD) + (255,), width=1)
            d.rectangle([0, 0, 3, ROW_H - 1], fill=T.rgb(T.RED_HI) + (255,))
        ic = framed_icon(t['icon'], 64, has)
        base.alpha_composite(ic, (14, (ROW_H - 64) // 2))
        right = 150
        fname = T.font(19, 'title')
        name = ellipsize(t['name'], fname, w - 96 - right)
        if has:
            nm = T.metal_text(name, 19, spacing=0.02, shadow=True)
            base.alpha_composite(nm, (92, 8))
        else:
            d.text((96, 14), name, font=fname, fill=(170, 156, 136, 255))
        fdesc = T.font(13, 'text')
        d.text((96, 46), ellipsize(t['desc'], fdesc, w - 96 - right), font=fdesc,
               fill=T.rgb(T.TEXT if has else T.DIM) + (255,))
        base.alpha_composite(T.medal(30, t['type']), (w - right + 8, (ROW_H - 30) // 2))
        status = 'OBTIDO' if has else 'BLOQUEADO'
        d.text((w - right + 46, ROW_H // 2 - 16), TYPE_NAME[t['type']].upper(), font=T.font(12, 'bold'),
               fill=T.TYPE_RGB[t['type']] + (255,))
        d.text((w - right + 46, ROW_H // 2 + 2), status, font=T.font(12, 'title'),
               fill=(T.rgb(T.GOLD) if has else T.rgb(T.DIM)) + (255,))
        return base

    def _paint_rows(self):
        c = self.list
        w = max(300, c.winfo_width())
        c.delete('all')
        self.row_items = []
        for i, t in enumerate(self.rows):
            ph = self._photo(('row', i), self._row(t, i, w))
            self.row_items.append(c.create_image(0, i * ROW_H, image=ph, anchor='nw'))
        if not self.rows:
            msg = T.metal_text('Nenhum troféu aqui', 22, kind='serif', shadow=False)
            ph = self._photo('empty', msg)
            c.create_image(w // 2, 60, image=ph)
        c.configure(scrollregion=(0, 0, w, max(1, len(self.rows) * ROW_H)))

    def _repaint_row(self, i):
        if i is None or not (0 <= i < len(self.rows)) or i >= len(self.row_items):
            return
        w = max(300, self.list.winfo_width())
        ph = self._photo(('row', i), self._row(self.rows[i], i, w))
        self.list.itemconfigure(self.row_items[i], image=ph)

    def _index_at(self, y):
        i = int(self.list.canvasy(y) // ROW_H)
        return i if 0 <= i < len(self.rows) else None

    def _set_hover(self, i):
        if i != self.hover:
            old, self.hover = self.hover, i
            self._repaint_row(old)
            self._repaint_row(i)

    def _on_motion(self, e):
        self._set_hover(self._index_at(e.y))

    def _select_index(self, i):
        if i is None:
            return
        old = next((k for k, t in enumerate(self.rows) if t['id'] == self.sel), None)
        self.sel = self.rows[i]['id']
        self._repaint_row(old)
        self._repaint_row(i)
        self.show()

    def _on_click(self, e):
        self._select_index(self._index_at(e.y))

    def _step(self, k):
        cur = next((i for i, t in enumerate(self.rows) if t['id'] == self.sel), -1)
        i = max(0, min(len(self.rows) - 1, cur + k))
        if self.rows:
            self._select_index(i)
            top, bot = self.list.canvasy(0), self.list.canvasy(self.list.winfo_height())
            if i * ROW_H < top or (i + 1) * ROW_H > bot:
                self.list.yview_moveto(max(0, i * ROW_H - ROW_H) / max(1, len(self.rows) * ROW_H))

    def show(self):
        h = max(420, self.list.winfo_height() + 2)
        img = T.panel(DETAIL_W, h, seed=21)
        t = next((x for x in self.sets.get(self.game, []) if x['id'] == self.sel), None) if self.game else None
        if t is None:
            self.detail.config(image=self._photo('detail', img))
            return
        has = t['id'] in self.got[self.game]
        d = ImageDraw.Draw(img)
        # ícone grande com auréola vermelha quando obtido
        size = 168
        cx = DETAIL_W // 2
        if has:
            halo = Image.new('RGBA', (size + 80, size + 80), (0, 0, 0, 0))
            ImageDraw.Draw(halo).ellipse([10, 10, size + 70, size + 70], fill=(190, 40, 20, 120))
            halo = halo.filter(ImageFilter.GaussianBlur(22))
            img.alpha_composite(halo, (cx - halo.width // 2, 28 - 40))
        ic = framed_icon(t['icon'], size, has)
        img.alpha_composite(ic, (cx - size // 2, 28))
        y = 28 + size + 14
        # nome em ouro, em quantas linhas couber
        fname = T.font(25, 'title')
        for line in T.wrap(t['name'], fname, DETAIL_W - 60)[:3]:
            nm = T.metal_text(line, 25, spacing=0.03) if has else None
            if nm is None:
                tw = int(fname.getlength(line))
                d.text((cx - tw // 2, y + 4), line, font=fname, fill=(196, 184, 164, 255))
                y += 34
            else:
                img.alpha_composite(nm, (cx - nm.width // 2, y))
                y += nm.height - 6
        y += 6
        md = T.medal(26, t['type'])
        label = '%s  ·  %s' % (TYPE_NAME[t['type']].upper(), 'OBTIDO' if has else 'AINDA NÃO')
        fl = T.font(14, 'title')
        lw = int(fl.getlength(label))
        x0 = cx - (lw + 34) // 2
        img.alpha_composite(md, (x0, y))
        d.text((x0 + 34, y + 4), label, font=fl, fill=(T.TYPE_RGB[t['type']] if has else T.rgb(T.DIM)) + (255,))
        y += 40
        img.alpha_composite(T.meander(DETAIL_W - 60, 14), (30, y))
        y += 28
        ftext, fen = T.font(15, 'serif'), T.font(13, 'italic')

        def section(title, body, f, color, y):
            hd = T.metal_text(title, 15, spacing=0.18, shadow=False)
            img.alpha_composite(hd, (30, y))
            y += hd.height + 2
            for line in T.wrap(body, f, DETAIL_W - 64):
                if y > h - 30:
                    break
                d.text((32, y), line, font=f, fill=color)
                y += int(f.size * 1.35)
            return y + 10

        y = section('O QUE PEDE', t['desc'], ftext, T.rgb(T.TEXT) + (255,), y)
        if t['desc'] != t['detail'] and y < h - 60:
            for line in T.wrap(t['detail'], fen, DETAIL_W - 64):
                if y > h - 30:
                    break
                d.text((32, y), line, font=fen, fill=T.rgb(T.DIM) + (255,))
                y += int(fen.size * 1.35)
            y += 10
        if t['tip'] and y < h - 60:
            section('COMO PEGAR', t['tip'], ftext, T.rgb(T.TEXT) + (255,), y)
        self.detail_img = img
        self.detail.config(image=self._photo('detail', img))


def main():
    app = App()
    if app.errors:
        from tkinter import messagebox
        messagebox.showwarning('vitart Troféus', '\n'.join(app.errors))
    app.mainloop()


if __name__ == '__main__':
    main()
