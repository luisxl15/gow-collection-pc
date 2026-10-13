"""Leitor/gravador do FLP (interface tipo Flash da Santa Monica) do God of War 1 do Vita.

    python tools/flp.py <arquivo.flp> dump            # árvore: clipes, quadros, rótulos, scripts
    python tools/flp.py <arquivo.flp> roundtrip       # confere que ler+gravar devolve os mesmos bytes

Formato (porte do god_of_war_browser de mogaika, pack/wad/flp, caminho GOW1 sem PS2: strings por
deslocamento na seção de strings). O arquivo é um fluxo: cabeçalho de 0x60 com contagens, depois cada
tabela na ordem fixa, cada elemento com cabeçalho fixo e corpo variável; alinhamentos de 4 bytes. Não há
ponteiros absolutos (os do cabeçalho são lixo da máquina do estúdio), só deslocamentos u16 para a seção de
strings, então dá para regravar com tamanhos novos.

Tipos de alça global (TypeArrayId): 1 malha, 3 fonte, 4 rótulo estático, 5 rótulo dinâmico, 6 clipe com
eventos de botão (Data6), 7 clipe (Data7), 8 raiz, 9 transformação, 10 cor.
"""
import struct
import sys


def pad4(p):
    return (p + 3) & ~3


class R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def u16(self, o=None):
        return struct.unpack_from('<H', self.b, self.p if o is None else o)[0]

    def u32(self, o=None):
        return struct.unpack_from('<I', self.b, self.p if o is None else o)[0]


class W:
    def __init__(self):
        self.b = bytearray()

    def pad(self):
        while len(self.b) % 4:
            self.b.append(0)

    def raw(self, x):
        self.b += x


# ---------------------------------------------------------------- estruturas
class Mesh:  # MeshPartReference: cabeçalho de 8 + materiais de 8
    def head(self, b, p):
        self.h = bytearray(b[p:p + 8])
        self.nmat = struct.unpack_from('<H', b, p + 2)[0]
        return p + 8

    def body(self, b, p):
        self.mats = [bytearray(b[p + 8 * i:p + 8 * i + 8]) for i in range(self.nmat)]
        return p + 8 * self.nmat

    def w_head(self, w):
        struct.pack_into('<H', self.h, 2, len(self.mats))
        w.raw(self.h)

    def w_body(self, w):
        for m in self.mats:
            w.raw(m)


class Font:
    def head(self, b, p):
        self.h = bytearray(b[p:p + 0x24])
        self.count = struct.unpack_from('<I', b, p)[0]
        self.size = struct.unpack_from('<h', b, p + 6)[0]
        self.flags = struct.unpack_from('<H', b, p + 0xc)[0]
        return p + 0x24

    def body(self, b, p):
        self.meshes = []
        if self.flags & 6:
            self.meshes = [Mesh() for _ in range(self.count)]
            for m in self.meshes:
                p = m.head(b, p)
            for m in self.meshes:
                p = m.body(b, p)
        self.widths = list(struct.unpack_from('<%dh' % self.count, b, p))
        p = pad4(p + 2 * self.count)
        n = 0x100 if self.flags & 1 else self.count
        self.cmap = list(struct.unpack_from('<%dh' % n, b, p))
        return pad4(p + 2 * n)

    def w_head(self, w):
        w.raw(self.h)

    def w_body(self, w):
        for m in self.meshes:
            m.w_head(w)
        for m in self.meshes:
            m.w_body(w)
        w.raw(struct.pack('<%dh' % len(self.widths), *self.widths))
        w.pad()
        w.raw(struct.pack('<%dh' % len(self.cmap), *self.cmap))
        w.pad()


class Static:  # rótulo estático: transformação (0x14) + tamanho dos comandos em +0x14 ... 0x24
    def head(self, b, p):
        self.h = bytearray(b[p:p + 0x24])
        self.n = struct.unpack_from('<I', b, p + 0x14)[0]
        return p + 0x24

    def body(self, b, p):
        self.cmds = parse_label(b[p:p + self.n])
        return pad4(p + self.n)

    def w_head(self, w):
        self.data = write_label(self.cmds)
        struct.pack_into('<I', self.h, 0x14, len(self.data))
        w.raw(self.h)

    def w_body(self, w):
        w.raw(self.data)
        w.pad()


