"""Empacota um quadro capturado pelo vitart para o renderizador Vulkan (tools/gxm_replay).

    python tools/gxm_pack_frame.py <dir da captura> [saída.gxmf]

Cada desenho sai pronto para a GPU: SPIR-V traduzido (vitarecomp.usse), atributos já convertidos para
vec4 float (location k = k-ésimo parâmetro de entrada do programa de vértice), índices como lista de
triângulos u32, uniforms de vértice e de fragmento, texturas decodificadas em RGBA8 e o estado (blend,
profundidade, viewport, alvo). Formato (little-endian):

    "GXMF" u32 versão u32 n_desenhos
    por desenho:
      u32 válido, prim, cull, tem_blend, blend, função_prof, escreve_prof
      u32 tem_stencil, frente[7], verso[7]  (função, falha, falha_prof, passa, máscara_cmp, máscara_escr, ref)
      f32 vps[6]                      (x_off, x_escala, y_off, y_escala, z_off, z_escala)
      u32 alvo_end, alvo_l, alvo_a, prof_end
      u32 n_vértices, n_atributos, n_índices
      f32 atributos[n_atributos][n_vértices][4]; u32 índices[n_índices]
      u32 n, bytes (uniforms de vértice); u32 n, bytes (uniforms de fragmento)
      u32 n, SPIR-V do vértice; u32 n, SPIR-V do fragmento
      u32 n_texturas; por textura: u32 binding, tipo, endereço, l, a, palavra0 [, rgba[l*a*4] se tipo 0]
        tipo 0 = pixels a seguir, 1 = alvo de cor deste quadro, 2 = superfície de profundidade
"""
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
import gxm_softraster as sr  # noqa: E402
import gxm_tex_dump as tx  # noqa: E402
from vitarecomp.usse.decode import Program  # noqa: E402
from vitarecomp.usse.translate import Unsupported, translate  # noqa: E402

VERSION = 3


