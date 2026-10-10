"""Traduz para SPIR-V todos os programas GXP de uma captura que o tradutor já cobre e valida cada
um com o spirv-val (vulkan1.1).

    python tools/usse_translate_all.py <dir da captura> [dir de saída]
"""
import collections
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
from usse_coverage import programs  # noqa: E402
from vitarecomp.usse.translate import Unsupported, translate  # noqa: E402

VAL = os.path.join(os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\latest"), "Bin", "spirv-val")


def main():
    progs = programs(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(sys.argv[1], "spv")
    os.makedirs(out, exist_ok=True)
    missing = collections.Counter()
    ok = bad = 0
    first_err = None
    for h, p in progs.items():
        try:
            spv = translate(p)
        except Unsupported as e:
            missing[str(e)] += 1
            continue
        path = os.path.join(out, "%s.%s.spv" % (h, "frag" if p.fragment else "vert"))
        open(path, "wb").write(spv)
        r = subprocess.run([VAL, "--target-env", "vulkan1.1", path], capture_output=True, text=True)
        if r.returncode == 0:
            ok += 1
        else:
            bad += 1
            first_err = first_err or (path, (r.stdout + r.stderr)[:400])
    print("%d programas: %d traduzidos e válidos, %d inválidos, %d não cobertos" % (
        len(progs), ok, bad, sum(missing.values())))
    print("primeira instrução não coberta, por programa:", missing.most_common(8))
    if first_err:
        print("primeiro inválido:", first_err)


if __name__ == "__main__":
    main()
