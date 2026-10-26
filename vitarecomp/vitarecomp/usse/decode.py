"""Decodificador de instruções USSE (GPU SGX543 do PS Vita) e leitor de programas GXP.

Primeiro estágio do tradutor USSE -> SPIR-V: identifica a classe de cada instrução de 64 bits e
extrai os campos nomeados (encodings.json, padrões de bits do ISA). O casamento segue a ordem da
tabela, a primeira que casa vence, como no hardware: as classes são separadas pelos 5 bits altos
(opcode1) e por bits fixos adicionais.
"""
import json
import os
import struct
from dataclasses import dataclass, field

_HERE = os.path.dirname(__file__)


@dataclass
class Encoding:
    name: str
    mask: int
    expect: int
    fields: dict  # nome -> lista de (bit, largura) do MSB ao LSB, concatenados


def _load():
    table = json.load(open(os.path.join(_HERE, "encodings.json"), encoding="utf-8"))
    encs = []
    for name, e in table.items():
        bits = e["bits"]
        mask = expect = 0
        spans = {}
        for i, ch in enumerate(bits):
            pos = 63 - i
            if ch in "01":
                mask |= 1 << pos
                if ch == "1":
                    expect |= 1 << pos
            elif ch != "-":
                spans.setdefault(e["fields"].get(ch, ch), []).append(pos)
        encs.append(Encoding(name, mask, expect, spans))
    return encs


ENCODINGS = _load()


@dataclass
class Instr:
    pc: int
    raw: int
    op: str
    f: dict = field(default_factory=dict)

    def __str__(self):
        args = " ".join("%s=%d" % kv for kv in sorted(self.f.items()))
        return "%04d %016x %-9s %s" % (self.pc, self.raw, self.op, args)


def decode(raw, pc=0):
    for e in ENCODINGS:
        if raw & e.mask == e.expect:
            vals = {}
            for name, positions in e.fields.items():
                v = 0
                for p in positions:  # do MSB para o LSB, como na string de bits
                    v = (v << 1) | ((raw >> p) & 1)
                vals[name] = v
            return Instr(pc, raw, e.name, vals)
    return Instr(pc, raw, "?")