def parse_label(d):
    """Lista de comandos: dict(flags, font, scale, color, x, y, glyphs=[(id, largura)])."""
    out, cur, i = [], None, 0
    while i < len(d):
        op = d[i]
        i += 1
        if op & 0x80:
            cur = {'flags': op & 0x7f, 'glyphs': []}
            out.append(cur)
            if op & 8:
                cur['font'], cur['scale'] = struct.unpack_from('<Hh', d, i)
                i += 4
            if op & 4:
                cur['color'] = bytes(d[i:i + 4])
                i += 4
            if op & 2:
                cur['x'] = struct.unpack_from('<h', d, i)[0]
                i += 2
            if op & 1:
                cur['y'] = struct.unpack_from('<h', d, i)[0]
                i += 2
        else:
            if cur is None:
                cur = {'flags': 0, 'glyphs': []}
                out.append(cur)
            for _ in range(op):
                cur['glyphs'].append(struct.unpack_from('<Hh', d, i))
                i += 4
    return out


def write_label(cmds):
    o = bytearray()
    for c in cmds:
        f = c['flags']
        if f:
            o.append(f | 0x80)
            if f & 8:
                o += struct.pack('<Hh', c['font'], c['scale'])
            if f & 4:
                o += c['color']
            if f & 2:
                o += struct.pack('<h', c['x'])
            if f & 1:
                o += struct.pack('<h', c['y'])
        g = c['glyphs']
        if g or not f:
            o.append(len(g))
            for gid, wd in g:
                o += struct.pack('<Hh', gid, wd)
    return bytes(o)


class Clip:  # Data6Subtype1 (0x18): quadros, animações de elementos, rótulos de quadro com scripts
    def head(self, b, p):
        self.h = bytearray(b[p:p + 0x18])
        self.frames = struct.unpack_from('<H', b, p)[0]
        self.nanim, self.nlab = struct.unpack_from('<HH', b, p + 2)
        return p + 0x18

    def body(self, b, p):
        p = pad4(p)
        self.anims = []
        for _ in range(self.nanim):
            self.anims.append([bytearray(b[p:p + 8]), []])
            p += 8
        for a in self.anims:
            p = pad4(p)
            nk = struct.unpack_from('<H', a[0], 2)[0]
            a[1] = [bytearray(b[p + 10 * k:p + 10 * k + 10]) for k in range(nk)]
            p += 10 * nk
        p = pad4(p)
        self.labels = []
        for _ in range(self.nlab):
            self.labels.append([bytearray(b[p:p + 0xc]), []])
            p += 0xc
        for lab in self.labels:
            p = pad4(p)
            ns = struct.unpack_from('<H', lab[0], 2)[0]
            heads = []
            for _ in range(ns):
                heads.append(bytearray(b[p:p + 8]))
                p += 8
            for h in heads:
                p = pad4(p)
                n = struct.unpack_from('<I', h, 0)[0]
                lab[1].append([h, bytes(b[p:p + n])])
                p += n
        return p

    def w_head(self, w):
        struct.pack_into('<HH', self.h, 2, len(self.anims), len(self.labels))
        w.raw(self.h)

    def w_body(self, w):
        w.pad()
        for h, keys in self.anims:
            struct.pack_into('<H', h, 2, len(keys))
            w.raw(h)
        for h, keys in self.anims:
            w.pad()
            for k in keys:
                w.raw(k)
        w.pad()
        for h, subs in self.labels:
            struct.pack_into('<H', h, 2, len(subs))
            w.raw(h)
        for h, subs in self.labels:
            w.pad()
            for sh, code in subs:
                struct.pack_into('<I', sh, 0, len(code))  # tamanho antes de gravar o cabeçalho
                w.raw(sh)
            for sh, code in subs:
                w.pad()
                w.raw(code)


