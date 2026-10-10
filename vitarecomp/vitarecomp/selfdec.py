"""Decifragem de SELF NPDRM do PS Vita (apps do PSN) para ELF.

Porte de `decrypt_fself`/`get_segments` do Vita3K (vita3k/packages/src/sce_utils.cpp,
GPL-2.0). A cadeia é:

  klicensee (RIF+0x50) --AES-128-CBC(chave NPDRM)--> chave intermediária
  bloco MetadataInfo   --AES-128-CBC(intermediária)--> --AES-256-CBC(chave METADATA)-->
                          chave/iv dos metadados
  metadados            --AES-128-CBC--> seções (offset, segmento, chave, iv, deflate)
  cada segmento        --AES-128-CTR--> [zlib] --> bytes do PT_LOAD

As chaves do sistema não ficam neste repositório: são importadas da fonte do
Vita3K para um JSON local (`python -m vitarecomp keys-import`).
"""
from __future__ import annotations

import json
import os
import re
import struct
import zlib
from pathlib import Path

from Crypto.Cipher import AES
from Crypto.Util import Counter

SCE_MAGIC = b"SCE\0"
SELF_TYPE_APP = 8
CTRL_DIGEST_SHA256 = 4   # vita3k ControlType
CTRL_NPDRM_VITA = 5
ENC_AES128CTR = 3        # vita3k EncryptionType
COMP_DEFLATE = 2
SECURE_NO, SECURE_YES = 1, 2


class SelfError(Exception):
    pass


# ---------------------------------------------------------------------- chaves

_ENUMS = {
    "KeyType": {"METADATA": 0, "NPDRM": 1},
    "SceType": {"SELF": 1, "SRVK": 2, "SPKG": 3, "DEV": 0xC0},
    "SelfType": {"NONE": 0, "KERNEL": 7, "APP": 8, "BOOT": 9, "SECURE": 11, "USER": 13},
}


def import_keys(sce_utils_cpp: Path) -> list[dict]:
    """Lê `register_keys(KeyStore&, int type)` caso 1 (o que decrypt_fself usa)."""
    src = sce_utils_cpp.read_text(encoding="utf-8")
    body = src[src.index("void register_keys("):]
    body = body[:body.index("\n}\n")]
    case1 = body[body.index("case 1:"):]
    nxt = re.search(r"\n\s*(case \d+:|default:)", case1[7:])
    if nxt:
        case1 = case1[:7 + nxt.start()]
    keys = []
    for call in re.findall(r"SCE_KEYS\.register_keys\((.*?)\);", case1, re.S):
        args = [a.strip() for a in call.split(",")]
        kt = _ENUMS["KeyType"][args[0].split("::")[1]]
        st = _ENUMS["SceType"][args[1].split("::")[1]]
        rev = int(args[2], 0)
        key, iv = args[3].strip('"'), args[4].strip('"')
        minv = int(args[5], 0) if len(args) > 5 else 0
        maxv = int(args[6], 0) if len(args) > 6 else 0xFFFFFFFFFFFFFFFF
        sf = _ENUMS["SelfType"][args[7].split("::")[1]] if len(args) > 7 else 0
        keys.append(dict(keytype=kt, scetype=st, rev=rev, key=key, iv=iv, minver=minv, maxver=maxv, selftype=sf))
    if not keys:
        raise SelfError("nenhuma chave encontrada no caso 1 de register_keys")
    return keys


def default_keys_path() -> Path:
    return Path(os.environ.get("VITARECOMP_KEYS", Path.home() / ".vitarecomp" / "keys.json"))


def load_keys(path: Path | None = None) -> list[dict]:
    p = path or default_keys_path()
    if not p.exists():
        raise SelfError(f"chaves não encontradas em {p}; rode `python -m vitarecomp keys-import "
                        f"<Vita3K>/vita3k/packages/src/sce_utils.cpp`")
    return json.loads(p.read_text())


def _get_key(keys, keytype, scetype, sysver, rev, selftype):
    # Mesma regra de KeyStore::get: primeira entrada com versão no intervalo e revisão igual.
    for k in keys:
        if (k["keytype"], k["scetype"], k["selftype"]) != (keytype, scetype, selftype):
            continue
        if k["minver"] <= sysver <= k["maxver"] and k["rev"] == rev:
            return bytes.fromhex(k["key"]), bytes.fromhex(k["iv"])
    raise SelfError(f"sem chave para keytype={keytype} sce={scetype} sysver={sysver:#x} rev={rev} self={selftype}")


# ------------------------------------------------------------------- formato

