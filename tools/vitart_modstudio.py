"""vitart Mod Studio: interface gráfica do kit de mods do vitart (docs/modding/README.md).

    python tools/vitart_modstudio.py

Abas:
  Mods            os mods instalados: ligar/desligar, ordem, remover (vai para mods_removidos), reconstruir
  Converter PS2   mod de PS2 em xdelta (ou .zip com o xdelta) + ISO original -> mod do vitart, instalado
  Texturas        texturas dos WADs do jogo: ver, exportar PNG, trocar por PNG e salvar como mod novo
  Textos          tradução: exporta os textos do jogo (MSGS_TXT) e instala um .txt traduzido como mod
  Configurações   pastas do jogo, do vitart-data, xdelta3 e 7-Zip
  Ajuda           o guia de modding
As operações demoradas rodam numa thread; o registro mostra a saída das ferramentas.
"""
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vitart_modkit as K  # noqa: E402
import vitart_tema as T  # noqa: E402

DOCS = os.path.join(K.REPO, 'docs', 'modding', 'README.md')


class Log(ttk.Frame):
    """Caixa de registro alimentada por uma fila (as threads de trabalho só escrevem na fila)."""

    def __init__(self, master, height=10):
        super().__init__(master)
        self.text = tk.Text(self, height=height, wrap='word', state='disabled', font=('Consolas', 9))
        sb = ttk.Scrollbar(self, command=self.text.yview)
        self.text['yscrollcommand'] = sb.set
        self.text.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')
        self.q = queue.Queue()
        self.after(100, self._poll)

    def write(self, line):
        self.q.put(str(line))

    def _poll(self):
        lines = []
        while not self.q.empty():
            lines.append(self.q.get())
        if lines:
            self.text['state'] = 'normal'
            self.text.insert('end', '\n'.join(lines) + '\n')
            self.text.see('end')
            self.text['state'] = 'disabled'
        self.after(100, self._poll)


