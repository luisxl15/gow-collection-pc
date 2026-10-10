"""Teste do construtor SPIR-V: gera o esqueleto de vertex shader do tradutor USSE e valida com o
spirv-val do Vulkan SDK.

O esqueleto tem a mesma forma que os shaders traduzidos terão:
  * atributos (location 0..n-1, vec4) -> banco PA;
  * buffer de armazenamento (set 0, binding 0) com os uniforms -> banco SA;
  * corpo (aqui, um vmad de exemplo: o = pa * sa + sa);
  * banco OUTPUT -> gl_Position.

    python tools/usse_spirv_smoke.py [saída.spv]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
from vitarecomp.usse import spirv as S  # noqa: E402


def build(n_attrs=2, sa_words=64):
    b = S.Builder()
    f32, i32 = b.f32(), b.i32()
    v4 = b.vec(f32, 4)
    regs = S.RegisterFile(b)
    # entradas
    attrs = []
    for k in range(n_attrs):
        a = b.variable(v4, S.SCInput, "attr%d" % k)
        b.decorate(a, S.DecLocation, k)
        attrs.append(a)
    rta = b.runtime_array(f32, 4)
    ub_t = b.struct("ub", rta)
    b.decorate(ub_t, S.DecBlock)
    b.member_decorate(ub_t, 0, S.DecOffset, 0)
    ub = b.variable(ub_t, S.SCStorageBuffer, "uniforms")
    b.decorate(ub, S.DecDescriptorSet, 0)
    b.decorate(ub, S.DecBinding, 0)
    b.decorate(ub, S.DecNonWritable)
    # saída gl_Position (bloco gl_PerVertex simplificado: só a posição)
    pv_t = b.struct("pv", v4)
    b.decorate(pv_t, S.DecBlock)
    b.member_decorate(pv_t, 0, S.DecBuiltIn, S.BuiltInPosition)
    pv = b.variable(pv_t, S.SCOutput, "gl_PerVertex")

    main = b.id()
    b.body += b.inst(S.OpFunction, b.void(), main, 0, b.fn(b.void()))
    b.body += b.inst(S.OpLabel, b.id())
    # PA <- atributos
    for k, a in enumerate(attrs):
        v = b.load(v4, a)
        for c in range(4):
            regs.write("pa", 4 * k + c, b.emit(S.OpCompositeExtract, f32, v, c))
    # SA <- uniforms
    for w in range(sa_words):
        p = b.chain(S.SCStorageBuffer, f32, ub, b.const(i32, 0), b.const(i32, w))
        regs.write("sa", w, b.load(f32, p))
    # corpo de exemplo: o[c] = pa[c] * sa[c] + sa[4 + c]  (forma de um vmad)
    for c in range(4):
        m = b.emit(S.OpFMul, f32, regs.read("pa", c), regs.read("sa", c))
        regs.write("out", c, b.emit(S.OpFAdd, f32, m, regs.read("sa", 4 + c)))
    # OUTPUT -> gl_Position
    pos = b.emit(S.OpCompositeConstruct, v4, *[regs.read("out", c) for c in range(4)])
    b.store(b.chain(S.SCOutput, v4, pv, b.const(i32, 0)), pos)
    b.body += b.inst(S.OpReturn) + b.inst(S.OpFunctionEnd)
    return b.finish(S.ExecVertex, main, attrs + [pv])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "usse_smoke.spv"
    open(out, "wb").write(build())
    val = os.path.join(os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\latest"), "Bin", "spirv-val")
    r = subprocess.run([val, "--target-env", "vulkan1.1", out], capture_output=True, text=True)
    print("spirv-val:", "OK" if r.returncode == 0 else "FALHOU", r.stdout, r.stderr)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