class Clip6:  # Data6: cabeçalho de 0xc (eventos em +8) + Clip + eventos de botão (0x10 + script)
    def head(self, b, p):
        self.h = bytearray(b[p:p + 0xc])
        self.nev = struct.unpack_from('<H', b, p + 8)[0]
        return p + 0xc

    def body(self, b, p):
        p = pad4(p)
        self.clip = Clip()
        p = self.clip.head(b, p)
        p = self.clip.body(b, p)
        heads = []
        for _ in range(self.nev):
            heads.append(bytearray(b[p:p + 0x10]))
            p += 0x10
        self.events = []
        for h in heads:
            p = pad4(p)
            n = struct.unpack_from('<I', h, 0)[0]
            self.events.append([h, bytes(b[p:p + n])])
            p += n
        return p

    def w_head(self, w):
        struct.pack_into('<H', self.h, 8, len(self.events))
        w.raw(self.h)

    def w_body(self, w):
        w.pad()
        self.clip.w_head(w)
        self.clip.w_body(w)
        for h, code in self.events:
            struct.pack_into('<I', h, 0, len(code))
            w.raw(h)
        for h, code in self.events:
            w.pad()
            w.raw(code)


class FLP:
    def __init__(self, b):
        self.hdr = bytearray(b[:0x60])
        r = R(b)
        cnt = lambda o: r.u32(o)
        nh, nm, nf, ns, nd, n6, n7 = (cnt(o) for o in (0xc, 0x14, 0x1c, 0x24, 0x2c, 0x34, 0x3c))
        nt, nc = r.u16(0x48), r.u16(0x50)
        p = 0x60
        self.handlers = [struct.unpack_from('<HH', b, p + 4 * i) for i in range(nh)]
        p += 4 * nh
        self.meshes = [Mesh() for _ in range(nm)]
        for m in self.meshes:
            p = m.head(b, p)
        for m in self.meshes:
            p = m.body(b, p)
        self.fonts = [Font() for _ in range(nf)]
        for f in self.fonts:
            p = f.head(b, p)
        for f in self.fonts:
            p = f.body(b, p)
        self.statics = [Static() for _ in range(ns)]
        for s in self.statics:
            p = s.head(b, p)
        for s in self.statics:
            p = s.body(b, p)
        self.dyns = [bytearray(b[p + 0x20 * i:p + 0x20 * i + 0x20]) for i in range(nd)]
        p += 0x20 * nd
        self.c6 = [Clip6() for _ in range(n6)]
        for c in self.c6:
            p = c.head(b, p)
        for c in self.c6:
            p = c.body(b, p)
        p = pad4(p)
        self.c7 = [Clip() for _ in range(n7)]
        for c in self.c7:
            p = c.head(b, p)
        for c in self.c7:
            p = c.body(b, p)
        p = pad4(p)
        self.root = Clip()
        p = self.root.head(b, p)
        p = self.root.body(b, p)
        p = pad4(p)
        self.xforms = [bytearray(b[p + 0x14 * i:p + 0x14 * i + 0x14]) for i in range(nt)]
        p += 0x14 * nt
        self.colors = [bytearray(b[p + 8 * i:p + 8 * i + 8]) for i in range(nc)]
        p += 8 * nc
        self.strings_at = p
        self.strings = bytearray(b[p:])

    def s(self, off):
        if off == 0xffff:
            return None
        e = self.strings.find(b'\0', off)
        return self.strings[off:e].decode('latin1')

    def add_string(self, txt):
        """Deslocamento de uma string na seção (reaproveita se já existir)."""
        k = bytes(txt.encode('latin1')) + b'\0'
        i = 0
        while True:
            i = self.strings.find(k, i)
            if i < 0:
                break
            if i == 0 or self.strings[i - 1] == 0:
                return i
            i += 1
        off = len(self.strings)
        self.strings += k
        assert off < 0xffff
        return off

    def write(self):
        w = W()
        h = self.hdr
        for o, v in ((0xc, len(self.handlers)), (0x14, len(self.meshes)), (0x1c, len(self.fonts)),
                     (0x24, len(self.statics)), (0x2c, len(self.dyns)), (0x34, len(self.c6)), (0x3c, len(self.c7))):
            struct.pack_into('<I', h, o, v)
        struct.pack_into('<H', h, 0x48, len(self.xforms))
        struct.pack_into('<H', h, 0x50, len(self.colors))
        struct.pack_into('<H', h, 0x58, len(self.strings) & 0xffff)
        w.raw(h)
        for a, b_ in self.handlers:
            w.raw(struct.pack('<HH', a, b_))
        for group in (self.meshes, self.fonts, self.statics):
            for x in group:
                x.w_head(w)
            for x in group:
                x.w_body(w)
        for d in self.dyns:
            w.raw(d)
        for c in self.c6:
            c.w_head(w)
        for c in self.c6:
            c.w_body(w)
        w.pad()
        for c in self.c7:
            c.w_head(w)
        for c in self.c7:
            c.w_body(w)
        w.pad()
        self.root.w_head(w)
        self.root.w_body(w)
        w.pad()
        for x in self.xforms:
            w.raw(x)
        for c in self.colors:
            w.raw(c)
        w.raw(self.strings)
        return bytes(w.b)


