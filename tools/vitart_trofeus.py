"""vitart Troféus: progresso e troféus do God of War Collection no vitart.

    python tools/vitart_trofeus.py

Mostra, para cada jogo, quanto foi feito (porcentagem como a do PlayStation, pelos pontos de cada troféu:
bronze 15, prata 30, ouro 90, platina 180), quais troféus foram pegos e quais faltam, com a descrição e a
dica de como pegar (tools/trofeus_guia.json). A lista vem do TROP.SFM de dentro do TROPHY.TRP do jogo; os
desbloqueados, do <vitart-data>/trophy/<NPWR>/unlocked.txt que o vitart grava. A janela se atualiza
sozinha enquanto o jogo roda.
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

from PIL import Image, ImageEnhance, ImageOps, ImageTk  # noqa: E402

GAMES = [('NPWR05244_00', 'God of War'), ('NPWR05283_00', 'God of War II')]
POINTS = {'B': 15, 'S': 30, 'G': 90, 'P': 180}
TYPE_NAME = {'P': 'Platina', 'G': 'Ouro', 'S': 'Prata', 'B': 'Bronze'}
TYPE_COLOR = {'P': '#b8c6d8', 'G': '#e0b84a', 'S': '#c4c4c4', 'B': '#c07a46'}
GUIDE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'trofeus_guia.json')

BG, PANEL, FG, DIM, RED, GOLD = '#141010', '#1f1817', '#ece2d0', '#9a8f80', '#a5231b', '#d4a64a'


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


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('vitart Troféus')
        self.geometry('1180x760')
        self.minsize(900, 560)
        self.configure(bg=BG)
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
        self.images = {}  # mantém as PhotoImage vivas
        # abre no jogo com o troféu mais recente (o unlocked.txt gravado por último)
        recent = sorted(self.sets, key=lambda c: os.path.getmtime(unlocked_path(self.cfg, c))
                        if os.path.exists(unlocked_path(self.cfg, c)) else 0)
        self.game = recent[-1] if recent else None
        self.filter = tk.StringVar(value='todos')
        self.search = tk.StringVar()
        self._style()
        self._build()
        self.refresh()
        self.after(2000, self.poll)

    # ------------------------------------------------------------------ aparência
    def _style(self):
        st = ttk.Style(self)
        st.theme_use('clam')
        st.configure('.', background=BG, foreground=FG, fieldbackground=PANEL, font=('Segoe UI', 10))
        st.configure('Treeview', background=PANEL, fieldbackground=PANEL, foreground=FG, rowheight=56, borderwidth=0)
        st.map('Treeview', background=[('selected', '#4a1714')], foreground=[('selected', '#fff3dc')])
        st.configure('Treeview.Heading', background='#2a201e', foreground=GOLD, font=('Segoe UI', 10, 'bold'))
        st.configure('red.Horizontal.TProgressbar', troughcolor='#2a201e', background=RED, bordercolor=BG,
                     lightcolor=RED, darkcolor='#6e140f')
        st.configure('TRadiobutton', background=BG, foreground=FG)
        st.map('TRadiobutton', background=[('active', BG)])
        st.configure('TEntry', fieldbackground=PANEL, foreground=FG, insertcolor=FG)

    def _build(self):
        top = tk.Frame(self, bg=BG)
        top.pack(fill='x', padx=14, pady=(12, 6))
        self.game_btns = {}
        for comm, title in GAMES:
            if comm not in self.sets:
                continue
            card = tk.Frame(top, bg=PANEL, cursor='hand2', highlightthickness=2, highlightbackground=PANEL)
            card.pack(side='left', fill='x', expand=True, padx=(0, 10))
            name = tk.Label(card, text=title, bg=PANEL, fg=FG, font=('Georgia', 15, 'bold'), anchor='w')
            name.pack(fill='x', padx=12, pady=(8, 0))
            pct = tk.Label(card, bg=PANEL, fg=GOLD, font=('Segoe UI', 11), anchor='w')
            pct.pack(fill='x', padx=12)
            bar = ttk.Progressbar(card, style='red.Horizontal.TProgressbar', maximum=100)
            bar.pack(fill='x', padx=12, pady=(4, 10))
            for w in (card, name, pct):
                w.bind('<Button-1>', lambda _e, c=comm: self.select_game(c))
            self.game_btns[comm] = (card, pct, bar)

        bar = tk.Frame(self, bg=BG)
        bar.pack(fill='x', padx=14, pady=4)
        self.summary = tk.Label(bar, bg=BG, fg=DIM, font=('Segoe UI', 10), anchor='w')
        self.summary.pack(side='left')
        ent = ttk.Entry(bar, textvariable=self.search, width=26)
        ent.pack(side='right')
        tk.Label(bar, text='Buscar', bg=BG, fg=DIM).pack(side='right', padx=6)
        for val, label in (('faltando', 'Faltando'), ('obtidos', 'Obtidos'), ('todos', 'Todos')):
            ttk.Radiobutton(bar, text=label, value=val, variable=self.filter, command=self.fill).pack(side='right', padx=6)
        self.search.trace_add('write', lambda *_: self.fill())

        body = tk.PanedWindow(self, orient='horizontal', bg=BG, sashwidth=6, bd=0)
        body.pack(fill='both', expand=True, padx=14, pady=(4, 12))
        left = tk.Frame(body, bg=BG)
        self.tree = ttk.Treeview(left, columns=('tipo', 'estado'), show='tree headings', selectmode='browse')
        self.tree.heading('#0', text='Troféu')
        self.tree.heading('tipo', text='Tipo')
        self.tree.heading('estado', text='Estado')
        self.tree.column('#0', width=420)
        self.tree.column('tipo', width=80, anchor='center')
        self.tree.column('estado', width=90, anchor='center')
        sb = ttk.Scrollbar(left, command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')
        self.tree.bind('<<TreeviewSelect>>', lambda _e: self.show())
        self.tree.tag_configure('falta', foreground=DIM)
        body.add(left, minsize=480)

        right = tk.Frame(body, bg=PANEL)
        self.big = tk.Label(right, bg=PANEL)
        self.big.pack(pady=(18, 6))
        self.t_name = tk.Label(right, bg=PANEL, fg=FG, font=('Georgia', 16, 'bold'), wraplength=380, justify='center')
        self.t_name.pack(padx=16)
        self.t_meta = tk.Label(right, bg=PANEL, fg=DIM, font=('Segoe UI', 10))
        self.t_meta.pack(pady=(2, 10))
        self.t_text = tk.Text(right, bg=PANEL, fg=FG, relief='flat', wrap='word', font=('Segoe UI', 11), height=14,
                              padx=16, pady=4, highlightthickness=0, cursor='arrow')
        self.t_text.tag_configure('h', foreground=GOLD, font=('Segoe UI', 11, 'bold'), spacing1=8, spacing3=4)
        self.t_text.tag_configure('en', foreground=DIM, font=('Segoe UI', 9, 'italic'))
        self.t_text.pack(fill='both', expand=True, padx=4, pady=(0, 12))
        body.add(right, minsize=320)

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
        self.game = comm
        self.refresh()

    def icon(self, t, size, got):
        key = (self.game, t['id'], size, got)
        if key not in self.images:
            img = t['icon'] or Image.new('RGBA', (size, size), '#333')
            img = img.resize((size, size), Image.LANCZOS)
            self.images[key] = ImageTk.PhotoImage(img if got else gray(img))
        return self.images[key]

    def refresh(self, keep=False):
        for comm, (card, pct, bar) in self.game_btns.items():
            tr, got = self.sets[comm], self.got[comm]
            p = progress(tr, got)
            n = sum(1 for t in tr if t['id'] in got)
            pct.config(text='%d%%   ·   %d de %d troféus' % (int(p), n, len(tr)))
            bar['value'] = p
            card.config(highlightbackground=RED if comm == self.game else PANEL)
        self.fill(keep)

    def fill(self, keep=False):
        if not self.game:
            return
        sel = self.tree.selection()
        tr, got = self.sets[self.game], self.got[self.game]
        parts = []
        for ty in 'PGSB':
            tot = [t for t in tr if t['type'] == ty]
            if tot:
                parts.append('%s %d/%d' % (TYPE_NAME[ty], sum(1 for t in tot if t['id'] in got), len(tot)))
        self.summary.config(text='   '.join(parts))
        self.tree.delete(*self.tree.get_children())
        q = self.search.get().strip().lower()
        f = self.filter.get()
        for t in tr:
            has = t['id'] in got
            if (f == 'obtidos' and not has) or (f == 'faltando' and has):
                continue
            if q and q not in (t['name'] + ' ' + t['desc'] + ' ' + t['detail']).lower():
                continue
            self.tree.insert('', 'end', iid=str(t['id']), text='  ' + t['name'], image=self.icon(t, 48, has),
                             values=(TYPE_NAME[t['type']], 'Obtido' if has else 'Falta'),
                             tags=() if has else ('falta',))
        kids = self.tree.get_children()
        if keep and sel and sel[0] in kids:
            self.tree.selection_set(sel[0])
        elif kids:
            self.tree.selection_set(kids[0])
        else:
            self.show()

    def show(self):
        sel = self.tree.selection()
        self.t_text.config(state='normal')
        self.t_text.delete('1.0', 'end')
        if not sel:
            self.big.config(image='')
            self.t_name.config(text='')
            self.t_meta.config(text='')
            self.t_text.config(state='disabled')
            return
        t = next(x for x in self.sets[self.game] if x['id'] == int(sel[0]))
        has = t['id'] in self.got[self.game]
        self.big.config(image=self.icon(t, 160, has))
        self.t_name.config(text=t['name'])
        self.t_meta.config(text='%s  ·  %s' % (TYPE_NAME[t['type']], 'Obtido' if has else 'Ainda não'),
                           fg=TYPE_COLOR[t['type']] if has else DIM)
        self.t_text.insert('end', 'O que pede\n', 'h')
        self.t_text.insert('end', t['desc'] + '\n')
        if t['desc'] != t['detail']:
            self.t_text.insert('end', t['detail'] + '\n', 'en')
        if t['tip']:
            self.t_text.insert('end', 'Como pegar\n', 'h')
            self.t_text.insert('end', t['tip'] + '\n')
        self.t_text.config(state='disabled')


def main():
    app = App()
    if app.errors:
        from tkinter import messagebox
        messagebox.showwarning('vitart Troféus', '\n'.join(app.errors))
    app.mainloop()


if __name__ == '__main__':
    main()
