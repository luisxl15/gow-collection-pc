"""Extrai as codificações de bits das instruções USSE (dados do ISA) do comentário/tabela do
decodificador do Vita3K, para vitarecomp/usse/encodings.json. Só os padrões de bits e os nomes dos
campos são usados: o tradutor é nosso.

    python tools/usse_extract_encodings.py <vita3k>/shader/src/usse_translator_entry.cpp
"""
import json
import os
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read().splitlines()
out = {}
fields = {}
for line in src:
    m = re.match(r"\s*([A-Za-z]+) = ([a-z0-9_]+)", line)
    if m and len(set(m.group(1))) == 1:
        fields[m.group(1)[0]] = m.group(2)
        continue
    m = re.search(r'INST\(&V::([a-z0-9_]+), "[^"]*", "([01a-zA-Z-]{64})"\)', line)
    if m:
        name, bits = m.groups()
        letters = sorted(set(c for c in bits if c.isalpha()))
        out[name] = {"bits": bits, "fields": {c: fields.get(c, c) for c in letters}}
        fields = {}
dst = os.path.join(os.path.dirname(__file__), "..", "vitarecomp", "vitarecomp", "usse", "encodings.json")
json.dump(out, open(dst, "w", encoding="utf-8"), indent=1)
print(len(out), "classes ->", os.path.normpath(dst))
