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

/* Metadados do módulo do Vita traduzido: o runtime liga imports a exports de outros módulos
 * (ou a implementações próprias por NID) sem precisar do ELF original. */
typedef struct {
  uint32_t stub;       /* func: endereço do stub chamado por BL; var: tabela de relocação da variável */
  uint32_t lib_nid, nid;
  uint32_t kind;       /* 0 func, 1 var, 2 tls */
  const char *lib, *name;
} arc_import_entry;
typedef struct {
  uint32_t lib_nid, nid, addr;
  uint32_t kind;       /* 0 func, 1 var, 2 tls */
  const char *lib, *name;
} arc_export_entry;
typedef struct {
  const char *name;
  uint32_t module_start, module_stop;   /* com bit Thumb; 0 se ausente */
  uint32_t tls_addr, tls_filesz, tls_memsz;
  uint32_t process_param;               /* sceProcessParam do executável principal; 0 se ausente */
  uint32_t link_base, load_base;
  const arc_import_entry *imports;
  unsigned import_count;
  const arc_export_entry *exports;
  unsigned export_count;
} arc_module_info;

#endif
