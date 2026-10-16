# PBR: texturas e iluminação físicas no vitart

O renderizador Vulkan ganha uma variante PBR dos fragment shaders do jogo. Ela usa a BRDF GGX
(Cook-Torrance), normal map e mapa ORM por textura, e as luzes reais de cada desenho. A variante entra
só onde há material; o resto continua idêntico ao original.

## Como o jogo ilumina (medido nos shaders)

- 2.313 dos 2.373 fragment shaders leem a textura difusa `sTextureSampler` pela UV0, sem projeção, e
  multiplicam pela cor que vem do vértice (`texcoord1`).
- A luz é calculada **por vértice, em espaço de câmera**:
  - `g_Transforms` (mundo × câmera) leva posição e normal para a câmera;
  - `g_LightDir` e `g_LightColor` são 2 luzes direcionais e `g_Ambient` é o ambiente;
  - o resultado é `ambiente + Σ cor·max(N·L, 0)`. Exemplo de desmontagem: o programa
    `1084e5e1a409bca3`.
- `g_Projection` fica separada e vem em 4 colunas (`clip = Σ coluna_k · v_k`).

## O que a variante PBR faz (vitarecomp/usse/pbr.py)

O shader original roda inteiro. No fim, antes de gravar a cor, a variante:

1. **Reconstrói a posição em espaço de câmera** a partir de `gl_FragCoord` (x, y e profundidade). Usa a
   inversa de `g_Projection` combinada com o viewport que a GPU usou: escala interna, meio pixel e
   altura negativa. Não depende de varyings novas.
2. **Calcula a normal geométrica** pelas derivadas da posição e monta o *cotangent frame* (Schüler).
   Assim o normal map funciona sem tangentes nos vértices.
3. **Preserva a luz do jogo:** `luz assada = cor original / textura`. O normal map só a modula, pela razão
   entre a luz das mesmas lâmpadas do desenho na normal mapeada e na normal geométrica. As facetas da
   malha aparecem dos dois lados da razão e se cancelam.
4. **Especular GGX** (Smith correlacionado, Fresnel de Schlick) com as 2 luzes do desenho, mais um
   especular de ambiente analítico (céu e chão, aproximação de Karis). O ambiente é escurecido pela luz
   assada do local: uma caverna não reflete o céu.
5. **Mistura com a cor original** pelo alfa do ORM (peso do material) e mantém o alfa original.

As derivadas e as leituras dos mapas acontecem antes do código do jogo, em fluxo uniforme: depois de
um `kill`, as derivadas deixam de ser confiáveis.

Desenhos que ficam com a cor original:
- sem material e com `all_surfaces=0`;
- projeção ortográfica (HUD, passadas de tela);
- blend aditivo ou multiplicativo (fogo, brilhos);
- textura difusa que é um alvo de render.

Desenhos do cenário sem `g_LightDir` usam as luzes do último desenho iluminado do quadro (em geral,
os personagens). Se não houver, usam `fallback_dir` e `fallback_color` do `pbr.ini`.

## Passo a passo

1. **Variantes PBR dos shaders.** Rode de novo o gerador de cache. Ele grava `<hash>.pbr.spv` ao lado de
   cada `<hash>.spv` que serve e não refaz o que já existe:

   ```
   python tools/usse_build_cache.py <dir do cache> <dir do cache>/miss [capturas...]
   ```

2. **Materiais.** Gere normal map e ORM a partir do pack HD (as chaves são as mesmas do pack):

   ```
   python tools/pbr_generate.py vitart-data/texpack/gow1 vitart-data/pbr/gow1
   python tools/pbr_generate.py vitart-data/texpack/gow1 vitart-data/pbr/gow1 --overrides materiais.tsv --force
   ```

   `materiais.tsv` ajusta rugosidade, metal e força do relevo pelo nome do PNG ou pela chave:

   ```
   # padrão           rugosidade  metal  força
   *blades*           0.30        1.0
   0123456789abcdef   0.45        1.0    1.5
   ```

   Mapas feitos à mão (Substance, Materialize...) entram no mesmo `index.tsv`, uma linha por textura:
   `chave<TAB>normal.png<TAB>orm.png`. Um dos dois mapas pode ser `-`.

3. **Rodar.** Compile e rode como sempre. Na primeira execução aparece `vitart-data/pbr.ini`, relido com o
   jogo aberto: salve o arquivo e veja a mudança na hora. O log mostra a cada 10 s quantos desenhos
   usaram PBR.

