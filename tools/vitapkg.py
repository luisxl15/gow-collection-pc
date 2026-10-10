#!/usr/bin/env python3
"""Leitor/extrator de PKG do PS Vita (camada externa, estilo pkg2zip).

Decifra apenas a camada do PKG (AES-128-CTR com a chave pública do formato).
Os arquivos de dentro continuam como estão no pacote: o app fica cifrado por
PFS e o eboot.bin é um SELF NPDRM. Essas duas camadas exigem a licença
(work.bin/zRIF) e são tratadas por outra etapa.

    python tools/vitapkg.py list  jogo.pkg
    python tools/vitapkg.py extract jogo.pkg saida/ [--only eboot.bin,sce_sys/]
"""
import argparse
import hashlib
import json
import os
import struct
import sys

from Crypto.Cipher import AES
from Crypto.Util import Counter

# Chaves públicas do formato PKG (as mesmas do pkg2zip / Vita3K).
PKG_PSP_KEY = bytes.fromhex("07f2c68290b50d2c33818d709b60e62b")
PKG_VITA = {
    2: bytes.fromhex("e31a70c9ce1dd72bf3c0622963f2eccb"),
    3: bytes.fromhex("423aca3a2bd5649f9686abad6fd8801f"),
    4: bytes.fromhex("af07fd59652527baf13389668b28a902"),
}

DIR_FLAGS = (4, 18)


class Pkg:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(0x100)
        if h[:4] != b"\x7fPKG":
            raise ValueError("não é um PKG")
        self.header = h
        self.meta_offset, self.meta_count = struct.unpack(">II", h[8:16])
        self.item_count = struct.unpack(">I", h[20:24])[0]
        self.total_size, self.enc_offset, self.enc_size = struct.unpack(">QQQ", h[24:48])
        self.content_id = h[0x30:0x60].split(b"\0")[0].decode()
        self.iv = h[0x70:0x80]
        self.key_type = h[0xE7] & 7
        if self.key_type == 1:
            self.main_key = PKG_PSP_KEY
        elif self.key_type in PKG_VITA:
            self.main_key = AES.new(PKG_VITA[self.key_type], AES.MODE_ECB).encrypt(self.iv)
        else:
            raise ValueError(f"key_type {self.key_type} desconhecido")
        self.iv_int = int.from_bytes(self.iv, "big")
        self.items = self._read_items()

    def read_dec(self, rel_off, size):
        """Lê `size` bytes decifrados a partir de enc_offset + rel_off."""
        start = rel_off & ~15
        skip = rel_off - start
        self.f.seek(self.enc_offset + start)
        raw = self.f.read(skip + size)
        ctr = Counter.new(128, initial_value=(self.iv_int + start // 16) & ((1 << 128) - 1))
        return AES.new(self.main_key, AES.MODE_CTR, counter=ctr).decrypt(raw)[skip:]

    def _read_items(self):
        table = self.read_dec(0, self.item_count * 32)
        items = []
        for i in range(self.item_count):
            e = table[i * 32:(i + 1) * 32]
            name_off, name_size, data_off, data_size = struct.unpack(">IIQQ", e[:24])
            flags = e[27]
            name = self.read_dec(name_off, name_size).decode("utf-8", "replace")
            items.append(dict(name=name, offset=data_off, size=data_size, flags=flags,
                              is_dir=flags in DIR_FLAGS))
        return items

    def iter_data(self, item, chunk=8 << 20):
        pos, left = item["offset"], item["size"]
        while left:
            n = min(chunk, left)
            yield self.read_dec(pos, n)
            pos += n
            left -= n


def cmd_list(args):
    p = Pkg(args.pkg)
    info = dict(content_id=p.content_id, key_type=p.key_type, items=p.item_count,
                total_size=p.total_size, enc_offset=p.enc_offset, enc_size=p.enc_size)
    if args.json:
        info["entries"] = p.items
        json.dump(info, sys.stdout, indent=1, ensure_ascii=False)
        return
    for k, v in info.items():
        print(f"{k:12} {v}")
    for it in p.items:
        kind = "d" if it["is_dir"] else "f"
        print(f"{kind} {it['flags']:3d} {it['size']:12d}  {it['name']}")


def cmd_extract(args):
    p = Pkg(args.pkg)
    only = [s for s in (args.only or "").split(",") if s]
    os.makedirs(args.out, exist_ok=True)
    manifest = []
    for it in p.items:
        if only and not any(it["name"] == o or it["name"].startswith(o) for o in only):
            continue
        dst = os.path.join(args.out, *it["name"].split("/"))
        if it["is_dir"]:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        sha = hashlib.sha256()
        with open(dst, "wb") as o:
            for blk in p.iter_data(it):
                sha.update(blk)
                o.write(blk)
        manifest.append(dict(name=it["name"], size=it["size"], sha256=sha.hexdigest()))
        print(f"{it['size']:12d}  {it['name']}", flush=True)
    with open(os.path.join(args.out, "_pkg_manifest.json"), "w") as o:
        json.dump(dict(content_id=p.content_id, files=manifest), o, indent=1)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("list"); a.add_argument("pkg"); a.add_argument("--json", action="store_true")
    a.set_defaults(fn=cmd_list)
    a = sub.add_parser("extract"); a.add_argument("pkg"); a.add_argument("out"); a.add_argument("--only")
    a.set_defaults(fn=cmd_extract)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
