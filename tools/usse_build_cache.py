"""Gera o cache de SPIR-V que o vitart carrega ao vivo (o produto não roda Python).

    python tools/usse_build_cache.py <saída> <captura ou dir com .gxp> [...] [--d3d11] [--jobs N]

Cada programa GXP vira <saída>/<fnv1a64 do binário>.spv; os fragmentos com textura difusa ganham também
<hash>.pbr.spv (variante PBR, ver docs/pbr.md). As fontes são capturas do vitart (memória
com os programas carregados) e os .gxp que o vitart grava em <saída>/miss quando um desenho usa um
programa sem cache. O hash é FNV-1a de 64 bits sobre os `size` bytes do programa (campo +8), igual ao
do runtime (hle_gxm.cpp).

--d3d11: também <hash>.dx11 e <hash>.pbr.dx11 (DXBC shader model 5.0 para o renderizador Direct3D 11, via
tools/usse_d3d.py), em N processos (padrão: os núcleos da CPU). Só refaz o que falta.
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
from usse_coverage import programs  # noqa: E402
from vitarecomp.usse.decode import Program  # noqa: E402
from vitarecomp.usse.pbr import diffuse_resource  # noqa: E402
from vitarecomp.usse.translate import translate  # noqa: E402


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def d3d_job(job):
    """Processo filho: (hash, blob, pbr, caminho) -> None se ok, senão a mensagem. Sem blob (o .gxp de origem não
    existe mais), o DXBC sai do SPIR-V do Vulkan já no cache (usse_d3d.dxbc_from_vulkan_spirv)."""
    h, blob, pbr, path = job
    import usse_d3d
    try:
        if blob is None:
            code = usse_d3d.dxbc_from_vulkan_spirv(open(path[:-5] + ".spv", "rb").read())
        else:
            code = usse_d3d.dxbc(Program.parse(blob), pbr=pbr)
    except Exception as e:  # noqa: BLE001 - programa fora da cobertura fica sem DXBC
        return "%016x%s: %s" % (h, ".pbr" if pbr else "", e)
    open(path, "wb").write(code)
    return None


def build_d3d11(out, blobs, jobs):
    from multiprocessing import Pool
    work = []
    for h, blob in sorted(blobs.items()):
        if not os.path.exists(os.path.join(out, "%016x.spv" % h)):
            continue  # sem tradução para o Vulkan também não sai para o D3D
        for pbr in (False, True):
            if pbr and not os.path.exists(os.path.join(out, "%016x.pbr.spv" % h)):
                continue
            path = os.path.join(out, "%016x%s.dx11" % (h, ".pbr" if pbr else ""))
            if not os.path.exists(path):
                work.append((h, blob, pbr, path))
    for name in sorted(os.listdir(out)):  # só o SPIR-V (de capturas antigas)
        if name.endswith(".spv") and not os.path.exists(os.path.join(out, name[:-4] + ".dx11")):
            stem = name[:-4]
            h = int(stem.split(".")[0], 16)
            if h not in blobs:
                work.append((h, None, stem.endswith(".pbr"), os.path.join(out, stem + ".dx11")))
    print("Direct3D 11: %d shaders a compilar em %d processos" % (len(work), jobs))
    failed = 0
    with Pool(jobs) as pool:
        for k, err in enumerate(pool.imap_unordered(d3d_job, work, chunksize=8)):
            if err:
                failed += 1
                print("  " + err)
            if (k + 1) % 500 == 0:
                print("  %d/%d" % (k + 1, len(work)), flush=True)
    print("Direct3D 11: %d novos, %d sem DXBC -> %s" % (len(work) - failed, failed, out))


def main():
    args = sys.argv[1:]
    d3d11 = "--d3d11" in args
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else (os.cpu_count() or 2)
    if "--jobs" in args:
        del args[args.index("--jobs"):args.index("--jobs") + 2]
    args = [a for a in args if a != "--d3d11"]
    out = args[0]
    os.makedirs(out, exist_ok=True)
    blobs = {}
    for src in args[1:]:
        if glob.glob(os.path.join(src, "mem_*.bin")):
            for p in programs(src).values():
                blobs[fnv1a64(p.blob)] = p.blob
        for path in glob.glob(os.path.join(src, "*.gxp")):
            blob = open(path, "rb").read()
            size = struct.unpack_from("<I", blob, 8)[0]
            blobs[fnv1a64(blob[:size])] = blob[:size]
    done = skipped = failed = pbr = 0
    for h, blob in sorted(blobs.items()):
        path = os.path.join(out, "%016x.spv" % h)
        pbr_path = os.path.join(out, "%016x.pbr.spv" % h)
        prog = Program.parse(blob)
        want_pbr = diffuse_resource(prog) is not None and not os.path.exists(pbr_path)
        if os.path.exists(path) and not want_pbr:
            skipped += 1
            continue
        try:
            if not os.path.exists(path):
                spv = translate(prog)  # traduz antes de abrir: falha não deixa arquivo vazio no cache
                open(path, "wb").write(spv)
                done += 1
            # Variante PBR do fragmento (vitarecomp/usse/pbr.py), usada quando a textura tem material.
            if want_pbr:
                spv = translate(prog, pbr=True)
                open(pbr_path, "wb").write(spv)
                pbr += 1
        except Exception as e:  # noqa: BLE001 - programa fora da cobertura fica sem cache
            failed += 1
            print("  %016x: %s" % (h, e))
            continue
    print("%d programas: %d novos, %d variantes PBR novas, %d já no cache, %d sem tradução -> %s" % (
        len(blobs), done, pbr, skipped, failed, out))
    if d3d11:
        build_d3d11(out, blobs, jobs)


if __name__ == "__main__":
    main()
