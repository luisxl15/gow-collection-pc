"""Minimal ELF32 little-endian reader (sections, segments, symbols).

Kept dependency-free on purpose: the recompiler must run anywhere Python runs.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


class ElfError(Exception):
    pass


@dataclass
class Section:
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    entsize: int

    SHF_WRITE = 1
    SHF_ALLOC = 2
    SHF_EXECINSTR = 4
    SHT_NOBITS = 8

    @property
    def is_exec(self) -> bool:
        return bool(self.flags & self.SHF_EXECINSTR)

    @property
    def is_alloc(self) -> bool:
        return bool(self.flags & self.SHF_ALLOC)


@dataclass
class Segment:
    type: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int  # STT_*
    bind: int
    shndx: int

    STT_FUNC = 2


@dataclass
class Elf:
    data: bytes
    machine: int
    entry: int
    flags: int
    sections: list[Section] = field(default_factory=list)
    segments: list[Segment] = field(default_factory=list)
    symbols: list[Symbol] = field(default_factory=list)

    EM_MIPS = 8

    def section(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def read(self, addr: int, size: int) -> bytes:
        """Read initialised bytes at a virtual address via PT_LOAD segments."""
        for seg in self.segments:
            if seg.type == 1 and seg.vaddr <= addr and addr + size <= seg.vaddr + seg.filesz:
                off = seg.offset + (addr - seg.vaddr)
                return self.data[off:off + size]
        raise ElfError(f"address {addr:#x} not backed by file data")

    def word(self, addr: int) -> int:
        return struct.unpack_from("<I", self.read(addr, 4))[0]


def parse(data: bytes) -> Elf:
    if data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file")
    if data[4] != 1:
        raise ElfError("only ELF32 is supported")
    if data[5] != 1:
        raise ElfError("only little-endian ELF is supported (big-endian guests: TODO)")
    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, e_flags, _ehsize, e_phentsize,
     e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    elf = Elf(data=data, machine=e_machine, entry=e_entry, flags=e_flags)

    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _pa, p_filesz, p_memsz, p_flags, _al = struct.unpack_from(
            "<IIIIIIII", data, e_phoff + i * e_phentsize)
        elf.segments.append(Segment(p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags))

    raw = []
    for i in range(e_shnum):
        raw.append(struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize))
    strtab = raw[e_shstrndx] if e_shnum else None

    def cstr(off: int) -> str:
        end = data.index(b"\0", off)
        return data[off:end].decode("utf-8", "replace")

    for sh in raw:
        name = cstr(strtab[4] + sh[0]) if strtab else ""
        elf.sections.append(Section(name, sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[9]))

    for sec in elf.sections:
        if sec.type != 2:  # SHT_SYMTAB
            continue
        names = elf.sections[sec.link]
        for off in range(sec.offset, sec.offset + sec.size, 16):
            st_name, st_value, st_size, st_info, _other, st_shndx = struct.unpack_from(
                "<IIIBBH", data, off)
            elf.symbols.append(Symbol(cstr(names.offset + st_name), st_value, st_size,
                                      st_info & 0xF, st_info >> 4, st_shndx))
    return elf
