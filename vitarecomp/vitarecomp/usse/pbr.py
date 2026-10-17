"""Variante PBR dos fragment shaders traduzidos (iluminação por pixel sobre o resultado original).

O GoW calcula a luz por vértice, em espaço de câmera: g_Transforms (mundo x câmera) leva posição e
normal para a câmera, g_LightDir/g_LightColor (2 luzes direcionais) e g_Ambient entram num N.L, e o
resultado chega ao fragmento como cor interpolada. O fragmento só multiplica a textura por essa cor.

A variante PBR mantém o shader original inteiro e, no fim, refaz a iluminação por pixel:
- posição em espaço de câmera reconstruída de gl_FragCoord (x, y, profundidade) com a inversa de
  g_Projection, que o runtime já combina com o viewport (params[0..15]);
- normal geométrica pelas derivadas da posição e cotangent frame (Schüler) para o normal map, sem
  tangentes nos vértices;
- a luz "assada" do jogo (cor original / textura) é preservada: o normal map só a modula pela razão
  entre a luz das mesmas lâmpadas na normal mapeada e na normal geométrica, e as facetas se cancelam;
- especular GGX (Smith correlacionado, Fresnel de Schlick) com as luzes reais do desenho e um
  especular de ambiente analítico (céu/chão) escurecido pela luz assada do local;
- a mistura final com a cor original usa o alfa do mapa ORM (peso do material).

Contrato com o renderizador (gxm_vk.cpp), além do contrato do translate.py:
  set 3: binding 0 = normal map (tangente: R = +U, G = +V da textura, isto é, para baixo na imagem;
         o padrão "DirectX"), binding 1 = ORM (R = oclusão, G = rugosidade, B = metal, A = peso);
  set 4: binding 0 = storage buffer de floats por desenho (layout em PARAMS abaixo).
Os mesmos amostradores e UV da textura difusa (sTextureSampler) valem para os dois mapas.
"""
from . import spirv as S

OpFNegate, OpFAdd, OpFSub, OpFMul, OpFDiv = 127, 129, 131, 133, 136
OpSelect, OpFOrdEqual, OpFOrdLessThan = 169, 180, 184
OpDPdx, OpDPdy = 207, 208
G_FABS, G_POW, G_SQRT, G_INVSQRT, G_FMIN, G_FMAX, G_FCLAMP, G_FMIX, G_SMOOTHSTEP, G_EXP2 = 4, 26, 31, 32, 37, 40, 43, 46, 49, 29

# Layout do buffer de parâmetros (índice do float). O runtime preenche (hle_gxm.cpp, pbr_params).
PARAMS = dict(
    screen_to_view=0,  # mat4 coluna a coluna: (fragx, fragy, fragz, 1) -> câmera (homogêneo)
    l0_dir=16, l0_col=20,  # luz 0: direção PARA a luz em espaço de câmera; cor
    l1_dir=24, l1_col=28,  # luz 1
    ambient=32,            # rgb
    tune_a=36,             # x força do normal map, y especular, z especular de ambiente, w razão máx. da luz assada
    tune_b=40,             # x modo de depuração, y multiplicador de rugosidade, z de metal, w força da oclusão
    sky=44, ground=48,     # cores do ambiente (lineares)
    up=52,                 # "para cima" em espaço de câmera
)
PARAM_FLOATS = 56
PBR_SETS = (3, 4)
DIFFUSE_SAMPLER = "sTextureSampler"


def diffuse_resource(prog):
    """Recurso (binding) da textura difusa, ou None se o programa não serve para a variante PBR."""
    if not prog.fragment:
        return None
    res = [q["resource"] for q in prog.params if q["category"] == 2 and q["name"] == DIFFUSE_SAMPLER]
    if not res:
        return None
    for inp in prog.frag_inputs or []:
        if inp["kind"] == "tex" and inp["sampler"] == res[0] and inp["proj"] is None and inp["coord"] <= 9:
            return res[0]
    return None


