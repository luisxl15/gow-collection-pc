"""Loader self-check: relocating a Vita module to its link-time base must reproduce
the bytes of the ELF the linker produced (vita-elf-create only rewrites the
import stub sections .text.fstubs.*, which are excluded).

usage: python3 vitarecomp/tests/test_module.py <eboot.bin> <linked.elf> <vita-elf-create output .velf>
"""
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vitarecomp import module  # noqa: E402


def stub_ranges(velf: str):
    out = subprocess.run(["arm-none-eabi-readelf", "-SW", velf], capture_output=True, text=True).stdout
    ranges = []
    for line in out.splitlines():
        parts = line.split()
        if any(p.startswith(".text.fstubs") for p in parts):
            i = next(k for k, p in enumerate(parts) if p.startswith(".text.fstubs"))
            addr, size = int(parts[i + 2], 16), int(parts[i + 4], 16)
            ranges.append((addr, addr + size))
    return ranges


def main() -> int:
    eboot, elf_path, velf = sys.argv[1:4]
    mod = module.load(Path(eboot).read_bytes())
    elf = Path(elf_path).read_bytes()
    skip = stub_ranges(velf)
    phoff, = struct.unpack_from("<I", elf, 28)
    phnum, = struct.unpack_from("<H", elf, 44)
    bad = checked = 0
    for i in range(phnum):
        t, off, va, _pa, fs, *_ = struct.unpack_from("<8I", elf, phoff + 32 * i)
        if t != 1:
            continue
        seg = mod.seg_of(va)
        for k in range(fs):
            a = va + k
            if any(s <= a < e for s, e in skip):
                continue
            checked += 1
            if elf[off + k] != seg.data[a - seg.vaddr]:
                bad += 1
                if bad <= 5:
                    print(f"mismatch at {a:#x}")
    print(f"{checked} bytes comparados, {bad} divergentes; {len(mod.relocs)} relocacoes aplicadas")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
