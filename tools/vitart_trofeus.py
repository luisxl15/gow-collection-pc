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

from PIL import Image, ImageDraw, ImageEnhance, ImageOps, ImageTk  # noqa: E402

GAMES = [('NPWR05244_00', 'God of War'), ('NPWR05283_00', 'God of War II')]
POINTS = {'B': 15, 'S': 30, 'G': 90, 'P': 180}
TYPE_NAME = {'P': 'Platina', 'G': 'Ouro', 'S': 'Prata', 'B': 'Bronze'}
GUIDE = T.resource('trofeus_guia.json')

ROW_H = 76
DETAIL_W = 450
CARD_W, CARD_H = 330, 92
SURF, SURF_HI, SURF_SEL = (18, 14, 12), (26, 20, 17), (32, 20, 16)


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
    pic = (img or Image.new('RGBA', (size, size), (50, 40, 34, 255))).resize((size - 2, size - 2), Image.LANCZOS)
    if not got:
        pic = gray(pic)
    out = Image.new('RGBA', (size, size), (12, 8, 6, 255))
    out.alpha_composite(pic, (1, 1))
    d = ImageDraw.Draw(out)
    d.rectangle([0, 0, size - 1, size - 1], outline=T.rgb(T.GOLD_DIM if got else T.LINE) + (255,), width=1)
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
        sb = tk.Frame(right, bg=T.LINE, padx=1, pady=1)
        sb.pack(side='top', anchor='e', fill='x', padx=4)
        inner = tk.Frame(sb, bg='#120d0b')
        inner.pack(fill='x')
        self._mag = ImageTk.PhotoImage(T.magnifier(16))
        tk.Label(inner, image=self._mag, bg='#120d0b').pack(side='left', padx=(10, 2))
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
        holder = tk.Frame(body, bg=T.LINE, padx=1, pady=1)
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
            self.head.config(image=self._photo('head', T.banner(w, 170, 'Troféus', 'GOD OF WAR COLLECTION', self.art)))
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
        """Cartão do jogo: superfície lisa, número grande em marfim, linha de progresso; o ativo ganha a barra
        vermelha à esquerda."""
        tr, got = self.sets[comm], self.got[comm]
        pct = progress(tr, got)
        on = comm == self.game
        img = Image.new('RGBA', (CARD_W, CARD_H), (SURF_SEL if on else SURF) + (255,))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, CARD_W - 1, CARD_H - 1], outline=T.rgb(T.LINE) + (255,))
        if on:
            d.rectangle([0, 0, 2, CARD_H - 1], fill=T.rgb(T.RED_HI) + (255,))
        x = 22
        name = T.plain_text(dict(GAMES)[comm].upper(), 12, T.GOLD if on else T.DIM, kind='bold', spacing=0.18,
                            shadow=False)
        img.alpha_composite(name, (x - 3, 12))
        num = T.plain_text('%d%%' % int(pct), 32, T.TEXT, kind='title', shadow=False)
        img.alpha_composite(num, (x - 3, 30))
        n = sum(1 for t in tr if t['id'] in got)
        d.text((x + num.width + 6, 50), '%d de %d troféus' % (n, len(tr)), font=T.font(13, 'text'),
               fill=T.rgb(T.DIM) + (255,))
        img.alpha_composite(T.line_bar(CARD_W - 2 * x, pct / 100), (x, CARD_H - 14))
        return img

    def _paint_filters(self, hover=None):
        """Filtros como abas de texto: o ativo em marfim com sublinhado vermelho."""
        for val, (b, label) in self.fbtns.items():
            on, hv = val == self.filter, val == hover
            w = 108
            img = Image.new('RGBA', (w, 34), T.rgb(T.BG) + (255,))
            t = T.plain_text(label.upper(), 12, T.TEXT if on else T.GOLD if hv else T.DIM, kind='bold',
                             spacing=0.16, shadow=False)
            img.alpha_composite(t, ((w - t.width) // 2, 6))
            if on:
                ImageDraw.Draw(img).rectangle([18, 30, w - 19, 31], fill=T.rgb(T.RED_HI) + (255,))
            b.config(image=self._photo(('f', val), img))

    def _paint_summary(self):
        tr, got = self.sets[self.game], self.got[self.game]
        w = max(600, self.winfo_width() - 36)
        img = Image.new('RGBA', (w, 28), T.rgb(T.BG) + (255,))
        d = ImageDraw.Draw(img)
        x = 0
        f = T.font(13, 'text')
        for ty in 'PGSB':
            tot = [t for t in tr if t['type'] == ty]
            if not tot:
                continue
            img.alpha_composite(T.dot(10, ty), (x, 9))
            txt = '%s  %d/%d' % (TYPE_NAME[ty], sum(1 for t in tot if t['id'] in got), len(tot))
            d.text((x + 16, 5), txt, font=f, fill=T.rgb(T.DIM) + (255,))
            x += 16 + int(f.getlength(txt)) + 26
        tail = '%d na lista' % len(self.rows)
        d.text((w - int(f.getlength(tail)) - 4, 5), tail, font=f, fill=T.rgb(T.DIM) + (255,))
        self.summary.config(image=self._photo('summary', img))

    def _row(self, t, idx, w):
        has = t['id'] in self.got[self.game]
        sel, hov = t['id'] == self.sel, idx == self.hover
        base = Image.new('RGBA', (w, ROW_H), (SURF_SEL if sel else SURF_HI if hov else SURF) + (255,))
        d = ImageDraw.Draw(base)
        d.line([(16, ROW_H - 1), (w - 16, ROW_H - 1)], fill=T.rgb(T.LINE) + (255,))
        if sel:
            d.rectangle([0, 0, 2, ROW_H - 1], fill=T.rgb(T.RED_HI) + (255,))
        base.alpha_composite(framed_icon(t['icon'], 52, has), (20, (ROW_H - 52) // 2))
        right = 132
        fname = T.font(17, 'title')
        d.text((88, 14), ellipsize(t['name'], fname, w - 92 - right), font=fname,
               fill=T.rgb(T.TEXT if has else T.DIM) + (255,))
        fdesc = T.font(13, 'text')
        d.text((88, 42), ellipsize(t['desc'], fdesc, w - 92 - right), font=fdesc,
               fill=(T.rgb(T.DIM) if has else (120, 110, 98)) + (255,))
        base.alpha_composite(T.dot(10, t['type']), (w - right, ROW_H // 2 - 13))
        d.text((w - right + 16, ROW_H // 2 - 18), TYPE_NAME[t['type']].upper(), font=T.font(12, 'bold'),
               fill=T.rgb(T.TEXT if has else T.DIM) + (255,))
        d.text((w - right + 16, ROW_H // 2 + 2), 'OBTIDO' if has else 'BLOQUEADO', font=T.font(11, 'text'),
               fill=(T.rgb(T.GOLD) if has else (110, 100, 90)) + (255,))
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
        """Painel de detalhe: ícone, tipo e estado ao lado, nome em serifa marfim, e as seções com rótulo pequeno
        em caixa alta; texto corrido a 15 px com entrelinha 1,5."""
        h = max(420, self.list.winfo_height() + 2)
        img = Image.new('RGBA', (DETAIL_W, h), SURF + (255,))
        d = ImageDraw.Draw(img)
        d.rectangle([0, 0, DETAIL_W - 1, h - 1], outline=T.rgb(T.LINE) + (255,))
        t = next((x for x in self.sets.get(self.game, []) if x['id'] == self.sel), None) if self.game else None
        if t is None:
            self.detail_img = img
            self.detail.config(image=self._photo('detail', img))
            return
        has = t['id'] in self.got[self.game]
        pad, size = 32, 132
        img.alpha_composite(framed_icon(t['icon'], size, has), (pad, pad))
        tx = pad + size + 20
        img.alpha_composite(T.dot(10, t['type']), (tx, pad + 6))
        d.text((tx + 16, pad + 1), TYPE_NAME[t['type']].upper(), font=T.font(12, 'bold'), fill=T.rgb(T.TEXT) + (255,))
        d.text((tx, pad + 26), 'Obtido' if has else 'Ainda não obtido', font=T.font(13, 'text'),
               fill=T.rgb(T.GOLD if has else T.DIM) + (255,))
        if t['hidden']:
            d.text((tx, pad + 48), 'Troféu oculto', font=T.font(12, 'italic'), fill=T.rgb(T.DIM) + (255,))
        y = pad + size + 24
        fname = T.font(26, 'title')
        for line in T.wrap(t['name'], fname, DETAIL_W - 2 * pad)[:3]:
            d.text((pad, y), line, font=fname, fill=T.rgb(T.TEXT) + (255,))
            y += 34
        y += 14
        d.line([(pad, y), (DETAIL_W - pad, y)], fill=T.rgb(T.LINE) + (255,))
        y += 20
        ftext, fen = T.font(15, 'serif'), T.font(13, 'italic')

        def para(body, f, color, y):
            for line in T.wrap(body, f, DETAIL_W - 2 * pad):
                if y > h - 28:
                    break
                d.text((pad, y), line, font=f, fill=color)
                y += int(f.size * 1.5)
            return y

        def label(text, y):
            d.text((pad, y), text, font=T.font(11, 'bold'), fill=T.rgb(T.GOLD_DIM) + (255,))
            return y + 22

        y = label('O QUE PEDE', y)
        y = para(t['desc'], ftext, T.rgb(T.TEXT) + (255,), y)
        if t['desc'] != t['detail']:
            y = para(t['detail'], fen, T.rgb(T.DIM) + (255,), y + 4)
        if t['tip'] and y < h - 60:
            y = label('COMO PEGAR', y + 18)
            para(t['tip'], ftext, T.rgb(T.TEXT) + (255,), y)
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
