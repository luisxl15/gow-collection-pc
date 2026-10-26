# Renderizadores do vitart: Vulkan e Direct3D 11

O vitart desenha o GXM do Vita por um de dois renderizadores com a mesma interface (`runtime/vitart/gxm_api.inc`):

| Renderizador | Arquivo | Shaders no cache | Observações |
|---|---|---|---|
| Vulkan | `gxm_vk.cpp` (`gvk::vkb`) | `<hash>.spv`, `<hash>.pbr.spv` | o completo: pós-processamento, PBR, cópias de alvo por cache |
| Direct3D 11 | `gxm_d3d11.cpp` (`gvk::dx11`) | `<hash>.dx11`, `<hash>.pbr.dx11` | feature level 11.0; cai para WARP (software) se não houver GPU D3D11; sem pós-processamento |

`gxm_backend.cpp` escolhe um e repassa as chamadas; a janela (`gxm_window.cpp`) e os avisos/FPS/barras de vida
(`gxm_overlay.cpp`) são comuns aos dois.

## Escolha

`renderer=` no `vitart-data/vitart_options.ini` ou a variável `VITART_RENDERER`:

- `auto` (padrão): Vulkan; se o Vulkan não abrir (sem driver, sem `vulkan-1.dll`), Direct3D 11.
- `vulkan` ou `d3d11`: só aquele.

Vale ao abrir o jogo. O `vulkan-1.dll` é carregado por demanda (`/DELAYLOAD`), então o executável abre em
máquina sem Vulkan.

## Cache de shaders do Direct3D 11

```
python tools/usse_build_cache.py <dir do cache> <dir do cache>/miss --d3d11 [--jobs N]
```

Cada programa GXP vira SPIR-V no modo d3d do tradutor (`vitarecomp/usse/translate.py`, constantes `D3D_*`), depois
HLSL (spirv-cross do Vulkan SDK) e DXBC shader model 5.0 (`d3dcompiler_47.dll` do Windows). Programas cujo `.gxp`
não existe mais saem do `.spv` do Vulkan, levado ao mesmo contrato por texto (`tools/usse_d3d.py`).

Contrato de recursos:

- texturas e samplers em t/s 0..15, com os mapas do PBR em 14 e 15;
- uniforms num `ByteAddressBuffer` em t20 (e t21, as palavras do vldst);
- parâmetros do PBR em t22;
- `cbuffer b0`: x/y/z são as bases (em palavras) do vértice, do fragmento e do PBR; w é o sinal de y da posição
  (o viewport do D3D11 não aceita altura negativa).

O vértice e o fragmento declaram sempre as 13 varyings (TEXCOORD1..13), porque o D3D11 liga por registrador.

### Inteiros nos registradores e o FXC

O USSE guarda inteiros (endereços do vldst, índices) nos registradores de float. O FXC zera denormais ao dobrar
constantes: `asfloat(220u)` vira 0, e até `asuint(asfloat(220u))`, com qualquer opção de compilação. No GoW isso
fazia o vértice ler a matriz do uniform buffer no byte 4 em vez de 224, e toda a geometria saía atrás da câmera.
O mesmo vale para cores em meias-precisões empacotadas com a metade alta zerada (`0x00002E66`): o quadro que
escurece as sombras perdia o vermelho, e o chão ficava roxo. Por isso `usse_d3d.uint_registers` reescreve o
SPIR-V antes do spirv-cross:

- os bancos de registradores (`pa`, `sa`, `temp`…) viram arrays de `uint`;
- as gravações recebem os bits;
- o bitcast de volta para inteiro usa a carga `uint` direto.

Depuração do D3D11, por variável de ambiente:

- `VITART_D3D11_LOG=<quadro>`: todos os desenhos de um quadro, mais o stream output das posições dos primeiros
  desenhos 3D;
- `VITART_D3D11_WARP`: força o WARP;
- `VITART_D3D11_MAGENTA`: fragmento magenta nos desenhos 3D;
- `VITART_D3D11_WHITETEX`, `VITART_D3D11_NODEPTHTEX`.

## Direct3D 9: por que não há

Os shaders do Vita usam inteiros, índice dinâmico em arrays de registradores e leitura de buffer por endereço
(vldst). O shader model 3.0 do D3D9 não tem nenhum dos três. O FXC recusa os programas traduzidos (X3704 e
X3548). Emular isso em ps_3_0/vs_3_0 exigiria outro tradutor, com perdas. O Direct3D 11 roda em qualquer GPU
desde 2009 e, sem GPU, no WARP, que cobre as máquinas que o D3D9 cobriria.

## AVX2

O vitart não precisa de AVX2: o código do runtime é compilado para x86-64 básico. As rotinas com AVX que aparecem
no executável são da CRT/STL da Microsoft e escolhem a versão pela CPU em tempo de execução.
