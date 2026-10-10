#!/usr/bin/env python3
"""Oráculo ARMv7 com Unicorn, no lugar do qemu-arm onde ele não existe (Windows).

Executa um ELF ARM estático de Linux (os guests de teste: crt0_arm.S + glibc.c) com a
personalidade mínima que os testes usam: write(4) e exit(1)/exit_group(248) via `svc #0`.
Grava em stdout exatamente o que o guest escreveu e sai com o código do guest.

    python tests/oracle_unicorn.py guest.elf

O Unicorn é o mesmo núcleo do QEMU (TCG), mas não é o qemu-arm: as diferenças de
inicialização (sp, auxv, FPSCR) não afetam os testes, que semeiam o próprio estado.
"""
from __future__ import annotations

import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_INTR, UC_MODE_ARM, UC_PROT_ALL, Uc, UcError)
from unicorn import arm_const as R

STACK_TOP = 0xE0000000 - 0x1000
STACK_SIZE = 8 << 20
MAX_INSNS = 200_000_000


def load(uc: Uc, data: bytes) -> int:
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit("esperado ELF32")
    e_entry, e_phoff = struct.unpack_from("<II", data, 24)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    for i in range(e_phnum):
        p_type, off, va, _pa, fs, ms, _fl, _al = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        if p_type != 1 or not ms:
            continue
        lo, hi = va & ~0xFFF, (va + ms + 0xFFF) & ~0xFFF
        try:
            uc.mem_map(lo, hi - lo, UC_PROT_ALL)
        except UcError:   # segmento vizinho já mapeou parte da página
            for p in range(lo, hi, 0x1000):
                try:
                    uc.mem_map(p, 0x1000, UC_PROT_ALL)
                except UcError:
                    pass
        uc.mem_write(va, data[off:off + fs])
    return e_entry


def main() -> int:
    data = open(sys.argv[1], "rb").read()
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try:
        uc.ctl_set_cpu_model(R.UC_CPU_ARM_CORTEX_A9)
    except Exception:
        pass
    entry = load(uc, data)
    uc.mem_map(STACK_TOP + 0x1000 - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
    uc.reg_write(R.UC_ARM_REG_SP, STACK_TOP)
    # VFP/NEON ligados, como no Linux: CPACR cp10/cp11 = acesso total, FPEXC.EN = 1.
    uc.reg_write(R.UC_ARM_REG_C1_C0_2, uc.reg_read(R.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(R.UC_ARM_REG_FPEXC, 0x40000000)
    out = bytearray()
    status = {"code": None}

    def on_intr(uc: Uc, intno: int, _ud):
        nr = uc.reg_read(R.UC_ARM_REG_R7)
        a0, a1, a2 = (uc.reg_read(r) for r in (R.UC_ARM_REG_R0, R.UC_ARM_REG_R1, R.UC_ARM_REG_R2))
        if intno != 2:            # 2 = EXCP_SWI no QEMU
            status["code"] = 139
            uc.emu_stop()
            return
        if nr in (1, 248):
            status["code"] = a0 & 0xFF
            uc.emu_stop()
        elif nr == 4:
            buf = bytes(uc.mem_read(a1, a2))
            if a0 == 1:
                out.extend(buf)
            else:
                sys.stderr.buffer.write(buf)
            uc.reg_write(R.UC_ARM_REG_R0, a2)
        else:
            uc.reg_write(R.UC_ARM_REG_R0, (-38) & 0xFFFFFFFF)

    uc.hook_add(UC_HOOK_INTR, on_intr)
    try:
        uc.emu_start(entry, 0xFFFFFFFF, count=MAX_INSNS)   # bit 0 do entry escolhe Thumb
    except UcError as e:
        pc = uc.reg_read(R.UC_ARM_REG_PC)
        sys.stderr.write(f"oracle: falha {e} em pc={pc:#x}\n")
        status["code"] = 139
    sys.stdout.buffer.write(out)
    sys.stdout.flush()
    return 134 if status["code"] is None else status["code"]


if __name__ == "__main__":
    sys.exit(main())
