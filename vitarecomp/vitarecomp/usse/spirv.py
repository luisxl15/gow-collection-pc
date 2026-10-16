"""Construtor mínimo de módulos SPIR-V (binário) para o tradutor USSE.

Modelo do tradutor: os bancos de registradores do USSE viram arrays Private de float de 32 bits
(PA, SA, TEMP, OUTPUT, FPINTERNAL); cada instrução vira loads/stores de elementos. As entradas
são preenchidas no início do main: atributos em PA, uniforms e endereços em SA. As saídas vêm do
banco OUTPUT. Assim a semântica de cada instrução é a mesma do interpretador de referência.
"""
import struct

MAGIC = 0x07230203

# opcodes usados
OpName, OpMemberName, OpExtInstImport, OpExtInst = 5, 6, 11, 12
OpMemoryModel, OpEntryPoint, OpExecutionMode, OpCapability = 14, 15, 16, 17
OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector = 19, 20, 21, 22, 23
OpTypeArray, OpTypeRuntimeArray, OpTypeStruct, OpTypePointer, OpTypeFunction = 28, 29, 30, 32, 33
OpConstant, OpConstantComposite = 43, 44
OpFunction, OpFunctionEnd, OpVariable, OpLoad, OpStore, OpAccessChain = 54, 56, 59, 61, 62, 65
OpDecorate, OpMemberDecorate = 71, 72
OpCompositeExtract, OpCompositeConstruct = 81, 80
OpBitcast = 124
OpFAdd, OpFSub, OpFMul, OpFDiv, OpIAdd, OpIMul = 129, 131, 133, 136, 128, 132
OpLabel, OpReturn = 248, 253
OpTypeImage, OpTypeSampledImage, OpVectorShuffle = 25, 27, 79
OpImageSampleImplicitLod, OpImageSampleProjImplicitLod = 87, 91

# enums
CapShader = 1
AddrLogical, MemGLSL450 = 0, 1
ExecVertex, ExecFragment = 0, 4
SCUniformConstant, SCInput, SCUniform, SCOutput, SCPrivate, SCFunction, SCStorageBuffer = 0, 1, 2, 3, 6, 7, 12
DecBlock, DecBuiltIn, DecLocation, DecBinding, DecDescriptorSet, DecOffset, DecArrayStride, DecNonWritable = 2, 11, 30, 33, 34, 35, 6, 24
BuiltInPosition, BuiltInFragCoord = 0, 15
Dim2D, DimCube = 1, 3
ExecOriginUpperLeft = 7


def _str_words(s):
    b = s.encode() + b"\0"
    b += b"\0" * (-len(b) % 4)
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


class Builder:
    def __init__(self):
        self.bound = 1
        self.caps, self.ext, self.entry, self.modes, self.names, self.decos = [], [], [], [], [], []
        self.types = []  # tipos, constantes e variáveis globais (ordem importa)
        self.body = []
        self._tcache = {}

    def id(self):
        i = self.bound
        self.bound += 1
        return i

    @staticmethod
    def inst(op, *words):
        return [((len(words) + 1) << 16) | op, *words]

    def _type(self, key, op, *args):
        if key not in self._tcache:
            i = self.id()
            self.types += self.inst(op, i, *args)
            self._tcache[key] = i
        return self._tcache[key]

    # ---------------------------------------------------------------- tipos
    def void(self): return self._type("void", OpTypeVoid)
    def f32(self): return self._type("f32", OpTypeFloat, 32)
    def u32(self): return self._type("u32", OpTypeInt, 32, 0)
    def i32(self): return self._type("i32", OpTypeInt, 32, 1)
    def vec(self, t, n): return self._type(("vec", t, n), OpTypeVector, t, n)
    def ptr(self, sc, t): return self._type(("ptr", sc, t), OpTypePointer, sc, t)
    def fn(self, ret): return self._type(("fn", ret), OpTypeFunction, ret)

    def sampled_image(self, dim=Dim2D):
        img = self._type(("img", dim), OpTypeImage, self.f32(), dim, 0, 0, 0, 1, 0)
        return self._type(("simg", dim), OpTypeSampledImage, img)

    def const(self, t, v):
        key = ("c", t, v)
        if key not in self._tcache:
            i = self.id()
            word = struct.unpack("<I", struct.pack("<f", v))[0] if t == self.f32() else v & 0xFFFFFFFF
            self.types += self.inst(OpConstant, t, i, word)
            self._tcache[key] = i
        return self._tcache[key]

    def array(self, t, n, stride=None):
        a = self._type(("arr", t, n, stride), OpTypeArray, t, self.const(self.u32(), n))
        if stride:
            self.decorate(a, DecArrayStride, stride)
        return a

    def runtime_array(self, t, stride):
        key = ("rta", t, stride)
        new = key not in self._tcache
        a = self._type(key, OpTypeRuntimeArray, t)
        if new:  # o tipo é reaproveitado: a decoração só pode existir uma vez
            self.decorate(a, DecArrayStride, stride)
        return a

    def struct(self, key, *members):
        return self._type(("st", key), OpTypeStruct, *members)

    # -------------------------------------------------------------- globais
    def variable(self, t, sc, name=None):
        i = self.id()
        self.types += self.inst(OpVariable, self.ptr(sc, t), i, sc)
        if name:
            self.name(i, name)
        return i

    def name(self, i, s): self.names += self.inst(OpName, i, *_str_words(s))
    def decorate(self, i, *d): self.decos += self.inst(OpDecorate, i, *d)
    def member_decorate(self, i, m, *d): self.decos += self.inst(OpMemberDecorate, i, m, *d)

    # ---------------------------------------------------------------- corpo
    def emit(self, op, rtype, *args):
        i = self.id()
        self.body += self.inst(op, rtype, i, *args)
        return i

    def emit_void(self, op, *args): self.body += self.inst(op, *args)

    def load(self, t, p): return self.emit(OpLoad, t, p)
    def store(self, p, v): self.emit_void(OpStore, p, v)
    def chain(self, sc, t, base, *idx): return self.emit(OpAccessChain, self.ptr(sc, t), base, *idx)

    # --------------------------------------------------------------- módulo
    def finish(self, model, main, interfaces, frag=False):
        self.caps = self.inst(OpCapability, CapShader)
        mm = self.inst(OpMemoryModel, AddrLogical, MemGLSL450)
        self.entry = self.inst(OpEntryPoint, model, main, *_str_words("main"), *interfaces)
        if frag:
            self.modes = self.inst(OpExecutionMode, main, ExecOriginUpperLeft)
        words = self.caps + self.ext + mm + self.entry + self.modes + self.names + self.decos + self.types + self.body
        return struct.pack("<%dI" % (5 + len(words)), MAGIC, 0x00010300, 0, self.bound, 0, *words)


class RegisterFile:
    """Bancos de registradores do USSE como arrays Private de float (um elemento = 32 bits)."""
    SIZES = {"pa": 128, "sa": 256, "temp": 128, "out": 64, "fpi": 32}

    def __init__(self, b):
        self.b = b
        self.vars = {k: b.variable(b.array(b.f32(), n), SCPrivate, k) for k, n in self.SIZES.items()}

    def ptr(self, bank, n):
        b = self.b
        return b.chain(SCPrivate, b.f32(), self.vars[bank], b.const(b.i32(), n))

    def read(self, bank, n): return self.b.load(self.b.f32(), self.ptr(bank, n))
    def write(self, bank, n, v): self.b.store(self.ptr(bank, n), v)
