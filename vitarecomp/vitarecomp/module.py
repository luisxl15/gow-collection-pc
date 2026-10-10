"""PS Vita module loader for static analysis / recompilation.

Accepts a SELF/FSELF (``SCE\\0`` header, decrypted segments) or a plain Vita ELF
(ET_SCE_EXEC 0xFE00 / ET_SCE_RELEXEC 0xFE04). Segments are placed at their
link-time ``p_vaddr`` and SCE relocations (formats 0-9) are applied, so the
resulting image is what the code expects when loaded at its preferred base.
Every applied relocation is recorded: they are exact evidence of which words
are pointers (code pointers, jump/vtables) and which MOVW/MOVT pairs build
addresses.

Behaviour mirrors Vita3K (vita3k/kernel/src/load_self.cpp, relocation.cpp).
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

ET_SCE_EXEC, ET_SCE_RELEXEC = 0xFE00, 0xFE04
PT_LOAD, PT_SCE_RELA, PT_SCE_COMMENT, PT_SCE_VERSION, PT_ARM_EXIDX = 1, 0x60000000, 0x6FFFFF00, 0x6FFFFF01, 0x70000001
EM_ARM = 40

RELOC_NAMES = {0: "NONE", 2: "ABS32", 3: "REL32", 8: "ABS8", 10: "THM_CALL", 28: "CALL", 29: "JUMP24",
               38: "TARGET1", 40: "V4BX", 41: "TARGET2", 42: "PREL31", 43: "MOVW_ABS_NC",
               44: "MOVT_ABS", 47: "THM_MOVW_ABS_NC", 48: "THM_MOVT_ABS", 255: "RBASE"}


class ModuleError(Exception):
    pass


@dataclass
class Segment:
    index: int
    vaddr: int
    memsz: int
    filesz: int
    flags: int
    data: bytearray

    @property
    def executable(self) -> bool:
        return bool(self.flags & 1)

    def contains(self, addr: int) -> bool:
        return self.vaddr <= addr < self.vaddr + self.memsz


@dataclass
class Reloc:
    addr: int       # patched address
    code: int       # ARM reloc code
    target: int     # S + A (absolute value the reloc refers to)

    @property
    def name(self) -> str:
        return RELOC_NAMES.get(self.code, str(self.code))


@dataclass
class Import:
    library: str
    library_nid: int
    nid: int
    stub_addr: int  # address of the import stub (function) or the var reference
    kind: str       # "func" | "var" | "tls"
    name: str | None = None


@dataclass
class Export:
    library: str
    library_nid: int
    nid: int
    addr: int
    kind: str
    name: str | None = None


@dataclass
class Module:
    name: str
    elf_type: int
    segments: list[Segment]
    relocs: list[Reloc] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
    exports: list[Export] = field(default_factory=list)
    module_start: int | None = None
    module_stop: int | None = None
    exidx: tuple[int, int] | None = None
    tls: tuple[int, int, int] | None = None
    info_addr: int = 0
    process_param: int | None = None

    # ---- memory helpers on the link-time image ----
    def seg_of(self, addr: int) -> Segment | None:
        return next((s for s in self.segments if s.contains(addr)), None)

    def read(self, addr: int, n: int) -> bytes:
        s = self.seg_of(addr)
        if s is None or addr + n > s.vaddr + s.memsz:
            raise ModuleError(f"read outside segments: {addr:#x}+{n}")
        off = addr - s.vaddr
        return bytes(s.data[off:off + n]).ljust(n, b"\0")

    def u32(self, addr: int) -> int:
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u16(self, addr: int) -> int:
        return struct.unpack("<H", self.read(addr, 2))[0]

    def cstr(self, addr: int, limit: int = 256) -> str:
        raw = self.read(addr, min(limit, self.seg_of(addr).vaddr + self.seg_of(addr).memsz - addr))
        return raw.split(b"\0", 1)[0].decode("latin-1")


def _bits(v: int, lo: int, hi: int) -> int:
    return (v >> lo) & ((1 << (hi - lo)) - 1)


def _elf_from_self(data: bytes) -> tuple[bytes, list[tuple[int, int, int, int]], int]:
    """Return (elf_header_bytes_view, segment_infos, header_len)."""
    (magic, version, _sdk, header_type, _meta, header_len, _elfsz, _selfsz, _unk, _selfoff,
     _appinfo, elf_off, phdr_off, _shdr, secinfo_off) = struct.unpack_from("<IIHHIQQQQQQQQQQ", data, 0)
    if magic != 0x00454353:
        raise ModuleError("not a SELF")
    if version != 3 or header_type != 1:
        raise ModuleError(f"unsupported SELF version {version}/type {header_type}")
    return elf_off, phdr_off, secinfo_off, header_len


def load(data: bytes, name: str = "module") -> Module:
    is_self = data[:4] == b"SCE\0"
    if is_self:
        elf_off, phdr_off, secinfo_off, header_len = _elf_from_self(data)
    else:
        elf_off, header_len = 0, 0
    if data[elf_off:elf_off + 4] != b"\x7fELF":
        raise ModuleError("ELF header not found")
    (e_type, e_machine, _v, e_entry, e_phoff, _shoff, _flags, _ehs, e_phentsize, e_phnum) = struct.unpack_from(
        "<HHIIIIIHHH", data, elf_off + 16)
    if e_machine != EM_ARM:
        raise ModuleError(f"e_machine={e_machine}, expected ARM")
    if e_type not in (ET_SCE_EXEC, ET_SCE_RELEXEC):
        raise ModuleError(f"e_type={e_type:#x} is not a Vita executable (0xFE00/0xFE04)")
    if not is_self:
        phdr_off = elf_off + e_phoff

    phdrs = [struct.unpack_from("<IIIIIIII", data, phdr_off + i * e_phentsize) for i in range(e_phnum)]
    seginfo = []
    if is_self:
        for i in range(e_phnum):
            off, length, comp, enc = struct.unpack_from("<QQQQ", data, secinfo_off + i * 32)
            if enc != 2:
                raise ModuleError(f"segment {i} is encrypted (encryption={enc}); a decrypted dump is required")
            seginfo.append((off, length, comp))

    def seg_bytes(i: int) -> bytes:
        p_type, p_offset, _va, _pa, p_filesz = phdrs[i][:5]
        if is_self and seginfo[i][2] == 2:
            off, length, _ = seginfo[i]
            out = zlib.decompress(data[off:off + length])
            if len(out) != p_filesz:
                raise ModuleError(f"segment {i}: inflated {len(out)} bytes, expected {p_filesz}")
            return out
        base = header_len if is_self else 0
        return data[base + p_offset:base + p_offset + p_filesz]

    segments: list[Segment] = []
    relas: list[bytes] = []
    exidx = None
    for i, (p_type, _off, vaddr, _pa, filesz, memsz, flags, _align) in enumerate(phdrs):
        if p_type == PT_LOAD and memsz:
            buf = bytearray(seg_bytes(i))
            buf.extend(b"\0" * (memsz - len(buf)))
            segments.append(Segment(i, vaddr, memsz, filesz, flags, buf))
        elif p_type == PT_SCE_RELA:
            relas.append(seg_bytes(i))
        elif p_type == PT_ARM_EXIDX:
            exidx = (vaddr, vaddr + memsz)

    mod = Module(name=name, elf_type=e_type, segments=segments, exidx=exidx)
    by_index = {s.index: s for s in segments}
    for blob in relas:
        _apply_relocs(mod, blob, by_index)

    info_seg = by_index.get(e_entry >> 30)
    if info_seg is None:
        raise ModuleError("module_info segment missing")
    info = info_seg.vaddr + (e_entry & 0x3FFFFFFF)
    mod.info_addr = info
    _parse_module_info(mod, info, info_seg)
    return mod


def _apply_relocs(mod: Module, blob: bytes, segs: dict[int, Segment]) -> None:
    def base(i: int) -> int:
        if i == 0xF:
            return 0
        if i not in segs:
            raise ModuleError(f"relocation references missing segment {i}")
        return segs[i].vaddr

    def patch(code: int, p: int, s: int, a: int) -> None:
        seg = mod.seg_of(p)
        if seg is None:
            raise ModuleError(f"relocation patches unmapped address {p:#x}")
        off = p - seg.vaddr
        cur = struct.unpack_from("<I", seg.data, off)[0]
        v = (s + a) & 0xFFFFFFFF
        if code in (0, 40, 255):
            return
        if code in (2, 38):
            new = v
        elif code == 8:
            seg.data[off] = v & 0xFF
            mod.relocs.append(Reloc(p, code, v))
            return
        elif code in (3, 41):
            new = (v - p) & 0xFFFFFFFF
        elif code == 42:
            new = (v - p) & 0x7FFFFFFF
        elif code in (28, 29):
            new = (cur & 0xFF000000) | (((v - p) >> 2) & 0xFFFFFF)
        elif code == 10:
            d = (v - p) & 0xFFFFFFFF
            hi, lo = cur & 0xFFFF, cur >> 16
            sign = (d >> 24) & 1
            hi = (hi & ~0x7FF) | ((d >> 12) & 0x3FF) | (sign << 10)
            j1 = sign ^ ((~d >> 23) & 1)
            j2 = sign ^ ((~d >> 22) & 1)
            lo = (lo & ~0x2FFF) | ((d >> 1) & 0x7FF) | (j2 << 11) | (j1 << 13)
            new = hi | (lo << 16)
        elif code in (43, 44):
            imm = v & 0xFFFF if code == 43 else v >> 16
            new = (cur & ~0x000F0FFF) | (imm & 0xFFF) | ((imm >> 12) << 16)
        elif code in (47, 48):
            imm = v & 0xFFFF if code == 47 else v >> 16
            hi, lo = cur & 0xFFFF, cur >> 16
            hi = (hi & ~0x040F) | ((imm >> 12) & 0xF) | (((imm >> 11) & 1) << 10)
            lo = (lo & ~0x70FF) | (imm & 0xFF) | (((imm >> 8) & 7) << 12)
            new = hi | (lo << 16)
        else:
            return  # Vita3K ignores unknown codes too
        struct.pack_into("<I", seg.data, off, new & 0xFFFFFFFF)
        mod.relocs.append(Reloc(p, code, v))

    def orig_target(p: int) -> tuple[int, int]:
        val = mod.u32(p)
        for s in segs.values():
            if s.vaddr <= val < s.vaddr + s.memsz:
                return s.vaddr, val - s.vaddr
        raise ModuleError(f"format 6-9 relocation at {p:#x}: value {val:#x} not in any segment")

    g_addr = g_off = g_patchseg = g_s = g_a = g_t = g_t2 = 0
    pos = 0
    while pos < len(blob):
        w0 = struct.unpack_from("<I", blob, pos)[0]
        fmt = w0 & 0xF
        if fmt == 0:
            a, off = struct.unpack_from("<II", blob, pos + 4)
            symseg, code, pseg, code2, dist2 = _bits(w0, 4, 8), _bits(w0, 8, 16), _bits(w0, 16, 20), _bits(w0, 20, 28), _bits(w0, 28, 32)
            s, p = base(symseg), base(pseg) + off
            patch(code, p, s, a)
            if code2:
                patch(code2, p + dist2 * 2, s, a)
            g_addr, g_off, g_patchseg, g_s, g_a, g_t, g_t2 = base(pseg), off, pseg, s, a, code, code2
            pos += 12
        elif fmt == 1:
            v = struct.unpack_from("<Q", blob, pos)[0]
            symseg, code, pseg = _bits(v, 4, 8), _bits(v, 8, 16), _bits(v, 16, 20)
            off = _bits(v, 20, 32) | (_bits(v, 32, 42) << 12)
            a = _bits(v, 42, 64)
            s, p = base(symseg), base(pseg) + off
            patch(code, p, s, a)
            g_addr, g_off, g_patchseg, g_s, g_a, g_t, g_t2 = base(pseg), off, pseg, s, a, code, 0
            pos += 8
        elif fmt == 2:
            a = struct.unpack_from("<I", blob, pos + 4)[0]
            symseg, code, off = _bits(w0, 4, 8), _bits(w0, 8, 16), _bits(w0, 16, 32)
            g_off += off
            g_s, g_a, g_t, g_t2 = base(symseg), a, code, 0
            patch(g_t, g_addr + g_off, g_s, g_a)
            pos += 8
        elif fmt == 3:
            v = struct.unpack_from("<Q", blob, pos)[0]
            symseg, mode, off, dist2, a = _bits(v, 4, 8), _bits(v, 8, 9), _bits(v, 9, 27), _bits(v, 27, 32), _bits(v, 32, 54)
            g_t, g_t2 = (47, 48) if mode else (43, 44)
            g_off += off
            g_s, g_a = base(symseg), a
            p = g_addr + g_off
            patch(g_t, p, g_s, g_a)
            patch(g_t2, p + dist2, g_s, g_a)
            pos += 8
        elif fmt == 4:
            off, dist2 = _bits(w0, 4, 27), _bits(w0, 27, 32)
            g_off += off
            p = g_addr + g_off
            patch(g_t, p, g_s, g_a)
            patch(g_t2, p + dist2, g_s, g_a)
            pos += 4
        elif fmt == 5:
            d1, d2, d3, d4 = _bits(w0, 4, 13), _bits(w0, 13, 18), _bits(w0, 18, 27), _bits(w0, 27, 32)
            g_off += d1
            p = g_addr + g_off
            patch(g_t, p, g_s, g_a)
            patch(g_t2, p + d2, g_s, g_a)
            g_off += d3
            p = g_addr + g_off
            patch(g_t, p, g_s, g_a)
            patch(g_t2, p + d4, g_s, g_a)
            pos += 4
        elif fmt in (6, 7, 8, 9):
            if fmt == 6:
                steps = [_bits(w0, 4, 32)]
            else:
                bitsize = {7: 7, 8: 4, 9: 2}[fmt]
                offsets, steps = _bits(w0, 4, 32), []
                while True:
                    steps.append((offsets & ((1 << bitsize) - 1)) * 4)
                    offsets >>= bitsize
                    if not offsets:
                        break
            for step in steps:
                g_off += step
                p = base(g_patchseg) + g_off
                g_s, a = orig_target(p)
                g_t, g_t2 = 2, 0
                patch(2, g_addr + g_off, g_s, a)
            pos += 4
        else:
            raise ModuleError(f"unknown relocation format {fmt} at blob offset {pos}")


def _parse_module_info(mod: Module, info: int, seg: Segment) -> None:
    raw = mod.read(info, 0x5C)
    (attr, ver) = struct.unpack_from("<HH", raw, 0)
    mod.name = raw[4:31].split(b"\0", 1)[0].decode("latin-1") or mod.name
    (gp, exp_top, exp_end, imp_top, imp_end, mod_nid, tls_start, tls_filesz, tls_memsz,
     mstart, mstop, exidx_top, exidx_end, _extab_top, _extab_end) = struct.unpack_from("<15I", raw, 0x20)
    segbase = seg.vaddr
    if mstart not in (0, 0xFFFFFFFF):
        mod.module_start = segbase + mstart
    if mstop not in (0, 0xFFFFFFFF):
        mod.module_stop = segbase + mstop
    if tls_start:
        mod.tls = (segbase + tls_start, tls_filesz, tls_memsz)

    a = segbase + exp_top
    while a < segbase + exp_end:
        size = mod.read(a, 1)[0]
        (_sz, _r, _ver, flags, nfunc, nvar, ntls, lib_nid, lib_name, nid_tab, ent_tab) = struct.unpack(
            "<BBHHHIIIIII", mod.read(a, 0x20))
        lib = mod.cstr(lib_name) if lib_name else ("<main>" if flags & 0x8000 else "")
        for k in range(nfunc + nvar + ntls):
            kind = "func" if k < nfunc else ("var" if k < nfunc + nvar else "tls")
            mod.exports.append(Export(lib, lib_nid, mod.u32(nid_tab + 4 * k), mod.u32(ent_tab + 4 * k), kind))
        a += size or 0x20

    a = segbase + imp_top
    while a < segbase + imp_end:
        size = mod.read(a, 1)[0]
        if size == 0x34:
            (_s, _r, _v, _f, nfunc, nvar, ntls, _r1, lib_nid, lib_name, _r2, fnids, fents, vnids, vents,
             tnids, tents) = struct.unpack("<BBHHHHHIIIIIIIIII", mod.read(a, 0x34))
        elif size == 0x24:
            (_s, _r, _v, _f, nfunc, nvar, ntls, lib_nid, lib_name, fnids, fents, vnids, vents) = struct.unpack(
                "<BBHHHHHIIIIII", mod.read(a, 0x24))
            tnids = tents = 0
        else:
            raise ModuleError(f"unknown import table size {size:#x} at {a:#x}")
        lib = mod.cstr(lib_name) if lib_name else ""
        for k in range(nfunc):
            mod.imports.append(Import(lib, lib_nid, mod.u32(fnids + 4 * k), mod.u32(fents + 4 * k), "func"))
        for k in range(nvar):
            mod.imports.append(Import(lib, lib_nid, mod.u32(vnids + 4 * k), mod.u32(vents + 4 * k), "var"))
        for k in range(ntls):
            mod.imports.append(Import(lib, lib_nid, mod.u32(tnids + 4 * k), mod.u32(tents + 4 * k), "tls"))
        a += size

    # sceModuleInfo guarda module_start/stop SEM o bit Thumb; o export do mesmo endereço o tem
    # (no God of War: info=0x810983cc, export=0x810983cd, e o código só decodifica em Thumb).
    by_addr = {e.addr & ~1: e.addr for e in mod.exports if e.kind == "func"}
    if mod.module_start is not None:
        mod.module_start = by_addr.get(mod.module_start, mod.module_start)
    if mod.module_stop is not None:
        mod.module_stop = by_addr.get(mod.module_stop, mod.module_stop)
