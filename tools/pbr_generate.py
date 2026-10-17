"""Gera os materiais PBR (normal map + ORM) do vitart a partir de um pack de texturas.

    python tools/pbr_generate.py <pack> <saída> [--overrides materiais.tsv] [--strength 3] [--roughness 0.75]
                                 [--jobs N] [--force]

<pack>: pasta com index.tsv (chave<TAB>png), como vitart-data/texpack/gow1 (tools/texpack_build.py).
<saída>: pasta do material, normalmente vitart-data/pbr/gow1. Grava <nome>_n.png, <nome>_orm.png e
index.tsv (chave<TAB>normal<TAB>orm) com as MESMAS chaves do pack: o runtime acha o material da textura
pela chave (runtime/vitart/pbr.h).

Os mapas saem da própria textura (não há informação de relevo no jogo): altura = luminância em duas
escalas, normal pelas derivadas da altura, oclusão pelas cavidades e rugosidade pela luminância. É um
ponto de partida; mapas feitos à mão (Substance, Materialize...) entram no mesmo index.tsv.

Convenções (iguais às do shader, vitarecomp/usse/pbr.py):
  normal: R = +U (direita), G = +V (para baixo na imagem, padrão "DirectX"), B = para fora;
          mapas no padrão OpenGL: normal_opengl=1 no pbr.ini;
  ORM:    R = oclusão, G = rugosidade, B = metal, A = peso do PBR (255 = inteiro, 0 = cor original).

--overrides: TSV com padrão<TAB>rugosidade<TAB>metal[<TAB>força do normal], aplicado pelo nome do PNG ou
pela chave da textura (fnmatch, sem diferenciar maiúsculas; a última linha que casar vence). "-" mantém o
valor gerado. Ex.:
    *blade*     0.30    1.0
    *chain*     0.40    1.0     1.5
    *skin*      0.55    0       0.6
"""
import argparse
import fnmatch
import os
import sys
from multiprocessing import Pool

import numpy as np
from PIL import Image


def box_blur(a, r):
    """Média móvel separável com borda periódica (as texturas repetem)."""
    if r < 1:
        return a
    out = a
    for axis in (0, 1):
        n = out.shape[axis]
        k = min(2 * r + 1, n)
        pad = np.concatenate([np.take(out, range(n - r, n), axis=axis), out, np.take(out, range(0, r), axis=axis)],
                             axis=axis)
        c = np.cumsum(pad, axis=axis, dtype=np.float64)
        zero = np.zeros_like(np.take(c, [0], axis=axis))
        c = np.concatenate([zero, c], axis=axis)
        out = ((np.take(c, range(k, k + n), axis=axis) - np.take(c, range(0, n), axis=axis)) / k).astype(np.float32)
    return out


def blur(a, r):
    """Três passadas de caixa ~ gaussiana de sigma ~ r."""
    for _ in range(3):
        a = box_blur(a, r)
    return a


def gradients(h):
    """Sobel com borda periódica: derivadas em x (colunas) e y (linhas, para baixo)."""
    def s(dy, dx):
        return np.roll(np.roll(h, -dy, axis=0), -dx, axis=1)
    gx = (s(-1, 1) + 2 * s(0, 1) + s(1, 1) - s(-1, -1) - 2 * s(0, -1) - s(1, -1)) / 8.0
    gy = (s(1, -1) + 2 * s(1, 0) + s(1, 1) - s(-1, -1) - 2 * s(-1, 0) - s(-1, 1)) / 8.0
    return gx, gy


def load_overrides(path):
    rules = []
    if not path:
        return rules
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\r\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        f = line.split("\t")
        val = lambda i: None if len(f) <= i or f[i].strip() in ("", "-") else float(f[i])  # noqa: E731
        rules.append((f[0].strip().lower(), val(1), val(2), val(3)))
    return rules


def material_for(names, rules, rough, strength):
    metal = 0.0
    for pat, r, m, s in rules:
        if any(fnmatch.fnmatch(n.lower(), pat) for n in names):
            rough = r if r is not None else rough
            metal = m if m is not None else metal
            strength = s if s is not None else strength
    return rough, metal, strength


