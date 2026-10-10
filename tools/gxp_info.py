"""Lista os programas GXP (shaders do Vita) presentes numa captura do vitart e seus parâmetros.

    python tools/gxp_info.py <dir da captura>

Procura a assinatura "GXP\\0" nos mem_*.bin. Cabeçalho do SceGxmProgram: +0 "GXP\\0", +4 versão
(u8 maior, u8 menor), +8 tamanho total, +0x10 tipo (bit 0: 1 = fragmento), +0x24 número de parâmetros,
+0x28 deslocamento dos parâmetros (relativo ao próprio campo). Parâmetro (16 bytes): +0 nome (relativo
ao parâmetro), +4 categoria:4 tipo:4 componentes:4 contêiner:4, +6 semântica, +8 tamanho do array,
+12 índice de recurso.
"""
import glob
import hashlib
import os
import struct
import sys

CATEGORY = {0: "attribute", 1: "uniform", 2: "sampler", 3: "aux_surface", 4: "uniform_buffer"}
TYPE = {0: "f32", 1: "f16", 2: "c10", 3: "u32", 4: "s32", 5: "u16", 6: "s16", 7: "u8", 8: "s8", 9: "aggregate"}


def cstr(buf, off):
    end = buf.find(b"\0", off)
    return buf[off:end].decode("latin-1") if end >= 0 else "?"


def parse(buf, off):
    size = struct.unpack_from("<I", buf, off + 8)[0]
    if size < 0x40 or off + size > len(buf):
        return None
    ptype = struct.unpack_from("<B", buf, off + 0x10)[0]
    count = struct.unpack_from("<I", buf, off + 0x24)[0]
    if count > 512:
        return None
    poff = off + 0x28 + struct.unpack_from("<I", buf, off + 0x28)[0]
    params = []
    for i in range(count):
        p = poff + 16 * i
        name = cstr(buf, p + struct.unpack_from("<I", buf, p)[0])
        b4, b5, sem, semi = buf[p + 4], buf[p + 5], buf[p + 6], buf[p + 7]
        arr, res = struct.unpack_from("<II", buf, p + 8)
        params.append(dict(name=name, cat=CATEGORY.get(b4 & 15, b4 & 15), type=TYPE.get(b4 >> 4, b4 >> 4),
                           comps=b5 & 15, container=b5 >> 4, semantic=sem, sem_index=semi & 15, array=arr, res=res))
    body = bytes(buf[off:off + size])
    return dict(size=size, fragment=bool(ptype & 1), params=params, hash=hashlib.sha1(body).hexdigest()[:12])


def main():
    d = sys.argv[1]
    seen = {}
    for path in sorted(glob.glob(os.path.join(d, "mem_*.bin"))):
        buf = open(path, "rb").read()
        base = int(os.path.basename(path)[4:12], 16)
        pos = buf.find(b"GXP\0")
        while pos >= 0:
            info = parse(buf, pos)
            if info and info["hash"] not in seen:
                info["addr"] = base + pos
                seen[info["hash"]] = info
            pos = buf.find(b"GXP\0", pos + 4)
    vs = [p for p in seen.values() if not p["fragment"]]
    fs = [p for p in seen.values() if p["fragment"]]
    print("%d programas distintos: %d de vértice, %d de fragmento" % (len(seen), len(vs), len(fs)))
    for p in sorted(seen.values(), key=lambda p: (p["fragment"], p["addr"])):
        names = ", ".join("%s%s:%s%s" % (q["name"], "[%d]" % q["array"] if q["array"] > 1 else "", q["cat"][0], q["comps"])
                          for q in p["params"])
        print("%s %08x %5d B %s  %s" % ("FS" if p["fragment"] else "VS", p["addr"], p["size"], p["hash"], names))


if __name__ == "__main__":
    main()
