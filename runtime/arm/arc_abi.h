/*
 * ABI do código gerado: estruturas trocadas entre o C recompilado e quem o hospeda.
 * C puro, sem extensões de compilador, para poder ser incluído pelo MSVC (backend do Vita3K).
 * O layout de arc_ctx é contrato: mudar exige recompilar a DLL e o host juntos.
 */
#ifndef ARC_ABI_H
#define ARC_ABI_H
#include <stdint.h>

typedef struct arc_ctx {
  uint32_t r[16];
  uint32_t n, z, c, v; /* APSR flags as 0/1 */
  uint64_t d[32];      /* VFP/NEON D registers; S(2k) = low half of D(k), S(2k+1) = high half */
  uint32_t fpscr;
  uint32_t tpidruro;
  uint32_t tpidrurw;   /* TLS de usuário gravável (mcr/mrc p15,0,rX,c13,c0,2) */
  uint32_t apsr_rest; /* APSR bits other than NZCV (Q, GE); Q is not yet modelled */
  /* Monitor exclusivo local (LDREX/STREX): endereço, tamanho e valor lidos. O STREX é um CAS contra
   * esse valor, o que interopera com outras threads (nativas ou no JIT do Vita3K) que fazem o mesmo. */
  uint32_t excl_valid, excl_addr, excl_size;
  uint64_t excl_val;
} arc_ctx;

typedef void (*arc_func)(arc_ctx *);
typedef struct {
  uint32_t key; /* entry address | Thumb bit */
  arc_func fn;
  const char *name;
} arc_func_entry;
typedef struct {
  uint32_t vaddr, filesz, memsz;
  const uint8_t *data;
} arc_segment;

#endif