class V:
    """Vetor de 1 a 4 componentes float (ids SPIR-V). Os operadores emitem as instruções."""

    def __init__(self, m, ids):
        self.m, self.c = m, list(ids)

    def __len__(self):
        return len(self.c)

    def __getattr__(self, name):
        if name and set(name) <= set("xyzw") and len(name) <= 4:
            return V(self.m, [self.c["xyzw".index(ch)] for ch in name])
        raise AttributeError(name)

    def _bin(self, other, op, rev=False):
        a, b = self, self.m.lift(other)
        if rev:
            a, b = b, a
        return self.m.zipmap(lambda x, y: self.m.emit(op, x, y), a, b)

    def __add__(self, o): return self._bin(o, OpFAdd)
    def __radd__(self, o): return self._bin(o, OpFAdd, True)
    def __sub__(self, o): return self._bin(o, OpFSub)
    def __rsub__(self, o): return self._bin(o, OpFSub, True)
    def __mul__(self, o): return self._bin(o, OpFMul)
    def __rmul__(self, o): return self._bin(o, OpFMul, True)
    def __truediv__(self, o): return self._bin(o, OpFDiv)
    def __rtruediv__(self, o): return self._bin(o, OpFDiv, True)
    def __neg__(self): return V(self.m, [self.m.emit(OpFNegate, x) for x in self.c])


class Math:
    """Funções vetoriais sobre o Builder do tradutor (escalares por componente, como o resto do tradutor)."""

    def __init__(self, tr):
        self.tr, self.b, self.f32 = tr, tr.b, tr.f32

    def emit(self, op, *args):
        return self.b.emit(op, self.f32, *args)

    def const(self, *vals):
        return V(self, [self.tr.const(v) for v in vals])

    def lift(self, o):
        return o if isinstance(o, V) else self.const(o)

    def vec(self, *parts):
        ids = []
        for p in parts:
            ids += self.lift(p).c
        return V(self, ids)

    def zipmap(self, fn, *vs):
        n = max(len(v) for v in vs)
        comps = [v.c if len(v) == n else v.c * n if len(v) == 1 else None for v in vs]
        if any(c is None for c in comps):
            raise ValueError("tamanhos incompatíveis")
        return V(self, [fn(*args) for args in zip(*comps)])

    def ext(self, code, *vs):
        vs = [self.lift(v) for v in vs]
        return self.zipmap(lambda *a: self.b.emit(S.OpExtInst, self.f32, self.tr.glsl, code, *a), *vs)

    def dot(self, a, b):
        p = a * b
        acc = V(self, p.c[:1])
        for k in range(1, len(p)):
            acc = acc + V(self, [p.c[k]])
        return acc

    def rsqrt(self, x): return self.ext(G_INVSQRT, x)
    def sqrt(self, x): return self.ext(G_SQRT, x)
    def max(self, a, b): return self.ext(G_FMAX, a, b)
    def min(self, a, b): return self.ext(G_FMIN, a, b)
    def clamp(self, x, lo, hi): return self.ext(G_FCLAMP, x, lo, hi)
    def saturate(self, x): return self.clamp(x, 0.0, 1.0)
    def mix(self, a, b, t): return self.ext(G_FMIX, a, b, t)
    def pow(self, a, b): return self.ext(G_POW, a, b)
    def smoothstep(self, e0, e1, x): return self.ext(G_SMOOTHSTEP, e0, e1, x)
    def exp2(self, x): return self.ext(G_EXP2, x)

    def normalize(self, v):
        return v * self.rsqrt(self.max(self.dot(v, v), 1e-30))

    def cross(self, a, b):
        return self.vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)

    def dfdx(self, v): return V(self, [self.emit(OpDPdx, x) for x in v.c])
    def dfdy(self, v): return V(self, [self.emit(OpDPdy, x) for x in v.c])

    def less(self, a, b):
        return self.b.emit(OpFOrdLessThan, self.tr.boolt, self.lift(a).c[0], self.lift(b).c[0])

    def equal(self, a, b):
        return self.b.emit(OpFOrdEqual, self.tr.boolt, self.lift(a).c[0], self.lift(b).c[0])

    def select(self, cond, a, b):
        return self.zipmap(lambda x, y: self.emit(OpSelect, cond, x, y), self.lift(a), self.lift(b))

    def luminance(self, c):
        return self.dot(c, self.const(0.2126, 0.7152, 0.0722))

    def srgb_to_linear(self, c):
        return self.pow(self.max(c, 0.0), 2.2)

    def linear_to_srgb(self, c):
        return self.pow(self.max(c, 0.0), 1.0 / 2.2)

    def from_vec4(self, vid, n=4):
        return V(self, [self.b.emit(S.OpCompositeExtract, self.f32, vid, k) for k in range(n)])

    def sample(self, var, uv):
        b = self.b
        img = b.load(b.sampled_image(), var)
        coord = b.emit(S.OpCompositeConstruct, self.tr.v2, *uv.c[:2])
        return self.from_vec4(b.emit(S.OpImageSampleImplicitLod, self.tr.v4, img, coord))