@dataclass
class Program:
    """SceGxmProgram: cabeçalho e as duas fases de código."""
    blob: bytes
    fragment: bool
    primary: list
    secondary: list
    params: list
    default_uniform_words: int
    sa_count: int = 0
    pa_count: int = 0
    containers: list = None      # (índice, ?, base_sa_offset, tamanho em f32)
    uniform_buffers: list = None  # (buffer onde reside, ldst_base_offset, ldst_base_value)
    literals: list = None         # (deslocamento em SA, valor)
    dep_samplers: list = None     # (layout, sa_offset) dos samplers de leitura dependente
    vertex_outputs: list = None   # vértice: (nome, location, n componentes, 1º registrador OUTPUT)
    frag_inputs: list = None      # fragmento: iteradores e leituras de textura não dependentes (dicts)
    output_type: int = 0          # fragmento: SceGxmParameterType da cor de saída
    output_components: int = 4
    output_start: int = 0         # fragmento: 1º PA da cor de saída (com cor nativa, OUTPUT 0)
    flags: int = 0
    native_color: bool = False

    @staticmethod
    def parse(blob):
        u32 = lambda o: struct.unpack_from("<I", blob, o)[0]
        if blob[:4] != b"GXP\0":
            raise ValueError("não é GXP")
        flags = u32(0x14)
        n_primary = u32(0x3C)
        primary_at = 0x40 + u32(0x40)
        secondary_at = 0x48 + u32(0x48)
        secondary_end = 0x4C + u32(0x4C)
        words = lambda a, n: [struct.unpack_from("<Q", blob, a + 8 * i)[0] for i in range(n)]
        primary = [decode(w, i) for i, w in enumerate(words(primary_at, n_primary))]
        n_sec = max(0, (secondary_end - secondary_at) // 8)
        secondary = [decode(w, i) for i, w in enumerate(words(secondary_at, n_sec))]
        params = []
        count, poff = u32(0x24), 0x28 + u32(0x28)
        for i in range(count):
            p = poff + 16 * i
            noff, b4, b5, sem, semi, arr, res = struct.unpack_from("<IBBBBII", blob, p)
            name = blob[p + noff:blob.index(b"\0", p + noff)].decode("latin-1")
            params.append(dict(name=name, category=b4 & 15, type=b4 >> 4, components=b5 & 15, container=b5 >> 4,
                               semantic=sem, array=arr, resource=res))
        prog = Program(blob, bool(flags & 1), primary, secondary, params, u32(0x64))
        prog.flags = flags
        prog.native_color = bool(flags & 0x40)
        # Tabelas de entrada (deslocamentos relativos ao próprio campo).
        rel = lambda field: field + u32(field)
        prog.sa_count = struct.unpack_from("<H", blob, 0x32)[0]
        prog.pa_count = struct.unpack_from("<H", blob, 0x30)[0]
        prog.containers = [struct.unpack_from("<HHHH", blob, rel(0x94) + 8 * i) for i in range(u32(0x90))]
        prog.uniform_buffers = [struct.unpack_from("<HHi", blob, rel(0x7C) + 8 * i) for i in range(u32(0x78))]
        prog.literals = [struct.unpack_from("<If", blob, rel(0x74) + 8 * i) for i in range(u32(0x70))]
        # Samplers das leituras dependentes (smp): {layout, sa_offset}; recurso = layout // 4, e os 4 layouts de um
        # recurso são as 4 palavras do estado da textura em SA (base do contêiner 19 + sa_offset).
        prog.dep_samplers = [struct.unpack_from("<HH", blob, rel(0x84) + 4 * i) for i in range(u32(0x80))]
        prog._parse_varyings(rel(0x2C))
        return prog

    # Saídas do vértice e entradas do fragmento (bloco de 32 bytes apontado pelo campo 0x2C).
    # Saídas: os registradores OUTPUT vêm empacotados na ordem posição(4), cor0(4), cor1(4), fog(2),
    # texcoord0..9 (2 a 4 componentes, 3 bits por coordenada em vertex_outputs2), psize(1).
    # Entradas: varyings_count descritores de 16 bytes {info, recurso, tamanho, componentes}; cada um
    # pode trazer um iterador (cor/texcoord interpolados) e uma leitura de textura não dependente, ambos
    # gravados nos PA em sequência.
    VARYING_LOC = {0xD: ("position", -1), 0xC: ("fog", 3), 0xA: ("color0", 1), 0xB: ("color1", 2)}

    def _parse_varyings(self, at):
        blob = self.blob
        u32 = lambda o: struct.unpack_from("<I", blob, o)[0]
        vo1, vo2 = u32(at + 0x10), u32(at + 0x14)
        if not self.fragment:
            outs = [("position", -1, 4)]
            if vo1 & 0x800:
                outs.append(("color0", 1, 4))
            if vo1 & 0x400:
                outs.append(("color1", 2, 4))
            if vo1 & 0x200:
                outs.append(("fog", 3, 2))
            for i in range(10):
                info = (vo2 >> (3 * i)) & 7
                if info:
                    n = (2 if info & 1 else 0) + (1 if info & 2 else 0) + (1 if info & 4 else 0)
                    outs.append(("texcoord%d" % i, 4 + i, n))
            if vo1 & 0x100:
                outs.append(("psize", -2, 1))
            reg, self.vertex_outputs = 0, []
            for name, loc, n in outs:
                self.vertex_outputs.append((name, loc, n, reg))
                reg += n
            return
        self.output_start = blob[at + 0x08]
        self.output_type = blob[at + 0x0A] & 0xF
        self.output_components = blob[at + 0x0B]
        count = struct.unpack_from("<H", blob, at + 0x0C)[0]
        desc_at = at + 0x10 + vo1
        pa, self.frag_inputs = 0, []
        for i in range(count):
            info, res, size, comp = struct.unpack_from("<IIII", blob, desc_at + 16 * i)
            if info & 0x4000F000 != 0xF000:
                if info & 0x40000000:
                    name, loc = "spritecoord", -3
                else:
                    sel = (info >> 12) & 0xF
                    name, loc = self.VARYING_LOC.get(sel, ("texcoord%d" % sel, 4 + sel))
                kind = info & 0x30100000
                if kind == 0x20000000:
                    dtype = "f16"
                elif kind == 0x10000000:
                    dtype = "fixed"
                elif kind == 0x100000:
                    dtype = "f32" if name in ("color0", "color1") else "u8"
                else:
                    dtype = "u8" if name in ("color0", "color1") else "f32"
                regs = ((size >> 4) & 3) + 1
                self.frag_inputs.append(dict(kind="iter", name=name, loc=loc, comps=((info >> 22) & 3) + 1,
                                             dtype=dtype, pa=pa, regs=regs))
                pa += regs
            tc = info & 0x40F
            if tc != 0xF:
                ctype = (comp >> 4) & 3
                store = {0: "int", 2: "f16", 3: "f32"}.get(ctype, "f16")
                proj = {0x100: None, 0x200: 3, 0x300: 2}.get(info & 0x300, None)
                regs = ((size >> 6) & 3) + 1
                self.frag_inputs.append(dict(kind="tex", coord=10 if tc == 0x400 else tc, sampler=res, store=store,
                                             proj=proj, pa=pa, regs=regs, centroid=bool(info & 0x10)))
                pa += regs
