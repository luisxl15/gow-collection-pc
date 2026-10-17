// Comum aos passes de pós-processamento (gxm_vk.cpp, post_*). Iluminação de tela no estilo dos jogos
// atuais, aplicada antes do HUD: oclusão de ambiente em duas escalas (SSAO), profundidade de campo com
// foco automático, raios de luz da direção da luz do jogo, bloom, curva fílmica, color grading,
// nitidez adaptativa, aberração cromática e granulação.
layout(set = 0, binding = 0) uniform sampler2D uColor; // cópia do alvo (sRGB em RGBA8)
layout(set = 0, binding = 1) uniform sampler2D uDepth; // cópia da profundidade (D32)
layout(set = 0, binding = 2, rgba16f) uniform image2D uA; // meia resolução: rgb = bloom, a = oclusão
layout(set = 0, binding = 3, rgba16f) uniform image2D uB; // meia resolução, ida e volta do desfoque de uA
layout(set = 0, binding = 4) uniform sampler2D uAs;    // uA amostrada (ampliação bilinear)
layout(set = 0, binding = 5, rgba8) uniform writeonly image2D uOut; // o alvo de cor do jogo
layout(set = 0, binding = 6, rgba16f) uniform image2D uC; // meia resolução: rgb = cor (linear) para o DOF, a = céu
layout(set = 0, binding = 7, rgba16f) uniform image2D uD; // ida e volta do desfoque de uC; depois os raios de luz
layout(set = 0, binding = 8) uniform sampler2D uCs;    // uC amostrada (cor desfocada)
layout(set = 0, binding = 9) uniform sampler2D uDs;    // uD amostrada (raios de luz)
layout(set = 0, binding = 10, rgba16f) uniform image2D uF; // 1x1: distância de foco (suavizada entre quadros)

layout(push_constant) uniform P {
    mat4 s2v;   // pixel (x+0,5, y+0,5, profundidade, 1) -> câmera (homogêneo)
    vec4 p0;    // x foco em pixels a 1 unidade, y raio da oclusão (unidades do jogo), z força da oclusão, w limiar do bloom
    vec4 p1;    // x intensidade do bloom, y exposição, z força da curva fílmica, w saturação
    vec4 p2;    // x vinheta, y largura, z altura, w direção do desfoque (0 = horizontal)
    vec4 p3;    // x largura/2, y altura/2, z quadro (ruído), w passo do bloom em texels
    vec4 p4;    // x força do DOF, y faixa de foco (relativa à distância), z desfoque perto (0/1), w passo do DOF
    vec4 p5;    // xy posição da luz na tela (pixels), z intensidade dos raios, w decaimento
    vec4 p6;    // rgb cor da luz, a luz visível (0/1)
    vec4 p7;    // x granulação, y aberração cromática, z nitidez, w adaptação do foco por quadro
    vec4 p8;    // rgb lift (sombras), a força do color grading
    vec4 p9;    // rgb gain (altas luzes), a contraste
    vec4 p10;   // x tela dividida, y vista de depuração, z distância de foco (suavizada pela CPU)
    vec4 p11;   // reservado
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
float dist_at(ivec2 px) { return length(view_at(px)); }
bool is_sky(ivec2 px) {
    float d = texelFetch(uDepth, clamp(px, ivec2(0), ivec2(pc.p2.yz) - 1), 0).r;
    return d >= 1.0 || d <= 0.0; // só o valor de limpeza: com o plano distante do GoW, a cena já fica acima de 0,9999
}
// céu para os raios de luz: vazio ou muito longe (o céu do GoW é geometria distante que grava profundidade)
bool is_far(ivec2 px) { return is_sky(px) || dist_at(px) > 1500.0; }