# ---------------------------------------------------------------- scripts
def disasm(f, code):
    """Lista de (deslocamento, texto) do script (variante do Vita: strings por deslocamento)."""
    out, i = [], 0
    S = lambda o: repr(f.s(struct.unpack_from('<H', code, o)[0]))
    names = {0: 'end', 6: 'play', 7: 'stop', 0xa: 'add', 0xb: 'sub', 0xc: 'mul', 0xd: 'div', 0xe: 'eq',
             0xf: 'lt', 0x10: 'and', 0x11: 'or', 0x12: 'not', 0x13: 'streq', 0x14: 'strlen', 0x15: 'substr',
             0x17: 'pop', 0x18: 'round', 0x1c: 'getvar', 0x1d: 'setvar', 0x20: 'settarget_pop', 0x21: 'concat',
             0x22: 'getprop', 0x23: 'setprop', 0x26: 'trace', 0x29: 'strne', 0x34: 'gettimer'}
    while i < len(code):
        op = code[i]
        at = i
        i += 1
        if op & 0x80:
            if op == 0x81:
                t = 'gotoframe %d' % struct.unpack_from('<H', code, i)[0]
                i += 2
            elif op == 0x83:
                t = 'fscommand %s %s' % (S(i), S(i + 2))
                i += 4
            elif op == 0x8b:
                t = 'settarget %s' % S(i)
                i += 2
            elif op == 0x8c:
                t = 'gotolabel %s' % S(i)
                i += 2
            elif op == 0x96:
                if code[i] == 1:
                    t = 'push %g' % struct.unpack_from('<f', code, i + 1)[0]
                    i += 5
                elif code[i] != 0:
                    t = 'push %s' % S(i)
                    i += 2
                else:
                    t = 'push %s' % S(i + 1)
                    i += 3
            elif op in (0x99, 0x9d):
                d = struct.unpack_from('<h', code, i)[0]
                t = '%s 0x%x' % ('jump' if op == 0x99 else 'jumpif', at + d + 3)
                i += 2
            elif op == 0x9e:
                t = 'callframe'
            elif op == 0x9f:
                t = 'gotoexpr %s' % ('play' if code[i] else 'stop')
                i += 1
            else:
                t = 'op?%02x' % op
        else:
            t = names.get(op, 'op%02x' % op)
        out.append((at, t))
    return out


def label_text(f, st):
    """Texto aproximado de um rótulo estático pelo mapa caractere->glifo da fonte."""
    s = ''
    font = None
    for c in st.cmds:
        if 'font' in c:
            h = f.handlers[c['font']] if c['font'] < len(f.handlers) else None
            font = f.fonts[h[1]] if h and h[0] == 3 and h[1] < len(f.fonts) else None
        for gid, _ in c['glyphs']:
            ch = '?'
            if font is not None and font.flags & 1:
                if gid in font.cmap:
                    ch = chr(font.cmap.index(gid))
            s += ch
        s += ' '
    return s.strip()


