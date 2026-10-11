"""Gera o cache de SPIR-V que o vitart carrega ao vivo (o produto não roda Python).

    python tools/usse_build_cache.py <saída> <captura ou dir com .gxp> [...]

Cada programa GXP vira <saída>/<fnv1a64 do binário>.spv. As fontes são capturas do vitart (memória
com os programas carregados) e os .gxp que o vitart grava em <saída>/miss quando um desenho usa um
programa sem cache. O hash é FNV-1a de 64 bits sobre os `size` bytes do programa (campo +8), igual ao
do runtime (hle_gxm.cpp).
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
from usse_coverage import programs  # noqa: E402
from vitarecomp.usse.decode import Program  # noqa: E402
from vitarecomp.usse.translate import translate  # noqa: E402


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    blobs = {}
    for src in sys.argv[2:]:
        if glob.glob(os.path.join(src, "mem_*.bin")):
            for p in programs(src).values():
                blobs[fnv1a64(p.blob)] = p.blob
        for path in glob.glob(os.path.join(src, "*.gxp")):
            blob = open(path, "rb").read()
            size = struct.unpack_from("<I", blob, 8)[0]
            blobs[fnv1a64(blob[:size])] = blob[:size]
    done = skipped = failed = 0
    for h, blob in sorted(blobs.items()):
        path = os.path.join(out, "%016x.spv" % h)
        if os.path.exists(path):
            skipped += 1
            continue
        try:
            spv = translate(Program.parse(blob))
        except Exception as e:  # noqa: BLE001 - programa fora da cobertura fica sem cache
            failed += 1
            print("  %016x: %s" % (h, e))
            continue
        open(path, "wb").write(spv)
        done += 1
    print("%d programas: %d novos, %d já no cache, %d sem tradução -> %s" % (len(blobs), done, skipped, failed, out))


if __name__ == "__main__":
    main()
