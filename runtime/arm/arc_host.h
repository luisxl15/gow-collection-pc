/*
 * Interface entre o código recompilado empacotado como biblioteca dinâmica (gow1.dll / .so) e o
 * host que a carrega (o backend do Vita3K). A DLL não depende de símbolos do executável: o host
 * entrega as funções de serviço por `arc_bind` e lê a tabela de funções por `arc_get_table`.
 */
#ifndef ARC_HOST_H
#define ARC_HOST_H
#include "arc_abi.h"

#define ARC_HOST_ABI 1

typedef struct arc_host_api {
  uint32_t abi;  /* ARC_HOST_ABI */
  /* Chamada a um stub de import (HLE por NID ou LLE religado). */
  void (*import_call)(arc_ctx *ctx, uint32_t nid, uint32_t stub);
  /* Desvio indireto para código sem tradução: executar no JIT até voltar. */
  void (*dispatch_miss)(arc_ctx *ctx, uint32_t target, uint32_t from);
  /* Um chamado voltou para outro lugar: desempilhar o C (não retorna). */
  void (*ret_mismatch)(arc_ctx *ctx, uint32_t expected);
  /* svc fora de stub de import (raro em apps): não retorna se o host não souber tratar. */
  void (*svc)(arc_ctx *ctx, uint32_t imm, uint32_t pc);
  /* Erro fatal no código gerado: instrução sem tradução, udf, índice de tabela inválido. */
  void (*fatal)(arc_ctx *ctx, uint32_t pc, const char *why);
  void (*log)(const char *msg);
} arc_host_api;

#if !defined(ARC_BUILD_DLL)
#define ARC_EXPORT /* visto pelo host: só declarações */
#elif defined(_WIN32)
#define ARC_EXPORT __declspec(dllexport)
#else
#define ARC_EXPORT __attribute__((visibility("default")))
#endif

/* Exportadas pela DLL. */
ARC_EXPORT int arc_bind(const arc_host_api *api, uint8_t *guest_mem);
ARC_EXPORT const arc_func_entry *arc_get_table(unsigned *count);
ARC_EXPORT const char *arc_get_module_name(void);
/* Segmentos da imagem usada na tradução: o host compara o de código com o que carregou. */
ARC_EXPORT const arc_segment *arc_get_segments(unsigned *count);
ARC_EXPORT const arc_module_info *arc_get_module(void);

#endif