def dump(f):
    hn = ['-', 'malha', '?', 'fonte', 'rótulo', 'dinâmico', 'clipe6', 'clipe7', 'raiz', 'transf', 'cor']

    def handler(i):
        if i >= len(f.handlers):
            return 'alça?%d' % i
        t, k = f.handlers[i]
        return '%s[%d]' % (hn[t] if t < len(hn) else t, k)

    print('fontes:', [(x.count, x.size, hex(x.flags)) for x in f.fonts])
    for i, d in enumerate(f.dyns):
        v, ph = struct.unpack_from('<HH', d, 0)
        print('dinâmico[%d] var=%s texto=%r' % (i, f.s(v), f.s(ph)))
    for i, st in enumerate(f.statics):
        print('rótulo[%d] %r' % (i, label_text(f, st)))

    def clip(name, c, events=None):
        print('== %s quadros=%d' % (name, c.frames))
        for h, keys in c.anims:
            for k in keys:
                end, hd, tr, col, nm = struct.unpack_from('<HHHHH', k)
                print('   elem %-12s até %3d nome=%s' % (handler(hd), end, f.s(nm)))
        for h, subs in c.labels:
            frame, _, nm = struct.unpack_from('<HHH', h, 0)[0], 0, struct.unpack_from('<H', h, 8)[0]
            print('   quadro %d rótulo=%s' % (frame, f.s(nm)))
            for _, code in subs:
                for at, t in disasm(f, code):
                    print('      %04x %s' % (at, t))
        for h, code in events or []:
            mask = struct.unpack_from('<I', h, 8)[0]
            print('   evento botões=0x%x' % mask)
            for at, t in disasm(f, code):
                print('      %04x %s' % (at, t))

    for i, c in enumerate(f.c6):
        clip('clipe6[%d]' % i, c.clip, c.events)
    for i, c in enumerate(f.c7):
        clip('clipe7[%d]' % i, c)
    clip('raiz', f.root)


def main():
    b = open(sys.argv[1], 'rb').read()
    f = FLP(b)
    if sys.argv[2] == 'roundtrip':
        o = f.write()
        print('igual' if o == b else 'DIFERENTE (%d vs %d bytes)' % (len(o), len(b)))
        if o != b:
            k = next(i for i in range(min(len(o), len(b))) if o[i] != b[i])
            print('primeira diferença em 0x%x' % k)
    else:
        dump(f)


# ---------------------------------------------------------------- montador de scripts
# Instrução: [op, arg]. arg: None, int (gotoframe), str (strings), (str, str) (fscommand), float (push
# número), ('L', índice da instrução de destino) para saltos, int 0/1 em gotoexpr. Saltos ficam por índice,
# então dá para inserir instruções e regravar.
def decode(f, code):
    ins, at2i, i = [], {}, 0
    S = lambda o: f.s(struct.unpack_from('<H', code, o)[0])
    raw_jumps = []
    while i < len(code):
        op, at = code[i], i
        at2i[at] = len(ins)
        i += 1
        arg = None
        if op & 0x80:
            if op == 0x81:
                arg = struct.unpack_from('<H', code, i)[0]; i += 2
            elif op == 0x83:
                arg = (S(i), S(i + 2)); i += 4
            elif op in (0x8b, 0x8c):
                arg = S(i); i += 2
            elif op == 0x96:
                if code[i] == 1:
                    arg = struct.unpack_from('<f', code, i + 1)[0]; i += 5
                elif code[i] != 0:
                    arg = S(i); i += 2
                else:
                    arg = S(i + 1); i += 3
            elif op in (0x99, 0x9d):
                raw_jumps.append((len(ins), at + struct.unpack_from('<h', code, i)[0] + 3)); i += 2
            elif op == 0x9e:
                pass
            elif op == 0x9f:
                arg = code[i]; i += 1
            else:
                raise ValueError('opcode %02x' % op)
        ins.append([op, arg])
    at2i[len(code)] = len(ins)
    for k, tgt in raw_jumps:
        ins[k][1] = ('L', at2i[tgt])
    return ins, at2i


