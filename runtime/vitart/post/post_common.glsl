// Comum aos passes de pós-processamento (gxm_vk.cpp, post_*). Iluminação de tela no estilo dos jogos
// atuais: oclusão de ambiente pela profundidade (SSAO), bloom e curva fílmica, aplicados antes do HUD.
layout(set = 0, binding = 0) uniform sampler2D uColor; // cópia do alvo (sRGB em RGBA8)
layout(set = 0, binding = 1) uniform sampler2D uDepth; // cópia da profundidade (D32)
layout(set = 0, binding = 2, rgba16f) uniform image2D uA; // meia resolução: rgb = bloom, a = oclusão
layout(set = 0, binding = 3, rgba16f) uniform image2D uB; // meia resolução, ida e volta do desfoque
layout(set = 0, binding = 4) uniform sampler2D uAs;    // uA amostrada (ampliação bilinear)
layout(set = 0, binding = 5, rgba8) uniform writeonly image2D uOut; // o alvo de cor do jogo

layout(push_constant) uniform P {
    mat4 s2v;   // pixel (x+0,5, y+0,5, profundidade, 1) -> câmera (homogêneo)
    vec4 p0;    // x foco em pixels a 1 unidade, y raio da oclusão (unidades do jogo), z força da oclusão, w limiar do bloom
    vec4 p1;    // x intensidade do bloom, y exposição, z força da curva fílmica, w saturação
    vec4 p2;    // x vinheta, y largura, z altura, w direção do desfoque (0 = horizontal)
    vec4 p3;    // x largura/2, y altura/2, z quadro (ruído), w raio do bloom
} pc;

vec3 srgb_to_lin(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 lin_to_srgb(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 view_at(ivec2 px) {
    ivec2 sz = ivec2(pc.p2.y, pc.p2.z);
    px = clamp(px, ivec2(0), sz - 1);
    float d = texelFetch(uDepth, px, 0).r;
    vec4 v = pc.s2v * vec4(vec2(px) + 0.5, d, 1.0);
    return v.xyz / v.w;
}