def main():
    d = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(d, "frame.gxmf")
    r = sr.Raster(d)
    draws = [json.loads(line) for line in open(os.path.join(d, "draws.jsonl"))]
    progs, spv, texcache = {}, {}, {}
    targets = {dr["cs"][4] for dr in draws}
    depths = {dr["ds"] for dr in draws if dr.get("ds")}

    def program(addr):
        if addr not in progs:
            size = struct.unpack("<I", r.mem.read(addr + 8, 4).tobytes())[0]
            progs[addr] = Program.parse(r.mem.read(addr, size).tobytes())
        return progs[addr]

    def shader(addr):
        if addr not in spv:
            try:
                spv[addr] = translate(program(addr))
            except (Unsupported, Exception) as e:  # noqa: B014 - qualquer falha deixa o desenho de fora
                spv[addr] = None
                print("  programa %08x não traduzido: %s" % (addr, e))
        return spv[addr]

    stats = {"válidos": 0, "sem shader": 0}
    parts = [b"GXMF", struct.pack("<II", VERSION, len(draws))]
    for dr in draws:
        vs, fs = shader(dr["vp"]), shader(dr["fp"]) if dr["fp"] else None
        valid = vs is not None and fs is not None
        stats["válidos" if valid else "sem shader"] += 1
        vprog = program(dr["vp"])
        nv = dr["maxi"] + 1
        # atributos: location k = k-ésimo parâmetro de categoria 0, ligado pelo índice de recurso
        locs = [q["resource"] for q in vprog.params if q["category"] == 0]
        by_reg = {reg: (st_i, off, fmt, comps) for st_i, off, fmt, comps, reg in dr["attrs"]}
        attrs = np.zeros((len(locs), nv, 4), np.float32)
        for k, res in enumerate(locs):
            if res not in by_reg:
                continue
            st_i, off, fmt, comps = by_reg[res]
            st = dr["streams"][st_i]
            if not st["size"]:
                continue
            dt, norm = sr.ATTR_FMT.get(fmt, ("<f4", False))
            esz = np.dtype(dt).itemsize
            buf = np.frombuffer(r.bin, np.uint8, st["size"], st["off"])
            rows = np.zeros((nv, comps * esz), np.uint8)
            for v in range(nv):
                b = v * st["stride"] + off
                if b + comps * esz <= len(buf):
                    rows[v] = buf[b:b + comps * esz]
            val = rows.view(dt).reshape(nv, comps).astype(np.float64)
            if norm:
                val /= float(np.iinfo(np.dtype(dt)).max)
            attrs[k, :, :comps] = val
        idx = np.frombuffer(r.bin[dr["ioff"]:dr["ioff"] + dr["icount"] * dr["isz"]],
                            "<u2" if dr["isz"] == 2 else "<u4").astype(np.int64)
        tris = sr.Raster.triangles(dr["prim"], idx).reshape(-1).astype(np.uint32)
        tris = np.minimum(tris, nv - 1)
        vu = r.blob(*dr["vu"]) or b""
        # Depuração: GXM_ZERO_VU="programa_hex:palavra,palavra" zera palavras dos uniforms de vértice.
        zv = os.environ.get("GXM_ZERO_VU")
        if zv and vu:
            prog_s, words_s = zv.split(":")
            if dr["vp"] == int(prog_s, 16):
                arr = bytearray(vu)
                for w in words_s.split(","):
                    arr[4 * int(w):4 * int(w) + 4] = bytes(4)
                vu = bytes(arr)
        fu = r.blob(*dr["fu"]) or b""
        fmt_, w, h, stride, addr = dr["cs"]
        dfunc = (dr["depth"][0] >> 22) & 7
        dwrite = int(dr["depth"][1] == 0)
        ds = dr.get("ds") or 0
        parts.append(struct.pack("<7I", int(valid), dr["prim"], dr["cull"], dr["hasblend"], dr["blend"], dfunc, dwrite))
        parts.append(struct.pack("<15I", *(dr.get("st") or [0, 7, 0, 0, 0, 255, 255, 0, 7, 0, 0, 0, 255, 255, 0])))
        parts.append(struct.pack("<6I", *dr["vps"]))
        parts.append(struct.pack("<4I", addr, w, h, ds))
        parts.append(struct.pack("<3I", nv, len(locs), len(tris)))
        parts.append(attrs.tobytes())
        parts.append(tris.tobytes())
        for blob in (vu, fu, vs or b"", fs or b""):
            parts.append(struct.pack("<I", len(blob)))
            parts.append(blob)
        texs = []
        for t in dr["tex"] or []:
            unit, words = t[0], t[1:5]
            desc = tx.describe(words)
            smp = struct.pack("<I", words[0])  # palavra 0: filtros e endereçamento
            if desc["data"] in targets or desc["data"] in depths:
                kind = 1 if desc["data"] in targets else 2
                texs.append(struct.pack("<5I", unit, kind, desc["data"], desc["width"], desc["height"]) + smp)
                continue
            key = tuple(words)
            if key not in texcache:
                try:
                    img = tx.decode(r.mem, desc)
                except Exception:
                    img = None
                if img is None:
                    img = np.full((1, 1, 4), 255, np.uint8)
                img = np.ascontiguousarray(img.astype(np.uint8))
                if img.shape[2] == 3:
                    img = np.concatenate([img, np.full(img.shape[:2] + (1,), 255, np.uint8)], 2)
                texcache[key] = img
            img = texcache[key]
            texs.append(struct.pack("<5I", unit, 0, 0, img.shape[1], img.shape[0]) + smp + img.tobytes())
        parts.append(struct.pack("<I", len(texs)))
        parts.extend(texs)
    with open(out, "wb") as f:
        for p in parts:
            f.write(p)
    print("%d desenhos (%s), %d programas, %d texturas -> %s (%.1f MB)" % (
        len(draws), stats, len(spv), len(texcache), out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
