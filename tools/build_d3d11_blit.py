"""Gera runtime/vitart/d3d11_blit.h: os shaders do renderizador Direct3D 11 que não vêm do jogo (cópia com escala
para a janela e entre alvos), compilados aqui para o vitart não depender do compilador de HLSL.

    python tools/build_d3d11_blit.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from usse_d3d import compile_hlsl  # noqa: E402

# Triângulo que cobre o viewport a partir do SV_VertexID (sem buffer de vértices); uv 0..1 de cima para baixo.
VS = b"""
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O main(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
"""
PS = b"""
Texture2D src : register(t0);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return src.Sample(smp, uv); }
"""
# Depuração (VITART_D3D11_MAGENTA): no lugar do fragmento do jogo, sem entradas (liga com qualquer vértice).
PS_MAGENTA = b"float4 main() : SV_Target { return float4(1, 0, 1, 1); }"


def array(name, data):
    words = ", ".join("0x%02x" % b for b in data)
    return "static const unsigned char %s[%d] = {%s};\n" % (name, len(data), words)


def main():
    out = os.path.join(os.path.dirname(__file__), "..", "runtime", "vitart", "d3d11_blit.h")
    text = ("// Gerado por tools/build_d3d11_blit.py. Não edite.\n#pragma once\n\n" +
            array("kBlitVS", compile_hlsl(VS, "vs_4_0")) + array("kBlitPS", compile_hlsl(PS, "ps_4_0")) +
            array("kMagentaPS", compile_hlsl(PS_MAGENTA, "ps_4_0")))
    open(out, "w", encoding="utf-8", newline="\n").write(text)
    print("ok:", out)


if __name__ == "__main__":
    main()