def generate(job):
    src, out_n, out_orm, rough_base, metal, strength = job
    img = np.asarray(Image.open(src).convert("RGBA"), dtype=np.float32) / 255.0
    h_, w_ = img.shape[:2]
    rgb, alpha = img[..., :3], img[..., 3]
    lum = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    # Escala: o relevo acompanha a resolução (o pack HD tem detalhe mais fino por pixel).
    res = max(1.0, max(w_, h_) / 256.0)
    r_small, r_large = max(1, int(round(res))), max(2, int(round(4 * res)))
    height = 0.6 * blur(lum, r_small) + 0.4 * blur(lum, r_large)
    gx, gy = gradients(height)
    k = strength * res * 4.0
    # Normal oposta ao aclive: x para a direita (+U), y para baixo (+V), z para fora.
    n = np.stack([-gx * k, -gy * k, np.ones_like(gx)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    flat = alpha < 0.05  # sem textura visível: normal reta
    n[flat] = (0.0, 0.0, 1.0)
    normal = np.clip((n * 0.5 + 0.5) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(normal, "RGB").save(out_n, optimize=False)

    # Oclusão pelas cavidades: mais baixo que a vizinhança = mais escuro.
    cavity = np.clip(blur(height, r_large) - height, 0.0, None)
    ao = np.clip(1.0 - 3.0 * cavity, 0.55, 1.0)
    # Rugosidade: base do material, mais áspera nas partes escuras e nas cavidades.
    rough = np.clip(rough_base + 0.25 * (0.5 - lum) + 1.5 * cavity, 0.08, 1.0)
    orm = np.stack([ao, rough, np.full_like(ao, metal), np.ones_like(ao)], axis=-1)
    Image.fromarray(np.clip(orm * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(out_orm, optimize=False)
    return src


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pack")
    ap.add_argument("out")
    ap.add_argument("--overrides")
    ap.add_argument("--strength", type=float, default=3.0, help="força do relevo (padrão 3)")
    ap.add_argument("--roughness", type=float, default=0.75, help="rugosidade base (padrão 0,75)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--force", action="store_true", help="refaz mapas já gerados")
    a = ap.parse_args()

    entries = []
    for line in open(os.path.join(a.pack, "index.tsv"), encoding="utf-8"):
        line = line.rstrip("\r\n")
        if "\t" in line:
            key, png = line.split("\t", 1)
            entries.append((key.strip(), png))
    rules = load_overrides(a.overrides)
    os.makedirs(a.out, exist_ok=True)

    keys_of = {}
    for key, png in entries:
        keys_of.setdefault(png, []).append(key)
    jobs, names = {}, {}
    for key, png in entries:
        if png in names:
            continue
        stem = os.path.splitext(png.replace("/", "_").replace("\\", "_"))[0]
        n_name, o_name = stem + "_n.png", stem + "_orm.png"
        names[png] = (n_name, o_name)
        src = os.path.join(a.pack, png)
        out_n, out_o = os.path.join(a.out, n_name), os.path.join(a.out, o_name)
        if not os.path.exists(src):
            continue
        fresh = (os.path.exists(out_n) and os.path.exists(out_o) and
                 min(os.path.getmtime(out_n), os.path.getmtime(out_o)) >= os.path.getmtime(src))
        if fresh and not a.force:
            continue
        rough, metal, strength = material_for([os.path.basename(png)] + keys_of[png], rules, a.roughness, a.strength)
        jobs[png] = (src, out_n, out_o, rough, metal, strength)

    print("%d texturas no pack, %d PNGs, %d para gerar" % (len(entries), len(names), len(jobs)))
    done = 0
    with Pool(a.jobs) as pool:
        for _ in pool.imap_unordered(generate, jobs.values(), chunksize=4):
            done += 1
            if done % 100 == 0 or done == len(jobs):
                print("  %d/%d" % (done, len(jobs)), flush=True)

    written = 0
    with open(os.path.join(a.out, "index.tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# chave\tnormal\torm (gerado por tools/pbr_generate.py)\n")
        for key, png in entries:
            n_name, o_name = names[png]
            if os.path.exists(os.path.join(a.out, n_name)) and os.path.exists(os.path.join(a.out, o_name)):
                f.write("%s\t%s\t%s\n" % (key, n_name, o_name))
                written += 1
    print("index.tsv: %d texturas com material -> %s" % (written, a.out))


if __name__ == "__main__":
    sys.exit(main())