def parse(data: bytes) -> dict:
    if data[:4] != SCE_MAGIC:
        raise SelfError("não é SELF (cabeçalho SCE ausente; camada PFS ainda cifrada?)")
    version, platform, key_rev, sce_type, meta_off, header_len, _datalen = struct.unpack_from("<IBBHIQQ", data, 4)
    (_flen, _f8, _selfoff, appinfo_off, elf_off, phdr_off, _shdr, seginfo_off, _ver_off, ctrl_off,
     _ctrl_len) = struct.unpack_from("<11Q", data, 32)
    auth_id, vendor_id, self_type, sys_version, _ = struct.unpack_from("<QIIQQ", data, appinfo_off)
    e_phnum = struct.unpack_from("<H", data, elf_off + 44)[0]
    phdrs = [struct.unpack_from("<8I", data, phdr_off + 32 * i) for i in range(e_phnum)]
    seginfos = []
    for i in range(e_phnum):
        off, size, comp, _f14, plain, _f1c = struct.unpack_from("<QQIIII", data, seginfo_off + 32 * i)
        seginfos.append(dict(offset=off, size=size, compressed=comp == SECURE_YES, plaintext=plain == SECURE_YES))
    # control info: [digest sha256] + npdrm
    npdrm_type = 0
    content_id = None
    off = ctrl_off
    ctype, csize = struct.unpack_from("<II", data, off)
    if ctype == CTRL_DIGEST_SHA256:   # cabeçalho de 16 + SceControlInfoDigest256 de 64
        off += 16 + 64
        ctype, csize = struct.unpack_from("<II", data, off)
    if ctype == CTRL_NPDRM_VITA:
        d = off + 16
        npdrm_type = struct.unpack_from("<I", data, d + 8)[0]
        content_id = data[d + 0x10:d + 0x40].split(b"\0")[0].decode()
    return dict(version=version, platform=platform, key_rev=key_rev, sce_type=sce_type, meta_off=meta_off,
                header_len=header_len, auth_id=auth_id, vendor_id=vendor_id, self_type=self_type,
                sys_version=sys_version, elf_off=elf_off, phdrs=phdrs, seginfos=seginfos,
                npdrm_type=npdrm_type, content_id=content_id)


def is_encrypted(info: dict) -> bool:
    return any(not s["plaintext"] for s in info["seginfos"])


def _cbc(key, iv, buf):
    return AES.new(key, AES.MODE_CBC, iv).decrypt(buf)


def _metadata_sections(data: bytes, h: dict, keys, klic: bytes | None):
    blob = data[h["meta_off"] + 48:h["header_len"]]
    info = blob[:64]
    if h["self_type"] == SELF_TYPE_APP:
        if not klic or klic == bytes(16):
            raise SelfError("SELF de app NPDRM exige a klicensee da licença")
        np_rev = 1 if h["key_rev"] >= 2 else 0
        np_key, np_iv = _get_key(keys, 1, h["sce_type"], h["sys_version"], np_rev, h["self_type"])
        inter = _cbc(np_key, np_iv, klic)
        info = _cbc(inter, np_iv, info)
    mkey, miv = _get_key(keys, 0, h["sce_type"], h["sys_version"], h["key_rev"], h["self_type"])
    dec = _cbc(mkey, miv, info)
    if dec[16:32] != bytes(16) or dec[48:64] != bytes(16):
        raise SelfError("MetadataInfo inválido após decifrar (licença ou chave erradas)")
    meta = _cbc(dec[0:16], dec[32:48], blob[64:])
    _siglen, _sigtype, nsec, nkeys = struct.unpack_from("<QIII", meta, 0)
    vault_at = 32 + nsec * 48
    vault = [meta[vault_at + 16 * i:vault_at + 16 * (i + 1)] for i in range(nkeys)]
    secs = []
    for i in range(nsec):
        (off, size, _type, seg_idx, _ht, _hi, enc, key_idx, iv_idx, comp) = struct.unpack_from(
            "<QQIiIiIiiI", meta, 32 + 48 * i)
        if enc == ENC_AES128CTR:
            secs.append(dict(offset=off, size=size, seg=seg_idx, key=vault[key_idx], iv=vault[iv_idx],
                             deflate=comp == COMP_DEFLATE))
    return secs


def decrypt_to_elf(data: bytes, klic: bytes | None, keys=None) -> tuple[bytes, dict]:
    """Devolve o ELF (cabeçalho + phdrs originais, segmentos nos seus p_offset)."""
    h = parse(data)
    keys = keys if keys is not None else (load_keys() if is_encrypted(h) else [])
    secs = {s["seg"]: s for s in _metadata_sections(data, h, keys, klic)} if is_encrypted(h) else {}
    ehdr = bytearray(data[h["elf_off"]:h["elf_off"] + 52])
    e_phoff = struct.unpack_from("<I", ehdr, 28)[0]
    e_phnum = len(h["phdrs"])
    # Sem seções: o ELF de saída não tem tabela de seções.
    struct.pack_into("<I", ehdr, 32, 0)              # e_shoff
    struct.pack_into("<HHH", ehdr, 46, 0, 0, 0)      # e_shentsize, e_shnum, e_shstrndx
    end = e_phoff + 32 * e_phnum
    segs = []
    for i, ph in enumerate(h["phdrs"]):
        p_offset, p_filesz = ph[1], ph[4]
        if p_filesz == 0:
            segs.append(b"")
            continue
        si = h["seginfos"][i]
        raw = data[si["offset"]:si["offset"] + si["size"]]
        if not si["plaintext"]:
            s = secs.get(i)
            if s is None:
                raise SelfError(f"segmento {i} cifrado sem seção de metadados")
            ctr = Counter.new(128, initial_value=int.from_bytes(s["iv"], "big"))
            raw = AES.new(s["key"], AES.MODE_CTR, counter=ctr).decrypt(raw)
        if si["compressed"]:
            raw = zlib.decompress(raw)
        if len(raw) != p_filesz:
            raise SelfError(f"segmento {i}: {len(raw)} bytes, p_filesz={p_filesz}")
        segs.append(raw)
        end = max(end, p_offset + p_filesz)
    out = bytearray(end)
    out[0:52] = ehdr
    for i, ph in enumerate(h["phdrs"]):
        struct.pack_into("<8I", out, e_phoff + 32 * i, *ph)
        if segs[i]:
            out[ph[1]:ph[1] + len(segs[i])] = segs[i]
    return bytes(out), h


def klic_from_rif(rif: bytes) -> bytes:
    if len(rif) != 512:
        raise SelfError("RIF precisa ter 512 bytes")
    return rif[0x50:0x60]
