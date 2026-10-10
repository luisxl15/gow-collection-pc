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
        # Tabelas de entrada (deslocamentos relativos ao próprio campo).
        rel = lambda field: field + u32(field)
        prog.sa_count = struct.unpack_from("<H", blob, 0x32)[0]
        prog.pa_count = struct.unpack_from("<H", blob, 0x30)[0]
        prog.containers = [struct.unpack_from("<HHHH", blob, rel(0x94) + 8 * i) for i in range(u32(0x90))]
        prog.uniform_buffers = [struct.unpack_from("<HHi", blob, rel(0x7C) + 8 * i) for i in range(u32(0x78))]
        prog.literals = [struct.unpack_from("<If", blob, rel(0x74) + 8 * i) for i in range(u32(0x70))]
        return prog
