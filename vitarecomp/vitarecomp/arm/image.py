"""Guest image abstraction for the ARM recompiler.

Two sources produce the same shape:
  * a Linux static ARM ELF (differential-test guests run under qemu-arm)
  * a PS Vita module (vitarecomp.module.Module): eboot.bin / .suprx
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


@dataclass
class Seg:
    addr: int
    data: bytes
    memsz: int
    exec: bool
    write: bool


@dataclass
class GuestImage:
    segments: list[Seg]
    entry: int                  # with bit0 = Thumb
    symbols: dict[int, str] = field(default_factory=dict)   # addr (bit0 = Thumb) -> name
    sizes: dict[int, int] = field(default_factory=dict)     # addr (bit0 = Thumb) -> size, when known
    code_ptrs: set[int] = field(default_factory=set)        # addresses (bit0 = Thumb) seen as pointers
    imports: dict[int, tuple[str, int, int]] = field(default_factory=dict)  # stub addr -> (name, lib_nid, nid)
    kind: str = "linux"

    def seg_of(self, a: int) -> Seg | None:
        return next((s for s in self.segments if s.addr <= a < s.addr + s.memsz), None)

    def is_code(self, a: int) -> bool:
        s = self.seg_of(a & ~1)
        return bool(s and s.exec and (a & ~1) < s.addr + len(s.data))

    def read(self, a: int, n: int) -> bytes:
        s = self.seg_of(a)
        if s is None:
            raise ValueError(f"unmapped {a:#x}")
        off = a - s.addr
        return bytes(s.data[off:off + n]).ljust(n, b"\0")

    def u8(self, a: int) -> int:
        return self.read(a, 1)[0]

    def u16(self, a: int) -> int:
        return struct.unpack("<H", self.read(a, 2))[0]

    def u32(self, a: int) -> int:
        return struct.unpack("<I", self.read(a, 4))[0]


def from_linux_elf(data: bytes) -> GuestImage:
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("expected ELF32 little-endian")
    e_type, e_machine, _v, e_entry, e_phoff, e_shoff, _fl, _eh, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = \
        struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_machine != 40:
        raise ValueError("not an ARM ELF")
    segs = []
    for i in range(e_phnum):
        p_type, off, va, _pa, fs, ms, flags, _al = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        if p_type == 1 and ms:
            segs.append(Seg(va, data[off:off + fs], ms, bool(flags & 1), bool(flags & 2)))
    img = GuestImage(segs, e_entry)
    # Symbols: STT_FUNC values carry the Thumb bit already.
    shdrs = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    for sh in shdrs:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = shdrs[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, info, _o, shndx = struct.unpack_from("<IIIBBH", data, off)
            if info & 0xF == 2 and value:
                end = data.index(b"\0", strtab[4] + name_off)
                img.symbols[value] = data[strtab[4] + name_off:end].decode()
                if size:
                    img.sizes[value] = size
    # Word-aligned values in data segments that point into code are candidate code pointers.
    for s in segs:
        if s.exec:
            continue
        for k in range(0, len(s.data) - 3, 4):
            v = struct.unpack_from("<I", s.data, k)[0]
            if img.is_code(v):
                img.code_ptrs.add(v)
    return img


def from_vita_module(mod) -> GuestImage:
    """Build a GuestImage from vitarecomp.module.Module (relocated at link-time base)."""
    segs = [Seg(s.vaddr, bytes(s.data[:s.filesz]), s.memsz, s.executable, bool(s.flags & 2)) for s in mod.segments]
    img = GuestImage(segs, mod.module_start or 0, kind="vita")
    for e in mod.exports:
        if e.kind == "func":
            img.symbols[e.addr] = e.name or f"export_{e.nid:08x}"
    if mod.module_start:
        img.symbols.setdefault(mod.module_start, "module_start")
    if mod.module_stop:
        img.symbols.setdefault(mod.module_stop, "module_stop")
    # ABS32/TARGET1 relocations whose target lies in code are exact code-pointer evidence.
    for r in mod.relocs:
        if r.code in (2, 38) and img.is_code(r.target):
            img.code_ptrs.add(r.target)
    for imp in mod.imports:
        if imp.kind == "func":
            img.imports[imp.stub_addr] = (imp.name or f"nid_{imp.nid:08x}", imp.library_nid, imp.nid)
    return img
