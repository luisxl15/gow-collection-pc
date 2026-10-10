"""Rasteriza no CPU um quadro capturado pelo vitart (draws.jsonl + draws.bin + memória).

    python tools/gxm_softraster.py <dir da captura> [saída.png]

Prova da infraestrutura do renderizador (busca de vértices, índices, uniforms por nome, viewport,
texturas, blend, profundidade), NÃO um renderizador fiel: os shaders USSE do jogo ainda não são
traduzidos, então cada desenho usa um shader genérico escolhido pela assinatura de parâmetros do
GXP (g_Projection, g_ViewProjection*g_World, posição já em clip; textura x cor x g_Color).
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import gxm_tex_dump as tx  # noqa: E402

ATTR_FMT = {0: ("u1", False), 1: ("i1", False), 2: ("<u2", False), 3: ("<i2", False), 4: ("u1", True), 5: ("i1", True),
            6: ("<u2", True), 7: ("<i2", True), 8: ("<f2", False), 9: ("<f4", False)}


def gxp_params(mem, prog):
    hdr = mem.read(prog, 0x40).tobytes()
    count = struct.unpack_from("<I", hdr, 0x24)[0]
    poff = prog + 0x28 + struct.unpack_from("<I", hdr, 0x28)[0]
    raw = mem.read(poff, 16 * count).tobytes()
    out = []
    for i in range(count):
        noff, b4, b5, sem, semi, arr, res = struct.unpack_from("<IBBBBII", raw, 16 * i)
        name = mem.read(poff + 16 * i + noff, 64).tobytes().split(b"\0")[0].decode("latin-1")
        out.append(dict(name=name, cat=b4 & 15, comps=b5 & 15, array=arr, res=res))
    return out


def uniform(blob, params, name, n):
    for p in params:
        if p["cat"] == 1 and p["name"] == name and blob is not None:
            off = 4 * p["res"]
            if off + 4 * n <= len(blob):
                return np.frombuffer(blob[off:off + 4 * n], "<f4").astype(np.float64)
    return None


class Raster:
    def __init__(self, d):
        self.d = d
        self.mem = tx.Memory(d)
        self.bin = open(os.path.join(d, "draws.bin"), "rb").read()
        self.targets = {}  # endereço -> (rgba float, depth)
        self.texcache = {}
        self.stats = {}

    def blob(self, off, size):
        return self.bin[off:off + size] if size else None

    def target(self, cs):
        fmt, w, h, stride, addr = cs
        if addr not in self.targets:
            self.targets[addr] = [np.zeros((h, w, 4), np.float32), np.ones((h, w), np.float32), (w, h, stride)]
        return self.targets[addr]

    def texture(self, words):
        t = tx.describe(words)
        key = (t["data"], t["width"], t["height"], t["base"])
        if t["data"] in self.targets:  # textura que é um alvo já desenhado neste quadro
            return self.targets[t["data"]][0]
        if key not in self.texcache:
            img = None
            try:
                img = tx.decode(self.mem, t)
            except Exception:
                pass
            self.texcache[key] = None if img is None else img.astype(np.float32) / 255.0
        return self.texcache[key]

    def draw(self, dr):
        vparams = gxp_params(self.mem, dr["vp"])
        fparams = gxp_params(self.mem, dr["fp"]) if dr["fp"] else []
        idx = np.frombuffer(self.bin[dr["ioff"]:dr["ioff"] + dr["icount"] * dr["isz"]], "<u2" if dr["isz"] == 2 else "<u4").astype(np.int64)
        nv = dr["maxi"] + 1
        attrs = {}
        by_res = {p["res"]: p["name"] for p in vparams if p["cat"] == 0}
        for st_i, off, fmt, comps, reg in dr["attrs"]:
            st = dr["streams"][st_i]
            if not st["size"]:
                continue
            dt, norm = ATTR_FMT.get(fmt, ("<f4", False))
            esz = np.dtype(dt).itemsize
            buf = np.frombuffer(self.bin, np.uint8, st["size"], st["off"])
            rows = np.zeros((nv, comps * esz), np.uint8)
            for v in range(nv):
                b = v * st["stride"] + off
                if b + comps * esz <= len(buf):
                    rows[v] = buf[b:b + comps * esz]
            val = rows.view(dt).reshape(nv, comps).astype(np.float64)
            if norm:
                val /= float(np.iinfo(np.dtype(dt)).max)
            name = by_res.get(reg, "reg%d" % reg).replace("input.", "")
            attrs[name] = val

        pos = attrs.get("position", attrs.get("posAndWeight"))
        if pos is None:
            self.stats["sem posição"] = self.stats.get("sem posição", 0) + 1
            return
        p4 = np.ones((nv, 4))
        p4[:, :pos.shape[1]] = pos
        if pos.shape[1] < 4 or "posAndWeight" in attrs:
            p4[:, 3] = 1.0
        if pos.shape[1] < 3:
            p4[:, 2] = 0.0
        vu = self.blob(*dr["vu"])
        names = {p["name"] for p in vparams}
        kind = "clip"
        clip = p4
        proj = uniform(vu, vparams, "g_Projection", 16)
        vp_m = uniform(vu, vparams, "g_ViewProjection", 16)
        world = uniform(vu, vparams, "g_World", 16)
        if "g_Transforms" in names:
            tr = uniform(vu, vparams, "g_Transforms", 4 * 64)
            if tr is not None and proj is not None:
                # Ossos 4x4 em convenção de vetor-linha (16 floats cada; o shader lê 16 palavras com
                # vldst e faz x*L0 + y*L1 + z*L2 + L3), dois ossos misturados como A + w*(B - A).
                bones = tr[:len(tr) // 16 * 16].reshape(-1, 4, 4)
                ph = np.c_[p4[:, :3], np.ones(nv)]
                if "indices" in attrs:
                    kind = "skin"
                    ind = attrs["indices"].astype(np.int64) % len(bones)
                    wgt = pos[:, 3:4] if pos.shape[1] > 3 else np.ones((nv, 1))
                    a = np.einsum("nj,nji->ni", ph, bones[ind[:, 0]])
                    b = np.einsum("nj,nji->ni", ph, bones[ind[:, 1]])
                    wp = (a + wgt * (b - a))[:, :3]
                else:
                    kind = "rigid"
                    wp = (ph @ bones[0])[:, :3]
                # Medido: projeção aplicada como coluna (P @ v), 82.637 de 170.719 vértices na tela contra 12.078.
                clip = np.c_[wp, np.ones(nv)] @ proj.reshape(4, 4).T
        elif vp_m is not None and world is not None:
            kind = "world"
            clip = (p4 @ world.reshape(4, 4)) @ vp_m.reshape(4, 4).T
        elif proj is not None:
            kind = "proj"
            clip = p4 @ proj.reshape(4, 4).T
        self.stats[kind] = self.stats.get(kind, 0) + 1

        # cor por vértice e coordenada de textura
        col = np.ones((nv, 4))
        if "color" in attrs or "vertexColor" in attrs:
            c = attrs.get("color", attrs.get("vertexColor"))
            if c.max() > 1.5:
                c = c / 255.0
            col[:, :c.shape[1]] = c
        g_color = uniform(vu, vparams, "g_Color", 4)
        if g_color is None:
            g_color = uniform(vu, vparams, "g_MaterialColor", 4)
        if g_color is not None:
            col *= g_color
        uv = attrs.get("texUV", attrs.get("uv", attrs.get("texcoord")))
        tex = None
        if any(p["cat"] == 2 for p in fparams) and dr["tex"]:
            tex = self.texture(dr["tex"][0][1:5])

        color, depth, (w, h, _) = self.target(dr["cs"])
        xo, xs, yo, ys, zo, zs = struct.unpack("<6f", struct.pack("<6I", *dr["vps"]))
        wcl = clip[:, 3]
        wcl = np.where(np.abs(wcl) < 1e-6, 1e-6, wcl)
        sx = xo + xs * clip[:, 0] / wcl
        sy = yo + ys * clip[:, 1] / wcl
        sz = zo + zs * clip[:, 2] / wcl
        tris = self.triangles(dr["prim"], idx)
        blend = dr["blend"] if dr["hasblend"] else None
        dfunc = (dr["depth"][0] >> 22) & 7
        dwrite = dr["depth"][1] == 0
        for a, b, c in tris:
            if min(wcl[a], wcl[b], wcl[c]) <= 0:
                continue
            self.tri(color, depth, w, h, (sx[[a, b, c]], sy[[a, b, c]], sz[[a, b, c]]), col[[a, b, c]],
                     None if uv is None else uv[[a, b, c], :2], tex, blend, dfunc, dwrite)

    @staticmethod
    def triangles(prim, idx):
        if prim == 0:
            return idx[:len(idx) // 3 * 3].reshape(-1, 3)
        if prim == 3:  # strip
            return np.array([(idx[i], idx[i + 1], idx[i + 2]) for i in range(len(idx) - 2)], np.int64).reshape(-1, 3)
        if prim == 4:  # fan
            return np.array([(idx[0], idx[i], idx[i + 1]) for i in range(1, len(idx) - 1)], np.int64).reshape(-1, 3)
        return np.zeros((0, 3), np.int64)

    def tri(self, color, depth, w, h, s, col, uv, tex, blend, dfunc, dwrite):
        x, y, z = s
        x0, x1 = max(int(np.floor(x.min())), 0), min(int(np.ceil(x.max())), w - 1)
        y0, y1 = max(int(np.floor(y.min())), 0), min(int(np.ceil(y.max())), h - 1)
        if x0 > x1 or y0 > y1:
            return
        area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
        if abs(area) < 1e-9:
            return
        py, px = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
        w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area
        w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area
        w2 = 1 - w0 - w1
        m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not m.any():
            return
        zz = w0 * z[0] + w1 * z[1] + w2 * z[2]
        dsub = depth[y0:y1 + 1, x0:x1 + 1]
        if dfunc in (1, 3):
            m &= zz <= dsub + (1e-6 if dfunc == 3 else 0)
        elif dfunc == 4 or dfunc == 6:
            m &= zz >= dsub
        if not m.any():
            return
        c = (w0[..., None] * col[0] + w1[..., None] * col[1] + w2[..., None] * col[2])
        if tex is not None and uv is not None:
            u = w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0]
            v = w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1]
            th, tw = tex.shape[:2]
            tu = np.clip((np.mod(u, 1.0) * tw).astype(np.int64), 0, tw - 1)
            tv = np.clip((np.mod(v, 1.0) * th).astype(np.int64), 0, th - 1)
            c = c * tex[tv, tu]
        c = np.clip(c, 0, 1).astype(np.float32)
        dst = color[y0:y1 + 1, x0:x1 + 1]
        if blend is not None:
            src_f, dst_f = (blend >> 16) & 15, (blend >> 20) & 15
            fs = self.factor(src_f, c, dst)
            fd = self.factor(dst_f, c, dst)
            out = np.clip(c * fs + dst * fd, 0, 1)
        else:
            out = c
        dst[m] = out[m]
        if dwrite:
            dsub[m] = zz[m]

    @staticmethod
    def factor(f, s, d):
        one = np.ones_like(s)
        sa = s[..., 3:4] * one
        da = d[..., 3:4] * one
        return {0: 0 * one, 1: one, 2: s, 3: 1 - s, 4: sa, 5: 1 - sa, 6: d, 7: 1 - d, 8: da, 9: 1 - da}.get(f, one)


def main():
    d = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(d, "frame.png")
    r = Raster(d)
    draws = [json.loads(l) for l in open(os.path.join(d, "draws.jsonl"), encoding="utf-8")]
    for i, dr in enumerate(draws):
        try:
            r.draw(dr)
        except Exception as e:  # um desenho ruim não deve esconder os outros
            r.stats["erro"] = r.stats.get("erro", 0) + 1
            print("desenho %d: %s" % (i, e))
    print("desenhos:", len(draws), r.stats)
    base = os.path.splitext(out)[0]
    for addr, (color, _, (w, h, stride)) in r.targets.items():
        img = (np.clip(color, 0, 1) * 255).astype(np.uint8)
        img[..., 3] = 255
        p = "%s_%08x.png" % (base, addr)
        Image.fromarray(img, "RGBA").save(p)
        print("alvo 0x%08x %dx%d -> %s" % (addr, w, h, p))


if __name__ == "__main__":
    main()