class Studio(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('vitart Mod Studio')
        self.geometry('1180x820')
        self.minsize(960, 680)
        self.cfg = K.load_config()
        self.busy = False
        T.style_ttk(self)
        T.set_icon(self)
        # cabeçalho com a arte do jogo e o friso grego no pé (redesenhados quando a largura muda)
        self.art = T.game_art(self.cfg.get('app'))
        self.head = tk.Label(self, bg=T.BG, bd=0)
        self.head.pack(fill='x')
        self.foot = tk.Label(self, bg=T.BG, bd=0)
        self.foot.pack(fill='x', side='bottom')
        self._art_w = None
        self.bind('<Configure>', lambda e: self.after_idle(self._paint_chrome) if e.widget is self else None)
        nb = ttk.Notebook(self)
        nb.pack(fill='both', expand=True, padx=12, pady=(10, 8))
        self.tab_mods = ttk.Frame(nb)
        self.tab_conv = ttk.Frame(nb)
        self.tab_tex = ttk.Frame(nb)
        self.tab_txt = ttk.Frame(nb)
        self.tab_cfg = ttk.Frame(nb)
        self.tab_help = ttk.Frame(nb)
        for t, label in ((self.tab_mods, 'Mods'), (self.tab_conv, 'Converter mod do PS2'), (self.tab_tex, 'Texturas'),
                         (self.tab_txt, 'Textos / Tradução'),
                         (self.tab_cfg, 'Configurações'), (self.tab_help, 'Ajuda')):
            nb.add(t, text=label)
        self.build_mods()
        self.build_conv()
        self.build_tex()
        self.build_txt()
        self.build_cfg()
        self.build_help()
        self.refresh_mods()

    def _paint_chrome(self):
        from PIL import ImageTk
        w = self.winfo_width()
        if w < 200 or w == self._art_w:
            return
        self._art_w = w
        self._head_ph = ImageTk.PhotoImage(T.banner(w, 150, 'MOD STUDIO', 'VITART  ·  GOD OF WAR COLLECTION', self.art))
        self._foot_ph = ImageTk.PhotoImage(T.meander(w, 16))
        self.head.config(image=self._head_ph)
        self.foot.config(image=self._foot_ph)

    # -------------------------------------------------------------- trabalho em segundo plano
    def work(self, log, fn, done=None):
        if self.busy:
            messagebox.showinfo('vitart Mod Studio', 'Espere a operação atual terminar.')
            return
        self.busy = True
        self.config(cursor='watch')

        def body():
            ok = True
            try:
                fn()
            except Exception as e:  # noqa: BLE001 - mostra qualquer falha no registro
                ok = False
                log.write('ERRO: %s' % e)
            finally:
                self.busy = False
                self.after(0, lambda: (self.config(cursor=''), done and done(ok)))
        threading.Thread(target=body, daemon=True).start()

    # -------------------------------------------------------------- aba Mods
    def build_mods(self):
        f = self.tab_mods
        top = ttk.Frame(f)
        top.pack(fill='both', expand=True, padx=8, pady=8)
        cols = ('on', 'name', 'game', 'author', 'version', 'files', 'size')
        self.tree = ttk.Treeview(top, columns=cols, show='headings', selectmode='browse', height=12)
        for c, label, w in (('on', 'Ligado', 60), ('name', 'Nome', 260), ('game', 'Jogo', 60), ('author', 'Autor', 240),
                            ('version', 'Versão', 70), ('files', 'Arquivos', 70), ('size', 'Tamanho', 90)):
            self.tree.heading(c, text=label)
            self.tree.column(c, width=w, anchor='w' if c in ('name', 'author') else 'center')
        self.tree.pack(side='left', fill='both', expand=True)
        self.tree.bind('<<TreeviewSelect>>', lambda e: self.show_mod())
        self.tree.bind('<Double-1>', lambda e: self.toggle_mod())
        btns = ttk.Frame(top)
        btns.pack(side='left', fill='y', padx=8)
        for label, cmd in (('Ligar / desligar', self.toggle_mod), ('Subir', lambda: self.move_mod(-1)),
                           ('Descer', lambda: self.move_mod(1)), ('Abrir pasta', self.open_mod),
                           ('Remover', self.remove_mod), ('Reconstruir menu e patches', self.rebuild),
                           ('Atualizar lista', self.refresh_mods)):
            ttk.Button(btns, text=label, command=cmd, width=26).pack(pady=3)
        ttk.Label(btns, text='Ligar/desligar aqui vale ao abrir\no jogo. No menu de pausa\n(Options > Mods > Apply) vale na hora.',
                  foreground=T.DIM).pack(pady=10)
        self.mod_info = tk.Text(f, height=5, wrap='word', state='disabled')
        self.mod_info.pack(fill='x', padx=8)
        self.mods_log = Log(f, height=9)
        self.mods_log.pack(fill='both', expand=False, padx=8, pady=8)

    def refresh_mods(self):
        self.tree.delete(*self.tree.get_children())
        self.mods = K.list_mods(self.cfg)
        for m in self.mods:
            self.tree.insert('', 'end', iid=m.id, values=('Sim' if m.enabled else 'Não', m.name, m.game.upper(), m.author,
                                                          m.version, len(m.files()) + len(m.files('src')),
                                                          '%.1f MB' % (m.size() / 1e6)))

    def selected(self):
        sel = self.tree.selection()
        return next((m for m in self.mods if m.id == sel[0]), None) if sel else None

    def show_mod(self):
        m = self.selected()
        self.mod_info['state'] = 'normal'
        self.mod_info.delete('1.0', 'end')
        if m:
            self.mod_info.insert('end', '%s (%s)\n%s\nPasta: %s\nArquivos: %s' % (
                m.name, m.id, m.description, m.dir, ', '.join((m.files() + m.files('src'))[:12]) +
                (' ...' if len(m.files()) > 12 else '')))
        self.mod_info['state'] = 'disabled'

    def toggle_mod(self):
        m = self.selected()
        if not m:
            return
        K.set_enabled(self.cfg, m.id, not m.enabled)
        self.mods_log.write('%s %s (vale ao abrir o jogo)' % (m.id, 'desligado' if m.enabled else 'ligado'))
        self.refresh_mods()
        self.tree.selection_set(m.id)

    def move_mod(self, d):
        m = self.selected()
        if not m:
            return
        ids = [x.id for x in self.mods]
        i = ids.index(m.id)
        j = max(0, min(len(ids) - 1, i + d))
        ids[i], ids[j] = ids[j], ids[i]
        K.write_modlist(self.cfg, [next(x for x in self.mods if x.id == k) for k in ids])
        self.refresh_mods()
        self.tree.selection_set(m.id)
        self.mods_log.write('ordem mudada: reconstrua para o menu do jogo mostrar a ordem nova')

    def open_mod(self):
        m = self.selected()
        os.startfile(m.dir if m else K.mods_dir(self.cfg))

    def remove_mod(self):
        m = self.selected()
        if not m or not messagebox.askyesno('Remover mod', 'Remover "%s"?\n\nA pasta vai para vitart-data/mods_removidos '
                                            '(nada é apagado).' % m.name):
            return
        dst = os.path.join(self.cfg['data'], 'mods_removidos', '%s_%s' % (m.id, time.strftime('%Y%m%d_%H%M%S')))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.move(m.dir, dst)
        K.set_enabled(self.cfg, m.id, False)
        self.mods_log.write('%s movido para %s' % (m.id, dst))
        self.refresh_mods()
        self.rebuild()

    def rebuild(self):
        self.work(self.mods_log, lambda: K.rebuild(self.cfg, self.mods_log.write), lambda ok: self.refresh_mods())

    # -------------------------------------------------------------- aba Converter PS2
    def build_conv(self):
        f = self.tab_conv
        ttk.Label(f, text='Converte um mod do PS2 distribuído como patch XDelta (solto ou dentro de um .zip) para o '
                          'formato do vitart e instala.\nPrecisa da ISO original do PS2 (ou do .7z com ela) e do '
                          'xdelta3 (Configurações).', justify='left').pack(anchor='w', padx=10, pady=8)
        g = ttk.Frame(f)
        g.pack(fill='x', padx=10)
        self.conv = {k: tk.StringVar() for k in ('game', 'patch', 'iso', 'id', 'name', 'author', 'desc')}
        self.conv['game'].set('gow2')
        rows = [('Jogo', 'game', None), ('Patch do mod (.xdelta / .zip)', 'patch', ('Patch', '*.xdelta *.xd3 *.vcdiff *.zip')),
                ('ISO original do PS2 (.iso / .7z)', 'iso', ('ISO', '*.iso *.7z')), ('Id do mod (pasta)', 'id', None),
                ('Nome (aparece no menu)', 'name', None), ('Autor(es)', 'author', None), ('Descrição', 'desc', None)]
        for r, (label, key, pick) in enumerate(rows):
            ttk.Label(g, text=label).grid(row=r, column=0, sticky='w', pady=3)
            if key == 'game':
                ttk.Combobox(g, textvariable=self.conv[key], values=['gow2'], state='readonly', width=10).grid(
                    row=r, column=1, sticky='w')
                ttk.Label(g, text='(GoW1: ainda não há conversor de malha do PS2 do GoW1)', foreground=T.DIM).grid(
                    row=r, column=2, sticky='w')
                continue
            ttk.Entry(g, textvariable=self.conv[key], width=80).grid(row=r, column=1, sticky='we', padx=4)
            if pick:
                ttk.Button(g, text='Escolher...', command=lambda k=key, p=pick: self.pick(self.conv[k], p)).grid(
                    row=r, column=2, sticky='w')
        g.columnconfigure(1, weight=1)
        self.keep_work = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text='Manter a pasta de trabalho (arquivos do PS2 extraídos)', variable=self.keep_work).pack(
            anchor='w', padx=10, pady=4)
        ttk.Button(f, text='Converter e instalar', command=self.convert).pack(anchor='w', padx=10, pady=4)
        self.conv_bar = ttk.Progressbar(f, mode='indeterminate')
        self.conv_bar.pack(fill='x', padx=10, pady=4)
        self.conv_log = Log(f, height=18)
        self.conv_log.pack(fill='both', expand=True, padx=10, pady=8)

    def pick(self, var, pattern):
        p = filedialog.askopenfilename(filetypes=[pattern, ('Todos', '*.*')])
        if p:
            var.set(p)
            if var is self.conv['patch'] and not self.conv['name'].get():
                base = os.path.splitext(os.path.basename(p))[0]
                self.conv['name'].set(base[:24])
                self.conv['id'].set(K.safe_id(base)[:32])

    def convert(self):
        c = {k: v.get().strip() for k, v in self.conv.items()}
        if not (c['patch'] and c['iso'] and c['name']):
            messagebox.showwarning('Converter', 'Escolha o patch, a ISO e dê um nome ao mod.')
            return
        mid = c['id'] or K.safe_id(c['name'])
        self.conv_bar.start(12)
        self.work(self.conv_log,
                  lambda: K.convert_ps2(self.cfg, c['game'], c['patch'], c['iso'], mid, c['name'], author=c['author'],
                                        description=c['desc'], log=self.conv_log.write, keep_work=self.keep_work.get()),
                  lambda ok: (self.conv_bar.stop(), self.refresh_mods(),
                              ok and messagebox.showinfo('Converter', 'Mod "%s" instalado. Ligue na aba Mods ou no menu '
                                                                      'de pausa (Options > Mods).' % c['name'])))

    # -------------------------------------------------------------- aba Textos
    def build_txt(self):
        f = self.tab_txt
        ttk.Label(f, text='Tradução como mod: exporte os textos do jogo (MSGS_TXT, os do modo PS TV que o vitart mostra), '
                          'traduza o .txt mantendo as linhas *N* e as marcas [XButton] etc., e instale.\n'
                          'O arquivo é UTF-8. A fonte do jogo não tem maiúsculas acentuadas (Á, É, Ú... viram ícones de '
                          'botão) nem ã/õ: o kit troca por A, E, U e ä/ö. Vale ao abrir o jogo.',
                  justify='left').pack(anchor='w', padx=10, pady=8)
        g = ttk.Frame(f)
        g.pack(fill='x', padx=10)
        self.txt = {k: tk.StringVar() for k in ('game', 'file', 'id', 'name', 'author')}
        self.txt['game'].set('gow2')
        self.txt['name'].set('Tradução')
        rows = [('Jogo', 'game'), ('Arquivo .txt traduzido', 'file'), ('Id do mod (pasta)', 'id'),
                ('Nome (aparece no menu)', 'name'), ('Autor(es)', 'author')]
        for r, (label, key) in enumerate(rows):
            ttk.Label(g, text=label).grid(row=r, column=0, sticky='w', pady=3)
            if key == 'game':
                ttk.Combobox(g, textvariable=self.txt[key], values=['gow2', 'gow1'], state='readonly', width=10).grid(
                    row=r, column=1, sticky='w')
                continue
            ttk.Entry(g, textvariable=self.txt[key], width=80).grid(row=r, column=1, sticky='we', padx=4)
            if key == 'file':
                ttk.Button(g, text='Escolher...', command=lambda: self.pick(self.txt['file'], ('Texto', '*.txt'))).grid(
                    row=r, column=2, sticky='w')
        g.columnconfigure(1, weight=1)
        b = ttk.Frame(f)
        b.pack(anchor='w', padx=10, pady=6)
        ttk.Button(b, text='Exportar os textos originais...', command=self.export_txt).pack(side='left')
        ttk.Button(b, text='Instalar tradução', command=self.install_txt).pack(side='left', padx=6)
        self.txt_log = Log(f, height=18)
        self.txt_log.pack(fill='both', expand=True, padx=10, pady=8)

    def export_txt(self):
        game = self.txt['game'].get()
        out = filedialog.asksaveasfilename(defaultextension='.txt', initialfile='%s_MSGS_TXT.txt' % game,
                                           filetypes=[('Texto', '*.txt')])
        if not out:
            return

        def go():
            recs = K.wad_records(K.read_psarc_file(K.psarc_path(self.cfg, game), K.TEXT_WAD), game)
            body = next(r[3] for r in recs if r[2] == 'MSGS_TXT').split(b'\0')[0]
            open(out, 'wb').write(body)
            self.txt_log.write('textos de %s (%s) salvos em %s' % (game, K.TEXT_WAD, out))
        self.work(self.txt_log, go)

    def install_txt(self):
        c = {k: v.get().strip() for k, v in self.txt.items()}
        if not (c['file'] and c['name']):
            messagebox.showwarning('Textos', 'Escolha o .txt traduzido e dê um nome ao mod.')
            return
        mid = c['id'] or K.safe_id(c['name'])
        self.work(self.txt_log,
                  lambda: K.make_text_mod(self.cfg, mid, c['name'], c['game'], c['file'], author=c['author'],
                                          log=self.txt_log.write),
                  lambda ok: (self.refresh_mods(),
                              ok and messagebox.showinfo('Textos', 'Mod "%s" instalado. Ligue na aba Mods ou no menu de '
                                                                   'pausa (Options > Mods) e abra o jogo de novo.'
                                                         % c['name'])))

    # -------------------------------------------------------------- aba Texturas
    def build_tex(self):
        f = self.tab_tex
        top = ttk.Frame(f)
        top.pack(fill='x', padx=8, pady=6)
        ttk.Label(top, text='Jogo').pack(side='left')
        self.tex_game = tk.StringVar(value='gow2')
        cb = ttk.Combobox(top, textvariable=self.tex_game, values=['gow1', 'gow2'], state='readonly', width=8)
        cb.pack(side='left', padx=4)
        cb.bind('<<ComboboxSelected>>', lambda e: self.load_wads())
        ttk.Label(top, text='Filtro').pack(side='left', padx=(12, 0))
        self.tex_filter = tk.StringVar()
        e = ttk.Entry(top, textvariable=self.tex_filter, width=24)
        e.pack(side='left', padx=4)
        e.bind('<KeyRelease>', lambda ev: self.load_wads())
        ttk.Button(top, text='Carregar lista de WADs', command=self.load_wads).pack(side='left', padx=8)
        mid = ttk.Frame(f)
        mid.pack(fill='both', expand=True, padx=8)
        self.wad_list = tk.Listbox(mid, width=38, exportselection=False)
        self.wad_list.pack(side='left', fill='y')
        self.wad_list.bind('<<ListboxSelect>>', lambda e: self.load_textures())
        self.tex_tree = ttk.Treeview(mid, columns=('name', 'size', 'fmt', 'new'), show='headings', selectmode='browse')
        for c, label, w in (('name', 'Textura (registro)', 230), ('size', 'Tamanho', 80), ('fmt', 'Formato', 70),
                            ('new', 'Troca', 160)):
            self.tex_tree.heading(c, text=label)
            self.tex_tree.column(c, width=w)
        self.tex_tree.pack(side='left', fill='both', expand=True, padx=6)
        self.tex_tree.bind('<<TreeviewSelect>>', lambda e: self.preview())
        right = ttk.Frame(mid)
        right.pack(side='left', fill='y')
        self.canvas = tk.Label(right, text='(prévia)', width=40, relief='flat', bg='#0f0b09', fg=T.DIM,
                               highlightthickness=1, highlightbackground=T.GOLD_DIM)
        self.canvas.pack(pady=4)
        for label, cmd in (('Exportar PNG...', self.export_png), ('Trocar por PNG...', self.import_png),
                           ('Desfazer troca', self.undo_png), ('Exportar todas do WAD...', self.export_all)):
            ttk.Button(right, text=label, command=cmd, width=28).pack(pady=2)
        box = ttk.LabelFrame(right, text='Salvar as trocas como mod')
        box.pack(fill='x', pady=10)
        self.tex_mod = {k: tk.StringVar() for k in ('name', 'author')}
        for label, k in (('Nome', 'name'), ('Autor', 'author')):
            ttk.Label(box, text=label).pack(anchor='w')
            ttk.Entry(box, textvariable=self.tex_mod[k], width=30).pack(anchor='w', padx=4)
        ttk.Button(box, text='Criar mod', command=self.save_tex_mod).pack(pady=6)
        self.tex_log = Log(f, height=6)
        self.tex_log.pack(fill='x', padx=8, pady=6)
        self.changes = {}  # (wad, registro) -> (png, rgba)
        self.cur_wad = None
        self._img = None

    def load_wads(self):
        game = self.tex_game.get()
        ps = K.psarc_path(self.cfg, game)
        if not os.path.exists(ps):
            self.tex_log.write('psarc não encontrado: %s (Configurações)' % ps)
            return
        flt = self.tex_filter.get().lower()
        names = [n for n in K.manifest(ps) if n.endswith('.wad_psp2') and '_VitaTV' not in n and flt in n.lower()]
        self.wad_list.delete(0, 'end')
        for n in names:
            self.wad_list.insert('end', n)

    def load_textures(self):
        sel = self.wad_list.curselection()
        if not sel:
            return
        game = self.tex_game.get()
        self.cur_wad = self.wad_list.get(sel[0])
        tv = self.cur_wad.replace('wad/', 'wad/_VitaTV/', 1)
        ps = K.psarc_path(self.cfg, game)
        src = tv if tv in K.manifest(ps) else self.cur_wad  # a versão do PS TV é a que o vitart carrega
        self.cur_body = K.read_psarc_file(ps, src)
        self.tex_tree.delete(*self.tex_tree.get_children())
        for name, w, h, fmt in K.wad_textures(self.cur_body, game):
            ch = self.changes.get((self.cur_wad, name))
            self.tex_tree.insert('', 'end', iid=name, values=(name, '%dx%d' % (w, h), fmt,
                                                              os.path.basename(ch[0]) if ch else ''))
        self.tex_log.write('%s: %d texturas' % (src, len(self.tex_tree.get_children())))

    def cur_tex(self):
        sel = self.tex_tree.selection()
        return sel[0] if sel else None

    def preview(self):
        from PIL import Image, ImageTk
        name = self.cur_tex()
        if not name:
            return
        ch = self.changes.get((self.cur_wad, name))
        rgba = ch[1] if ch else K.texture_rgba(self.cur_body, self.tex_game.get(), name)
        im = Image.fromarray(rgba)
        im.thumbnail((320, 320))
        bg = Image.new('RGBA', im.size, (60, 60, 60, 255))
        bg.alpha_composite(im)
        self._img = ImageTk.PhotoImage(bg)
        self.canvas.config(image=self._img, text='', width=320)

    def export_png(self):
        from PIL import Image
        name = self.cur_tex()
        if not name:
            return
        p = filedialog.asksaveasfilename(defaultextension='.png', initialfile=name.strip('~').replace('~', '_') + '.png')
        if p:
            Image.fromarray(K.texture_rgba(self.cur_body, self.tex_game.get(), name)).save(p)
            self.tex_log.write('exportada: %s' % p)

    def export_all(self):
        from PIL import Image
        if not self.cur_wad:
            return
        d = filedialog.askdirectory()
        if not d:
            return
        for name in self.tex_tree.get_children():
            Image.fromarray(K.texture_rgba(self.cur_body, self.tex_game.get(), name)).save(
                os.path.join(d, name.strip('~').replace('~', '_') + '.png'))
        self.tex_log.write('%d texturas em %s' % (len(self.tex_tree.get_children()), d))

    def import_png(self):
        import numpy as np
        from PIL import Image
        name = self.cur_tex()
        if not name:
            return
        p = filedialog.askopenfilename(filetypes=[('PNG', '*.png'), ('Imagens', '*.png *.jpg *.bmp *.tga')])
        if not p:
            return
        im = Image.open(p).convert('RGBA')
        w, h = im.size
        if w & (w - 1) or h & (h - 1):  # o Vita quer lados potência de 2
            pw, ph = 1 << (w - 1).bit_length(), 1 << (h - 1).bit_length()
            im = im.resize((pw, ph), Image.LANCZOS)
            self.tex_log.write('%s: %dx%d -> %dx%d (potência de 2)' % (os.path.basename(p), w, h, pw, ph))
        self.changes[(self.cur_wad, name)] = (p, np.array(im))
        self.tex_tree.set(name, 'new', os.path.basename(p))
        self.preview()

    def undo_png(self):
        name = self.cur_tex()
        if name and self.changes.pop((self.cur_wad, name), None):
            self.tex_tree.set(name, 'new', '')
            self.preview()

    def save_tex_mod(self):
        if not self.changes:
            messagebox.showinfo('Texturas', 'Nenhuma textura trocada.')
            return
        name = self.tex_mod['name'].get().strip()
        if not name:
            messagebox.showwarning('Texturas', 'Dê um nome ao mod.')
            return
        game = self.tex_game.get()
        changes = [(wad, rec, rgba) for (wad, rec), (png, rgba) in self.changes.items()]
        self.work(self.tex_log, lambda: K.make_texture_mod(self.cfg, K.safe_id(name), name, game, changes,
                                                           author=self.tex_mod['author'].get(), log=self.tex_log.write),
                  lambda ok: (self.refresh_mods(), ok and self.changes.clear(),
                              ok and messagebox.showinfo('Texturas', 'Mod "%s" criado e instalado.' % name)))

    # -------------------------------------------------------------- aba Configurações
    def build_cfg(self):
        f = self.tab_cfg
        self.cfg_vars = {}
        rows = [('app', 'Pasta do jogo (PCSA00126, com exec/*.psarc)', 'dir'), ('data', 'Pasta vitart-data', 'dir'),
                ('work', 'Pasta de trabalho (temporários da conversão)', 'dir'), ('xdelta3', 'xdelta3.exe', 'file'),
                ('sevenzip', '7z.exe (para ISO dentro de .7z)', 'file'), ('python', 'Python', 'file')]
        g = ttk.Frame(f)
        g.pack(fill='x', padx=10, pady=10)
        for r, (k, label, kind) in enumerate(rows):
            ttk.Label(g, text=label).grid(row=r, column=0, sticky='w', pady=4)
            v = tk.StringVar(value=self.cfg.get(k, ''))
            self.cfg_vars[k] = v
            ttk.Entry(g, textvariable=v, width=90).grid(row=r, column=1, sticky='we', padx=4)
            ttk.Button(g, text='...', width=4, command=lambda v=v, kind=kind: v.set(
                (filedialog.askdirectory() if kind == 'dir' else filedialog.askopenfilename()) or v.get())).grid(row=r, column=2)
        g.columnconfigure(1, weight=1)
        ttk.Button(f, text='Salvar', command=self.save_cfg).pack(anchor='w', padx=10)
        ttk.Label(f, text='O xdelta3 oficial: github.com/jmacd/xdelta-gpl/releases (xdelta3-3.1.0-x86_64.exe).',
                  foreground=T.DIM).pack(anchor='w', padx=10, pady=10)

    def save_cfg(self):
        for k, v in self.cfg_vars.items():
            self.cfg[k] = v.get().strip()
        K.save_config(self.cfg)
        messagebox.showinfo('Configurações', 'Salvo em %s' % K.CONFIG)

    # -------------------------------------------------------------- aba Ajuda
    def build_help(self):
        f = self.tab_help
        txt = tk.Text(f, wrap='word', font=('Segoe UI', 10))
        sb = ttk.Scrollbar(f, command=txt.yview)
        txt['yscrollcommand'] = sb.set
        sb.pack(side='right', fill='y')
        txt.pack(fill='both', expand=True, padx=8, pady=8)
        try:
            txt.insert('end', open(DOCS, encoding='utf-8').read())
        except OSError:
            txt.insert('end', 'Guia não encontrado: %s' % DOCS)
        txt['state'] = 'disabled'


if __name__ == '__main__':
    Studio().mainloop()
