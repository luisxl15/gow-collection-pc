"""Teste de GPU da variante PBR: shader real do jogo + caminho de parâmetros do runtime x referência numpy.

    python tests/pbr_gpu/run.py [programa.gxp]

Precisa de Linux com Vulkan (o lavapipe do Mesa serve: apt install mesa-vulkan-drivers), g++,
glslangValidator e libvulkan-dev. Monta um plano inclinado em espaço de câmera com projeção em
perspectiva no formato do GoW (g_Projection em colunas, z de 0 a 1), duas luzes e mapas constantes
(normal inclinada, ORM), desenha com pbr_gpu.cpp e confere, pixel a pixel no interior do plano:
  modo 1 (normal mapeada), modo 6 (posição reconstruída) e modo 0 (cor final) contra pbr.py em numpy.
"""
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "vitarecomp"))
from vitarecomp.usse.decode import Program  # noqa: E402
from vitarecomp.usse.translate import translate  # noqa: E402

RS = int(os.environ.get("PBR_RS", "1"))  # escala interna (como VITART_VK_SCALE)
W, H = 720 * RS, 408 * RS
NEAR, FAR, FOV = 0.5, 100.0, np.radians(60)
# Plano (paralelogramo) em espaço de câmera, câmera olhando para -z; UV nos cantos.
V00, V10, V01 = np.array([-3.0, -1.2, -3.0]), np.array([3.0, -1.2, -3.0]), np.array([-3.0, 1.0, -9.0])
DU, DV = V10 - V00, V01 - V00
LIGHTS = [(np.array([0.3, 0.8, 0.5]), np.array([0.8, 0.75, 0.7])), (np.array([-0.6, -0.2, 0.4]), np.array([0.2, 0.25, 0.35]))]
AMB = np.array([0.15, 0.15, 0.18])
VCOLOR = np.array([0.6, 0.5, 0.4])  # texcoord1: luz por vértice
DIFFUSE = np.array([180, 120, 90, 255], np.uint8)
TN = np.array([0.4, -0.3, 1.0]) / np.linalg.norm([0.4, -0.3, 1.0])
NORMAL = np.append(np.clip(np.round((TN * 0.5 + 0.5) * 255), 0, 255), 255).astype(np.uint8)
ORM = np.array([230, 128, 40, 255], np.uint8)


def projection():
    f = 1.0 / np.tan(FOV / 2)
    a = W / H
    cols = [[f / a, 0, 0, 0], [0, f, 0, 0], [0, 0, FAR / (NEAR - FAR), -1], [0, 0, NEAR * FAR / (NEAR - FAR), 0]]
    return np.array(cols, dtype=np.float64).T  # matriz (linha, coluna); as colunas vão para g_Projection


def vertex_glsl(P):
    corners = [V00, V10, V01, V00 + DU + DV]
    uvs = [(0, 0), (1, 0), (0, 1), (1, 1)]
    f = lambda v: ", ".join("%.9g" % float(x) for x in v)  # noqa: E731
    cols = ", ".join("vec4(%s)" % f(P[:, c]) for c in range(4))
    pos = ", ".join("vec3(%s)" % f(c) for c in corners)
    uv = ", ".join("vec2(%s)" % f(u) for u in uvs)
    return """#version 450
layout(location = 4) out vec4 texcoord0;
layout(location = 5) out vec4 texcoord1;
layout(location = 6) out vec4 texcoord2;
void main() {
    vec3 pos[4] = vec3[](%s);
    vec2 uv[4] = vec2[](%s);
    mat4 P = mat4(%s);
    gl_Position = P * vec4(pos[gl_VertexIndex], 1.0);
    texcoord0 = vec4(uv[gl_VertexIndex], 0.0, 0.0);
    texcoord1 = vec4(%s, 1.0);
    texcoord2 = vec4(0.0, 2.0, 0.0, 0.0); // x: névoa 0; y: referência do teste de alfa (passa com 2)
}
""" % (pos, uv, cols, f(VCOLOR))


def pick_program():
    if len(sys.argv) > 1:
        return sys.argv[1]
    # O formato mais comum do jogo: textura * cor por vértice (texcoord1), com teste de alfa e névoa.
    return os.path.join(ROOT, "spv", "miss", "00042ff66d85104a.gxp")


def build(tmp):
    exe = os.path.join(tmp, "pbr_gpu")
    rt = os.path.join(ROOT, "runtime")
    subprocess.check_call(["g++", "-std=c++20", "-O1", "-w", os.path.join(HERE, "pbr_gpu.cpp"),
                           os.path.join(rt, "vitart", "pbr.cpp"), "-I", os.path.join(rt, "vitart"), "-I",
                           os.path.join(rt, "arm"), "-I", os.path.join(ROOT, "third_party", "stb"), "-lvulkan", "-o", exe])
    return exe