4. **Desligar:** `enabled=0` no `pbr.ini` ou `VITART_PBR=0`.

## pbr.ini

| chave | padrão | efeito |
|---|---|---|
| `enabled` | 1 | liga a variante PBR |
| `all_surfaces` | 0 | também nas texturas sem material (dielétrico de `default_roughness`) |
| `normal_opengl` | 0 | normal maps com verde para cima (padrão OpenGL); o gerador grava no padrão DirectX |
| `normal_strength` | 1.0 | força do normal map |
| `specular` | 1.0 | especular das luzes |
| `env_specular` | 0.5 | reflexo do ambiente |
| `roughness_scale`, `metallic_scale`, `ao_strength` | 1.0 | escala dos canais do ORM |
| `baked_max` | 4.0 | teto da luz assada extraída do jogo |
| `sky`, `ground` | | cores lineares do ambiente refletido |
| `up` | 0 1 0 | "para cima" em espaço de câmera |
| `fallback_dir`, `fallback_color` | | luz sem desenho iluminado recente |
| `debug` | 0 | 1 normal, 2 luz assada, 3 rugosidade, 4 metal, 5 oclusão, 6 posição, 7 detalhe do normal map |

`debug=6` pinta `fract(posição / 100)`. Faixas contínuas que acompanham a geometria indicam que a
reconstrução está certa. Se a imagem mudar de cor ao girar a câmera sem mover nada, `g_Projection` não
é a matriz que o shader usou.

## Contrato (renderizador ↔ shader)

- **set 3:** binding 0 = normal map, binding 1 = ORM.
  - Normal: R = +U, G = +V (para baixo na imagem), B = para fora.
  - ORM: R = oclusão, G = rugosidade, B = metal, A = peso do PBR.
  - Os dois usam o endereçamento e o filtro da difusa, com mipmaps.
- **set 4:** binding 0 = storage buffer de 56 floats por desenho (`PARAMS` em `pbr.py`, preenchido por
  `runtime/vitart/pbr.cpp`):

  | floats | conteúdo |
  |---|---|
  | 0..15 | pixel → câmera (coluna a coluna) |
  | 16 / 20 | direção / cor da luz 0 |
  | 24 / 28 | direção / cor da luz 1 |
  | 32 | ambiente |
  | 36 | força do normal map, especular, especular de ambiente, teto da luz assada |
  | 40 | depuração, escala de rugosidade, escala de metal, força da oclusão |
  | 44 / 48 / 52 | céu / chão / "para cima" |

- O layout do pipeline tem 5 sets. Os shaders originais não usam os sets 3 e 4.

## Testes

- `tests/pbr_gpu/run.py` (Linux, qualquer Vulkan; o lavapipe do Mesa serve). Desenha um plano inclinado
  com o fragment shader mais comum do jogo (`00042ff66d85104a`) em variante PBR, pelo mesmo caminho de
  parâmetros do runtime, e compara pixel a pixel com uma referência numpy das mesmas contas. Medido em
  16/10/2026, nos 76.499 pixels do interior do plano:

  | modo | o que confere | resultado |
  |---|---|---|
  | 6 | posição reconstruída | diferença 0 |
  | 1 | normal mapeada | diferença 0 |
  | 7 | detalhe | diferença 0 |
  | 0 | cor final | no máximo 1/255 |

  Com `PBR_RS=2` (escala interna 2x), o resultado é o mesmo.
- Os 2.313 programas PBR passam no `spirv-val` (vulkan1.1). A tradução normal dos 4.746 programas
  continua idêntica byte a byte à anterior.

## Limites conhecidos

- O especular usa a normal geométrica (facetada) mais o normal map. Em malhas de poucos polígonos, o
  brilho pode mostrar as facetas. A normal suave por vértice exigiria passar a normal transformada
  como varying nova no shader de vértice traduzido.
- O ambiente refletido é analítico (céu e chão), não um cubemap da cena.
- Os mapas automáticos vêm da luminância da textura: bons para pedra, madeira e tecido, fracos para
  pintura plana. Para peças importantes (lâminas, armaduras), ajuste com `--overrides` ou com mapas
  feitos à mão.
- Ainda não existe opção no menu de pausa. O ajuste é pelo `pbr.ini`, ao vivo.