class PbrEmitter:
    """Prólogo (derivadas e mapas, em fluxo uniforme, antes de qualquer kill) e epílogo (a luz)."""

    def __init__(self, tr, ifaces, fragcoord_var, texel, coord):
        self.tr, self.b, self.m = tr, tr.b, Math(tr)
        b = self.b
        self.texel, self.coord = texel, coord
        self.fragcoord = fragcoord_var
        simg = b.sampled_image()
        self.normal_map = b.variable(simg, S.SCUniformConstant, "pbr_normal")
        b.decorate(self.normal_map, S.DecDescriptorSet, PBR_SETS[0])
        b.decorate(self.normal_map, S.DecBinding, 0)
        self.orm_map = b.variable(simg, S.SCUniformConstant, "pbr_orm")
        b.decorate(self.orm_map, S.DecDescriptorSet, PBR_SETS[0])
        b.decorate(self.orm_map, S.DecBinding, 1)
        rta = b.runtime_array(tr.f32, 4)
        st = b.struct("pbr_params", rta)
        b.decorate(st, S.DecBlock)
        b.member_decorate(st, 0, S.DecOffset, 0)
        self.params = b.variable(st, S.SCStorageBuffer, "pbr_params")
        b.decorate(self.params, S.DecDescriptorSet, PBR_SETS[1])
        b.decorate(self.params, S.DecBinding, 0)
        b.decorate(self.params, S.DecNonWritable)

    def param(self, name, n=1, k=0):
        b, tr = self.b, self.tr
        base = PARAMS[name] + k
        ids = []
        for i in range(n):
            ptr = b.chain(S.SCStorageBuffer, tr.f32, self.params, b.const(tr.i32, 0), b.const(tr.i32, base + i))
            ids.append(b.load(tr.f32, ptr))
        return V(self.m, ids)

    def prologue(self):
        m = self.m
        fc = m.from_vec4(self.b.load(self.tr.v4, self.fragcoord), 3)
        # Posição em espaço de câmera: params[0..15] já leva o pixel (com escala e meio pixel do
        # viewport) ao NDC do GXM e o NDC à câmera.
        cols = [self.param("screen_to_view", 4, 4 * j) for j in range(4)]
        ph = cols[0] * fc.x + cols[1] * fc.y + cols[2] * fc.z + cols[3]
        self.P = ph.xyz / ph.w
        self.uv = m.from_vec4(self.coord, 2)
        dp1, dp2 = m.dfdx(self.P), m.dfdy(self.P)
        duv1, duv2 = m.dfdx(self.uv), m.dfdy(self.uv)
        ng = m.normalize(m.cross(dp1, dp2))
        # Cotangent frame com a normal crua (não orientada): T e B saem como dP/du e dP/dv qualquer
        # que seja o sentido do eixo y da tela.
        dp2perp, dp1perp = m.cross(dp2, ng), m.cross(ng, dp1)
        t = dp2perp * duv1.x + dp1perp * duv2.x
        bt = dp2perp * duv1.y + dp1perp * duv2.y
        inv = m.rsqrt(m.max(m.max(m.dot(t, t), m.dot(bt, bt)), 1e-30))
        self.T, self.B = t * inv, bt * inv
        self.V = m.normalize(-self.P)
        self.Ng = m.select(m.less(m.dot(ng, self.V), 0.0), -ng, ng)
        self.nmap = m.sample(self.normal_map, self.uv)
        self.orm = m.sample(self.orm_map, self.uv)

    def epilogue(self, color_id):
        m = self.m
        c = m.from_vec4(color_id)
        texel = m.from_vec4(self.texel)
        tune_a, tune_b = self.param("tune_a", 4), self.param("tune_b", 4)

        tn = self.nmap.xyz * 2.0 - 1.0
        nxy = tn.xy * tune_a.x
        N = m.normalize(self.T * nxy.x + self.B * nxy.y + self.Ng * tn.z)
        Ng, V = self.Ng, self.V

        ao = m.mix(1.0, self.orm.x, tune_b.w)
        rough = m.clamp(self.orm.y * tune_b.y, 0.045, 1.0)
        metal = m.saturate(self.orm.z * tune_b.z)
        weight = self.orm.w

        albedo = m.srgb_to_linear(texel.xyz)
        orig = m.srgb_to_linear(m.saturate(c.xyz))
        baked = m.min(orig / m.max(albedo, 0.02), tune_a.w)

        lights = [(m.normalize(self.param("l0_dir", 3)), self.param("l0_col", 3)),
                  (m.normalize(self.param("l1_dir", 3)), self.param("l1_col", 3))]
        amb = self.param("ambient", 3)

        # Luz das mesmas lâmpadas na normal geométrica e na mapeada: a razão é o detalhe do normal map.
        lit_geom, lit_map = amb, amb
        for L, col in lights:
            lit_geom = lit_geom + col * m.max(m.dot(Ng, L), 0.0)
            lit_map = lit_map + col * m.max(m.dot(N, L), 0.0)
        detail = m.clamp((m.luminance(lit_map) + 0.05) / (m.luminance(lit_geom) + 0.05), 0.0, 2.5)
        diffuse = orig * detail * ao * (1.0 - metal)

        # Especular GGX das luzes do desenho.
        a = rough * rough
        a2 = a * a
        NoV = m.max(m.dot(N, V), 1e-4)
        F0 = m.mix(m.const(0.04, 0.04, 0.04), albedo, metal)
        spec = m.const(0.0, 0.0, 0.0)
        for L, col in lights:
            H = m.normalize(V + L)
            NoL = m.max(m.dot(N, L), 0.0)
            NoH = m.max(m.dot(N, H), 0.0)
            VoH = m.max(m.dot(V, H), 0.0)
            d = NoH * NoH * (a2 - 1.0) + 1.0
            D = a2 / (3.14159265 * d * d)
            gv = NoL * m.sqrt(NoV * NoV * (1.0 - a2) + a2)
            gl = NoV * m.sqrt(NoL * NoL * (1.0 - a2) + a2)
            vis = 0.5 / (gv + gl + 1e-5)
            f5 = m.pow(m.max(1.0 - VoH, 0.0), 5.0)
            F = F0 + (1.0 - F0) * f5
            spec = spec + F * (D * vis * NoL) * col
        spec = spec * tune_a.y * ao

        # Ambiente analítico (Karis, "Physically Based Shading on Mobile").
        R = N * (2.0 * NoV) - V
        up = self.param("up", 3)
        sky, ground = self.param("sky", 3), self.param("ground", 3)
        env = m.mix(ground, sky, m.smoothstep(-0.3, 0.3, m.dot(R, up)))
        env = m.mix(env, (sky + ground) * 0.5, rough)
        r = rough * m.const(-1.0, -0.0275, -0.572, 0.022) + m.const(1.0, 0.0425, 1.04, -0.04)
        a004 = m.min(r.x * r.x, m.exp2(NoV * -9.28)) * r.x + r.y
        env_brdf = F0 * (a004 * -1.04 + r.z) + (a004 * 1.04 + r.w)
        local = m.clamp(m.luminance(baked), 0.0, 1.5)
        env_spec = env * env_brdf * (local * ao * tune_a.z)

        lin = diffuse + spec + env_spec
        rgb = m.mix(c.xyz, m.saturate(m.linear_to_srgb(lin)), weight)

        # Depuração (tune_b.x): 1 normal, 2 luz assada, 3 rugosidade, 4 metal, 5 oclusão, 6 posição, 7 detalhe.
        views = [N * 0.5 + 0.5, m.saturate(m.linear_to_srgb(baked * 0.5)), m.vec(rough, rough, rough),
                 m.vec(metal, metal, metal), m.vec(ao, ao, ao), self.P * 0.01 - m.ext(8, self.P * 0.01),
                 m.vec(detail, detail, detail) * 0.5]
        for k, v in enumerate(views, 1):
            rgb = m.select(m.equal(tune_b.x, float(k)), v, rgb)
        return self.b.emit(S.OpCompositeConstruct, self.tr.v4, *(rgb.c + c.c[3:4]))