def pixel_rays(P):
    """Posição e UV de referência por pixel (interseção do raio com o plano), máscara do interior."""
    vx, vy, vw, vh = (0 + 0.5) * RS, (408 + 0.5) * RS, 720.0 * RS, -408.0 * RS  # viewport do gxm_vk.cpp
    px, py = np.meshgrid(np.arange(W) + 0.5, np.arange(H) + 0.5)
    nx = (px - (vx + vw / 2)) / (vw / 2)
    ny = (py - (vy + vh / 2)) / (vh / 2)
    Pi = np.linalg.inv(P)

    def unproject(z):
        h = np.stack([nx, ny, np.full_like(nx, z), np.ones_like(nx)], -1) @ Pi.T
        return h[..., :3] / h[..., 3:]
    a, b = unproject(0.0), unproject(1.0)
    d = b - a
    n = np.cross(DU, DV)
    t = ((V00 - a) @ n) / (d @ n)
    pos = a + d * t[..., None]
    # pos = V00 + u DU + v DV  (mínimos quadrados, o plano é exato)
    A = np.stack([DU, DV], 1)
    uv = (pos - V00) @ np.linalg.pinv(A).T
    inside = (uv[..., 0] > 0.01) & (uv[..., 0] < 0.99) & (uv[..., 1] > 0.01) & (uv[..., 1] < 0.99)
    return pos, uv, inside


def reference(pos, mode, cfg):
    """Mesmas contas do epílogo de vitarecomp/usse/pbr.py, em float64."""
    norm = lambda v: v / np.linalg.norm(v, axis=-1, keepdims=True)  # noqa: E731
    dot = lambda a, b: np.sum(a * b, -1, keepdims=True)  # noqa: E731
    V = norm(-pos)
    ng = norm(np.cross(DU, DV))
    Ng = np.where(dot(ng, V) < 0, -ng, ng)
    # Cotangent frame de um paralelogramo com DU perpendicular a DV: T = DU^ |DV| / s, B = DV^ |DU| / s,
    # s = max(|DU|, |DV|) (a normalização pelo maior dos dois, como no shader).
    lu, lv = np.linalg.norm(DU), np.linalg.norm(DV)
    s = max(lu, lv)
    T, B = DU / lu * (lv / s), DV / lv * (lu / s)
    tn = NORMAL[:3] / 255.0 * 2 - 1
    N = norm(T * tn[0] * cfg["normal_strength"] + B * tn[1] * cfg["normal_strength"] + Ng * tn[2])
    if mode == 1:
        return N * 0.5 + 0.5
    if mode == 6:
        return pos * 0.01 - np.floor(pos * 0.01)
    orm = ORM / 255.0
    ao = 1 + (orm[0] - 1) * cfg["ao_strength"]
    rough = np.clip(orm[1], 0.045, 1.0)
    metal = np.clip(orm[2], 0, 1)
    weight = orm[3]
    texel = DIFFUSE[:3] / 255.0
    c = texel * VCOLOR  # o shader do jogo: textura x cor por vértice (névoa 0)
    albedo = texel ** 2.2
    orig = np.clip(c, 0, 1) ** 2.2
    baked = np.minimum(orig / np.maximum(albedo, 0.02), cfg["baked_max"])
    Ls = [(norm(d), col) for d, col in LIGHTS]
    lum = lambda x: dot(x, np.array([0.2126, 0.7152, 0.0722]))  # noqa: E731
    lg = AMB + sum(col * np.maximum(dot(Ng, L), 0) for L, col in Ls)
    lm = AMB + sum(col * np.maximum(dot(N, L), 0) for L, col in Ls)
    detail = np.clip((lum(lm) + 0.05) / (lum(lg) + 0.05), 0, 2.5)
    diffuse = orig * detail * ao * (1 - metal)
    a = rough * rough
    a2 = a * a
    NoV = np.maximum(dot(N, V), 1e-4)
    F0 = 0.04 + (albedo - 0.04) * metal
    spec = 0
    for L, col in Ls:
        Hh = norm(V + L)
        NoL, NoH, VoH = np.maximum(dot(N, L), 0), np.maximum(dot(N, Hh), 0), np.maximum(dot(V, Hh), 0)
        d = NoH * NoH * (a2 - 1) + 1
        D = a2 / (np.pi * d * d)
        vis = 0.5 / (NoL * np.sqrt(NoV * NoV * (1 - a2) + a2) + NoV * np.sqrt(NoL * NoL * (1 - a2) + a2) + 1e-5)
        F = F0 + (1 - F0) * np.maximum(1 - VoH, 0) ** 5
        spec = spec + F * (D * vis * NoL) * col
    spec = spec * cfg["specular"] * ao
    R = N * (2 * NoV) - V
    up, sky, ground = np.array([0, 1.0, 0]), np.array(cfg["sky"]), np.array(cfg["ground"])
    t = np.clip((dot(R, up) + 0.3) / 0.6, 0, 1)
    env = ground + (sky - ground) * (t * t * (3 - 2 * t))
    env = env + ((sky + ground) * 0.5 - env) * rough
    r = rough * np.array([-1, -0.0275, -0.572, 0.022]) + np.array([1, 0.0425, 1.04, -0.04])
    a004 = np.minimum(r[0] * r[0], 2 ** (-9.28 * NoV)) * r[0] + r[1]
    env_brdf = F0 * (a004 * -1.04 + r[2]) + (a004 * 1.04 + r[3])
    local = np.clip(lum(baked), 0, 1.5)
    env_spec = env * env_brdf * (local * ao * cfg["env_specular"])
    lin = diffuse + spec + env_spec
    out = np.clip(np.maximum(lin, 0) ** (1 / 2.2), 0, 1)
    if mode == 7:
        return np.broadcast_to(detail * 0.5, out.shape)
    return c + (out - c) * weight


