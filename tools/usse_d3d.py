"""Shaders do renderizador Direct3D 11 do vitart: GXP -> SPIR-V (vitarecomp.usse, modo d3d) -> HLSL (spirv-cross
do Vulkan SDK) -> DXBC shader model 5.0 (D3DCompile do d3dcompiler_47.dll do Windows).

O contrato de recursos está em vitarecomp/usse/translate.py (D3D_*): texturas em t/s = recurso, mapas do PBR em
t/s 14 e 15, uniforms em t20/t21, parâmetros do PBR em t22 e o cbuffer b0 com as bases de cada desenho.
Usado pelo tools/usse_build_cache.py (--d3d11); sozinho, converte um .gxp para conferir:

    python tools/usse_d3d.py <programa.gxp> [saída_sem_extensão]
"""
import ctypes
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
from vitarecomp.usse.translate import translate  # noqa: E402

SPIRV_CROSS = os.path.join(os.environ.get("VULKAN_SDK", "C:/VulkanSDK/latest"), "Bin", "spirv-cross.exe")
D3DCOMPILE_OPTIMIZATION_LEVEL3 = 1 << 15

_d3dc = None


def _blob(p):
    """Conteúdo de um ID3DBlob (e o libera): vtable QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize."""
    if not p.value:
        return b""
    vt = ctypes.cast(p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(p)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(p)
    data = ctypes.string_at(ptr, size)
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(p)
    return data


def compile_hlsl(src, profile):
    """DXBC do HLSL; ValueError com a mensagem do compilador se falhar."""
    global _d3dc
    if _d3dc is None:
        _d3dc = ctypes.WinDLL("d3dcompiler_47.dll")
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = _d3dc.D3DCompile(ctypes.c_char_p(src), ctypes.c_size_t(len(src)), None, None, None, b"main",
                          profile.encode(), ctypes.c_uint(D3DCOMPILE_OPTIMIZATION_LEVEL3), 0,
                          ctypes.byref(code), ctypes.byref(err))
    out, msg = _blob(code), _blob(err)
    if hr:
        raise ValueError("D3DCompile %08x: %s" % (hr & 0xFFFFFFFF, msg.decode(errors="replace").strip()[:300]))
    return out


# O FXC dobra constantes em float com os denormais zerados, e o USSE guarda inteiros (endereços do vldst, índices)
# nos mesmos registradores que os floats: asfloat(220u) vira 0 e o vértice lê a matriz errada (tudo atrás da
# câmera). Mesmo asuint(asfloat(220u)) sai 0; só um caminho todo inteiro preserva os bits. Então os bancos de
# registradores (arrays de float Private: pa, sa, temp, ...) passam a ser arrays de uint: as cargas
# viram uint + bitcast para float, as gravações recebem os bits (a constante em uint, a origem inteira de um
# bitcast, ou um bitcast novo), e o bitcast de volta para inteiro usa a carga uint direto.
OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_ARRAY, OP_TYPE_POINTER, OP_CONSTANT = 21, 22, 28, 32, 43
OP_FUNCTION, OP_VARIABLE, OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_IB_ACCESS_CHAIN = 54, 59, 61, 62, 65, 66
OP_COPY_OBJECT, OP_BITCAST, OP_SELECT = 83, 124, 169
# Instruções sem tipo de resultado em w[1] (cabeçalho, nomes, tipos, decorações, gravações e fluxo).
_NO_RESULT_TYPE = {2, 3, 4, 5, 6, 7, 8, 10, 11, 14, 15, 16, 17} | set(range(19, 40)) | {56, 62, 63} | set(range(71, 76)) | set(range(246, 256)) |     {317, 330, 331, 332, 5632, 5633}


def uint_registers(spv):
    words = list(struct.unpack("<%dI" % (len(spv) // 4), spv[:len(spv) // 4 * 4]))
    insts = []
    i = 5
    while i < len(words):
        n = words[i] >> 16
        insts.append(words[i:i + max(n, 1)])
        i += max(n, 1)
    bound = [words[3]]

    def new_id():
        bound[0] += 1
        return bound[0] - 1

    rtype, f32, ints, u32 = {}, None, {}, None
    for w in insts:
        op = w[0] & 0xFFFF
        if op == OP_TYPE_FLOAT and w[2] == 32:
            f32 = w[1]
        elif op == OP_TYPE_INT and w[2] == 32:
            ints[w[1]] = w[3]
            if not w[3] and u32 is None:
                u32 = w[1]
        elif op not in _NO_RESULT_TYPE and len(w) > 2:
            rtype[w[2]] = w[1]
    if f32 is None:
        return spv
    arrays = {w[1]: w[3] for w in insts if w[0] & 0xFFFF == OP_TYPE_ARRAY and w[2] == f32}  # array -> comprimento
    ptrs = {w[1]: (w[2], w[3]) for w in insts if w[0] & 0xFFFF == OP_TYPE_POINTER}       # ponteiro -> (classe, tipo)
    reg_vars = {w[2] for w in insts if w[0] & 0xFFFF == OP_VARIABLE and len(w) == 4 and w[3] == 6
                and ptrs.get(w[1], (0, 0))[1] in arrays}
    # Só os arrays acessados elemento a elemento (cadeia de acesso); carga ou cópia do array inteiro: fica como está.
    for w in insts:
        op = w[0] & 0xFFFF
        if op in (OP_LOAD, OP_COPY_OBJECT) and w[3] in reg_vars:
            reg_vars.discard(w[3])
        if op == OP_STORE and w[1] in reg_vars:
            reg_vars.discard(w[1])
    if not reg_vars:
        return spv

    # Tipos novos entram logo antes da variável que os usa (os comprimentos dos arrays já foram declarados); as
    # constantes uint, que só aparecem nas funções, num bloco antes da primeira função.
    out, consts = [], []
    need_u32 = u32 is None
    if need_u32:
        u32 = new_id()
        ints[u32] = 0
    uarr, uptr, uconst = {}, {}, {}

    def u_array(arr):
        if need_u32 and not uarr:
            out.append([4 << 16 | OP_TYPE_INT, u32, 32, 0])
        if arr not in uarr:
            uarr[arr] = new_id()
            out.append([4 << 16 | OP_TYPE_ARRAY, uarr[arr], u32, arrays[arr]])
        return uarr[arr]

    def u_ptr(sc, t):
        if (sc, t) not in uptr:
            uptr[(sc, t)] = new_id()
            out.append([4 << 16 | OP_TYPE_POINTER, uptr[(sc, t)], sc, t])
        return uptr[(sc, t)]

    def u_const(bits):
        if bits not in uconst:
            uconst[bits] = new_id()
            consts.append([4 << 16 | OP_CONSTANT, u32, uconst[bits], bits])
        return uconst[bits]

    fconst = {w[2]: w[3] for w in insts if w[0] & 0xFFFF == OP_CONSTANT and w[1] == f32}
    ubits = {}     # float carregado de um registrador -> a carga uint
    from_int = {}  # float de OpBitcast de um inteiro -> o inteiro
    reg_ptrs = set(reg_vars)
    elem_ptr = {}  # classe -> ponteiro para uint, criado com o primeiro array dessa classe

    def bits_of(v, body):
        """Os bits (uint) do float v, sem passar por float quando der."""
        if v in ubits:
            return ubits[v]
        if v in fconst:
            return u_const(fconst[v])
        if v in from_int:
            x = from_int[v]
            if rtype.get(x) == u32:
                return x
            r = new_id()
            body.append([4 << 16 | OP_BITCAST, u32, r, x])
            return r
        r = new_id()
        body.append([4 << 16 | OP_BITCAST, u32, r, v])
        return r

    first_fn = None
    for w in insts:
        op = w[0] & 0xFFFF
        if op == OP_VARIABLE and w[2] in reg_vars:
            sc, arr = ptrs[w[1]]
            elem_ptr.setdefault(sc, u_ptr(sc, u32))
            out.append([w[0], u_ptr(sc, u_array(arr)), w[2], w[3]])
            continue
        if op == OP_FUNCTION and first_fn is None:
            first_fn = len(out)
        if op in (OP_ACCESS_CHAIN, OP_IB_ACCESS_CHAIN) and w[3] in reg_ptrs and ptrs.get(w[1], (0, 0))[1] == f32:
            reg_ptrs.add(w[2])
            out.append([w[0], elem_ptr[ptrs[w[1]][0]]] + w[2:])
            continue
        if op == OP_LOAD and w[3] in reg_ptrs and w[1] == f32:
            n = new_id()
            out.append([w[0], u32, n] + w[3:])
            out.append([4 << 16 | OP_BITCAST, f32, w[2], n])
            ubits[w[2]] = n
            continue
        if op == OP_STORE and w[1] in reg_ptrs:
            out.append([w[0], w[1], bits_of(w[2], out)] + w[3:])
            continue
        if op == OP_BITCAST and w[1] == f32 and rtype.get(w[3]) in ints:
            from_int[w[2]] = w[3]
        if op == OP_BITCAST and w[1] in ints and (w[3] in ubits or w[3] in fconst or w[3] in from_int):
            src = bits_of(w[3], out)
            out.append([4 << 16 | OP_COPY_OBJECT, w[1], w[2], src] if w[1] == u32 else [4 << 16 | OP_BITCAST, w[1], w[2], src])
            continue
        if (op == OP_SELECT and w[1] == f32 and all(v in ubits or v in fconst or v in from_int for v in w[4:6])):
            a, b = bits_of(w[4], out), bits_of(w[5], out)
            n = new_id()
            out.append([6 << 16 | OP_SELECT, u32, n, w[3], a, b])
            out.append([4 << 16 | OP_BITCAST, f32, w[2], n])
            ubits[w[2]] = n
            continue
        out.append(w)
    out[first_fn:first_fn] = consts
    words[3] = bound[0]
    flat = words[:5]
    for w in out:
        flat += w
    return struct.pack("<%dI" % len(flat), *flat)


def spirv_to_hlsl(spv):
    spv = uint_registers(spv)
    fd, path = tempfile.mkstemp(suffix=".spv")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(spv)
        r = subprocess.run([SPIRV_CROSS, "--hlsl", "--shader-model", "50", path], capture_output=True,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    finally:
        os.remove(path)
    if r.returncode:
        raise ValueError("spirv-cross: " + r.stderr.decode(errors="replace").strip()[:300])
    return r.stdout


def dxbc(prog, pbr=False):
    """DXBC SM 5.0 do programa (Unsupported/ValueError se não der)."""
    hlsl = spirv_to_hlsl(translate(prog, pbr=pbr, d3d=True))
    return compile_hlsl(hlsl, "ps_5_0" if prog.fragment else "vs_5_0")


# Programas que só existem como SPIR-V do Vulkan (o .gxp de origem veio de capturas que não existem mais): o HLSL
# do spirv-cross é levado ao mesmo contrato do modo d3d do tradutor (translate.py, D3D_*) por texto.
_REGISTERS = [(r"ByteAddressBuffer uniforms : register\(t\d+\)", "ByteAddressBuffer uniforms : register(t20)"),
              (r"ByteAddressBuffer memory : register\(t\d+\)", "ByteAddressBuffer memory : register(t21)"),
              (r"ByteAddressBuffer pbr_params : register\(t\d+\)", "ByteAddressBuffer pbr_params : register(t22)"),
              (r"pbr_normal : register\(t\d+\)", "pbr_normal : register(t14)"),
              (r"_pbr_normal_sampler : register\(s\d+\)", "_pbr_normal_sampler : register(s14)"),
              (r"pbr_orm : register\(t\d+\)", "pbr_orm : register(t15)"),
              (r"_pbr_orm_sampler : register\(s\d+\)", "_pbr_orm_sampler : register(s15)")]
_VARYINGS = ["color0 : TEXCOORD1", "color1 : TEXCOORD2", "fog : TEXCOORD3"] + \
    ["texcoord%d : TEXCOORD%d" % (i, 4 + i) for i in range(10)]


def _full_struct(src, name):
    """A struct de varyings com as 13 locations em ordem (e as variáveis de sistema que já tinha, no fim)."""
    m = re.search(r"struct %s\n\{\n(.*?)\n\};" % name, src, re.S)
    if not m:
        return src
    system = [l for l in m.group(1).split("\n") if ": SV_" in l]
    body = "\n".join(["    float4 %s;" % v for v in _VARYINGS] + system)
    return src[:m.start(1)] + body + src[m.end(1):]


def hlsl_from_vulkan_spirv(spv, fragment):
    s = spirv_to_hlsl(spv).decode().replace("\r\n", "\n")
    for a, b in _REGISTERS:
        s = re.sub(a, b, s)
    s = "cbuffer draw : register(b0)\n{\n    uint4 vt_base;\n};\n\n" + s
    base = "vt_base.y" if fragment else "vt_base.x"
    s = re.sub(r"\b(uniforms|memory)\.Load(\d?)\(", lambda m: "%s.Load%s(%s * 4 + " % (m.group(1), m.group(2), base), s)
    s = re.sub(r"\bpbr_params\.Load(\d?)\(", lambda m: "pbr_params.Load%s(vt_base.z * 4 + " % m.group(1), s)
    if fragment:
        s = _full_struct(s, "SPIRV_Cross_Input")
    else:
        s = _full_struct(s, "SPIRV_Cross_Output")
        s = s.replace("SPIRV_Cross_Output stage_output;", "SPIRV_Cross_Output stage_output = (SPIRV_Cross_Output)0;")
        s = s.replace("    return stage_output;", "    stage_output.gl_Position.y *= asfloat(vt_base.w);\n    return stage_output;")
    return s.encode()


def is_fragment_spirv(spv):
    """Modelo de execução do OpEntryPoint (4 = fragmento)."""
    words = struct.unpack("<%dI" % (len(spv) // 4), spv[:len(spv) // 4 * 4])
    i = 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xFFFF
        if op == 15:
            return words[i + 1] == 4
        i += max(n, 1)
    raise ValueError("SPIR-V sem OpEntryPoint")


def dxbc_from_vulkan_spirv(spv):
    fragment = is_fragment_spirv(spv)
    return compile_hlsl(hlsl_from_vulkan_spirv(spv, fragment), "ps_5_0" if fragment else "vs_5_0")


if __name__ == "__main__":
    from vitarecomp.usse.decode import Program
    blob = open(sys.argv[1], "rb").read()
    blob = blob[:struct.unpack_from("<I", blob, 8)[0]]
    prog = Program.parse(blob)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(sys.argv[1])[0]
    open(out + ".hlsl", "wb").write(spirv_to_hlsl(translate(prog, d3d=True)))
    open(out + ".dx11", "wb").write(dxbc(prog))
    print("ok:", out + ".hlsl", out + ".dx11")
