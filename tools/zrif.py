#!/usr/bin/env python3
"""Converte licença RIF (work.bin, 512 bytes) <-> zRIF (base64).

Mesmo formato do libzrif/pkg2zip: deflate nível 9, janela de 2^10, com
dicionário fixo de 1024 bytes. A conversão é verificada por ida e volta.

    python tools/zrif.py encode work.bin
    python tools/zrif.py decode <zRIF> work.bin
"""
import base64
import sys
import zlib

_TAIL = bytes.fromhex(
    "3030303039000000000000000000000030303030363030303037303030303800"
    "303030303330303030343030303035305f30302d414444434f4e543030303032"
    "2d5043534730303030303030303030312d504353453030302d50435346303030"
    "2d504353433030302d504353443030302d504353413030302d50435342303030"
    "0001000100010002efcdab8967452301")
DICT = bytes(1024 - len(_TAIL)) + _TAIL
assert len(DICT) == 1024


def rif_to_zrif(rif: bytes) -> str:
    if len(rif) != 512:
        raise ValueError("RIF precisa ter 512 bytes")
    c = zlib.compressobj(9, zlib.DEFLATED, 10, 8, zlib.Z_DEFAULT_STRATEGY, zdict=DICT)
    z = base64.b64encode(c.compress(rif) + c.flush()).decode()
    if zrif_to_rif(z) != rif:
        raise AssertionError("ida e volta falhou")
    return z


def zrif_to_rif(z: str) -> bytes:
    d = zlib.decompressobj(10, zdict=DICT)
    return d.decompress(base64.b64decode(z)) + d.flush()


if __name__ == "__main__":
    if sys.argv[1] == "encode":
        print(rif_to_zrif(open(sys.argv[2], "rb").read()))
    elif sys.argv[1] == "decode":
        open(sys.argv[3], "wb").write(zrif_to_rif(sys.argv[2]))