CFG = dict(normal_strength=1.0, specular=1.0, env_specular=0.5, baked_max=4.0, ao_strength=1.0,
           sky=[0.55, 0.60, 0.70], ground=[0.22, 0.20, 0.18])


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        blob = open(pick_program(), "rb").read()
        prog = Program.parse(blob[:struct.unpack_from("<I", blob, 8)[0]])
        frag = os.path.join(tmp, "frag.spv")
        open(frag, "wb").write(translate(prog, pbr=True))
        P = projection()
        open(os.path.join(tmp, "v.vert"), "w").write(vertex_glsl(P))
        vert = os.path.join(tmp, "vert.spv")
        subprocess.check_call(["glslangValidator", "-V", "-o", vert, os.path.join(tmp, "v.vert")], stdout=subprocess.DEVNULL)
        vu = np.zeros(40, np.float32)
        vu[0:16] = P.T.reshape(-1)  # colunas, como o g_Projection do GoW
        for i, (d, col) in enumerate(LIGHTS):
            vu[16 + 4 * i:19 + 4 * i] = d
            vu[24 + 4 * i:27 + 4 * i] = col
        vu[32:35] = AMB
        vu.tofile(os.path.join(tmp, "vu.bin"))
        maps = np.concatenate([np.tile(m, 16) for m in (DIFFUSE, NORMAL, ORM)]).astype(np.uint8)
        maps.tofile(os.path.join(tmp, "maps.bin"))
        pos, uv, inside = pixel_rays(P)
        # Longe das bordas do plano: o quad 2x2 das derivadas fica todo dentro.
        core = inside.copy()
        for dy in (-2, 0, 2):
            for dx in (-2, 0, 2):
                core &= np.roll(np.roll(inside, dy, 0), dx, 1)
        print("pixels no interior do plano: %d" % core.sum())
        ok = True
        for mode, tol_max, tol_mean in ((1, 3, 1.0), (6, 3, 1.0), (7, 3, 1.0), (0, 4, 1.0)):
            data = os.path.join(tmp, "data%d" % mode)
            os.makedirs(data, exist_ok=True)
            open(os.path.join(data, "pbr.ini"), "w").write("enabled=1\ndebug=%d\n" % mode)
            out = os.path.join(tmp, "out%d.rgba" % mode)
            subprocess.check_call([exe, vert, frag, os.path.join(tmp, "vu.bin"), os.path.join(tmp, "maps.bin"), out, str(RS)],
                                  env=dict(os.environ, PBR_TEST_DATA=data))
            img = np.fromfile(out, np.uint8).reshape(H, W, 4)[..., :3].astype(np.float64)
            ref = np.round(np.clip(reference(pos, mode, CFG), 0, 1) * 255)
            diff = np.abs(img - ref)
            if mode == 6:  # fract: 0 e 255 são vizinhos
                diff = np.minimum(diff, 255 - diff)
            dmax, dmean = diff[core].max(), diff[core].mean()
            nan_like = (img[core] == [0, 0, 255]).all(-1).sum()
            good = dmax <= tol_max and dmean <= tol_mean and nan_like == 0
            ok &= good
            print("modo %d: diferença máx %d, média %.2f (limite %d / %.1f) -> %s" % (
                mode, dmax, dmean, tol_max, tol_mean, "ok" if good else "FALHOU"))
            if mode == 0:
                cx, cy = W // 2, int(np.argwhere(core[:, W // 2]).mean())
                print("  amostra (%d,%d): GPU %s, referência %s" % (cx, cy, img[cy, cx], ref[cy, cx]))
        print("PBR GPU:", "ok" if ok else "FALHOU")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
