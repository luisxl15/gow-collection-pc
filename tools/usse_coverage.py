"""Decodifica todos os programas GXP de uma captura do vitart e mede a cobertura do decodificador.

    python tools/usse_coverage.py <dir da captura> [--dis HASH]
"""
import collections
import glob
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
from vitarecomp.usse.decode import Program  # noqa: E402


def programs(d):
    seen = {}
    for path in sorted(glob.glob(os.path.join(d, "mem_*.bin"))):
        buf = open(path, "rb").read()
        pos = buf.find(b"GXP\0")
        while pos >= 0:
            size = struct.unpack_from("<I", buf, pos + 8)[0]
            if 0x60 <= size < 0x100000 and pos + size <= len(buf):
                blob = buf[pos:pos + size]
                h = hashlib.sha1(blob).hexdigest()[:12]
                if h not in seen:
                    try:
                        seen[h] = Program.parse(blob)
                    except Exception:
                        pass
            pos = buf.find(b"GXP\0", pos + 4)
    return seen


def main():
    progs = programs(sys.argv[1])
    if "--dis" in sys.argv:
        p = progs[sys.argv[sys.argv.index("--dis") + 1]]
        print("fragmento" if p.fragment else "vértice", [q["name"] for q in p.params])
        print("-- secundário (uma vez por desenho)")
        for i in p.secondary: print(i)
        print("-- primário")
        for i in p.primary: print(i)
        return
    ops = collections.Counter()
    total = unknown = 0
    nv = sum(not p.fragment for p in progs.values())
    for p in progs.values():
        for i in p.primary + p.secondary:
            ops[i.op] += 1
            total += 1
            unknown += i.op == "?"
    print("%d programas (%d de vértice, %d de fragmento), %d instruções, %d sem classe (%.3f%%)" % (
        len(progs), nv, len(progs) - nv, total, unknown, 100.0 * unknown / max(total, 1)))
    for op, n in ops.most_common():
        print("  %-10s %8d" % (op, n))


if __name__ == "__main__":
    main()