def encode(f, ins):
    # dois passes: tamanhos dependem só do opcode e da forma do push de string (deslocamento)
    def size(op, arg):
        if not op & 0x80:
            return 1
        if op == 0x96:
            if isinstance(arg, float):
                return 6
            f.add_string(arg)
            return 4  # o jogo sempre usa 96 00 <u16>
        return 1 + {0x81: 2, 0x83: 4, 0x8b: 2, 0x8c: 2, 0x99: 2, 0x9d: 2, 0x9e: 0, 0x9f: 1}[op]
    pos = []
    p = 0
    for op, arg in ins:
        pos.append(p)
        p += size(op, arg)
    pos.append(p)
    out = bytearray()
    for k, (op, arg) in enumerate(ins):
        out.append(op)
        if not op & 0x80:
            continue
        if op == 0x81:
            out += struct.pack('<H', arg)
        elif op == 0x83:
            out += struct.pack('<HH', f.add_string(arg[0]), f.add_string(arg[1]))
        elif op in (0x8b, 0x8c):
            out += struct.pack('<H', f.add_string(arg))
        elif op == 0x96:
            if isinstance(arg, float):
                out += b'\x01' + struct.pack('<f', arg)
            else:
                out += bytes(1) + struct.pack('<H', f.add_string(arg))
        elif op in (0x99, 0x9d):
            out += struct.pack('<h', pos[arg[1]] - pos[k] - 3)
        elif op == 0x9f:
            out.append(arg)
    return bytes(out)


class Asm:
    """Construtor de scripts com rótulos simbólicos: a.push('x'); a.jumpif('fim'); a.label('fim')."""
    OPS = {'end': 0, 'play': 6, 'stop': 7, 'add': 0xa, 'sub': 0xb, 'mul': 0xc, 'div': 0xd, 'eq': 0xe, 'lt': 0xf,
           'and_': 0x10, 'or_': 0x11, 'not_': 0x12, 'toint': 0x18, 'getvar': 0x1c, 'setvar': 0x1d, 'concat': 0x21}

    def __init__(self):
        self.ins, self.labels, self.fix = [], {}, []

    def __getattr__(self, n):
        if n in Asm.OPS:
            return lambda: self.ins.append([Asm.OPS[n], None]) or self
        raise AttributeError(n)

    def push(self, v):
        # números como string, como o próprio jogo faz ('1', '10'); o motor converte na comparação/soma
        self.ins.append([0x96, ('%g' % v) if isinstance(v, (int, float)) else v]); return self

    def settarget(self, s):
        self.ins.append([0x8b, s]); return self

    def gotolabel(self, s):
        self.ins.append([0x8c, s]); return self

    def fscommand(self, cmd, arg=''):
        self.ins.append([0x83, (cmd, arg)]); return self

    def gotoframe(self, n):
        self.ins.append([0x81, n]); return self

    def callframe(self, name):
        self.push(name); self.ins.append([0x9e, None]); return self

    def jump(self, lab):
        self.fix.append(len(self.ins)); self.ins.append([0x99, lab]); return self

    def jumpif(self, lab):
        self.fix.append(len(self.ins)); self.ins.append([0x9d, lab]); return self

    def label(self, lab):
        self.labels[lab] = len(self.ins); return self

    def setv(self, name, value):
        return self.push(name).push(value).setvar()

    def getv(self, name):
        return self.push(name).getvar()

    def build(self, base=0, extern=None):
        """Instruções com saltos resolvidos; base = índice onde o bloco entra num script maior;
        extern: rótulos que apontam para índices do script hospedeiro (já finais)."""
        out = [list(x) for x in self.ins]
        for k in self.fix:
            lab = out[k][1]
            if lab in self.labels:
                out[k][1] = ('L', base + self.labels[lab])
            else:
                out[k][1] = ('L', extern[lab])
        return out


def insert(ins, at, block):
    """Insere instruções (já com saltos absolutos para o resultado) antes do índice at, corrigindo os saltos
    existentes que apontam para depois do ponto de inserção. Saltos para exatamente 'at' continuam indo para
    o bloco novo (ele vira o começo daquele trecho)."""
    n = len(block)
    for x in ins:
        if isinstance(x[1], tuple) and x[1][0] == 'L' and x[1][1] > at:
            x[1] = ('L', x[1][1] + n)
    return ins[:at] + block + ins[at:]


if __name__ == '__main__':
    main()
