#!/usr/bin/env python3
"""Herramienta para inspeccionar y modificar partidas guardadas (solo biblioteca estándar).

Nunca sobrescribe el original: todo lo que escribe va a un archivo nuevo (<nombre>.editado por defecto).

  Cualquier archivo o zip (partidas de PS4 exportadas, Unity, etc.)
    savetool.py info    ARCHIVO                      qué es: zip/gzip/zlib/JSON/SQLite..., entradas, tamaños, texto visible
    savetool.py hex     ARCHIVO [--entry E] [--offset N] [--len N]
    savetool.py strings ARCHIVO [--entry E] [-n 6]
    savetool.py find    ARCHIVO VALOR [--entry E] [--type i32|u32|i16|u16|i64|f32|f64|all]
    savetool.py trace   ANTES DESPUES VIEJO NUEVO [--entry E] [--type ...]   (el truco clásico, ver abajo)
    savetool.py diff    ANTES DESPUES [--entry E]
    savetool.py set     ARCHIVO OFFSET TIPO VALOR [--entry E] [-o SALIDA]

  Partidas de Ren'Py (.save), p. ej. de juegos hechos con Ren'Py en PC o Android
    savetool.py renpy-dump ARCHIVO.save [--grep texto]
    savetool.py renpy-set  ARCHIVO.save NOMBRE VALOR [-o SALIDA]

El truco para encontrar un valor (dinero, vidas...): guarda la partida con 1000 de dinero, gasta o gana algo para
que pase a otro número (por ejemplo 1350), guarda de nuevo y ejecuta
    savetool.py trace partida1.zip partida2.zip 1000 1350
Sale la lista de posiciones donde el 1000 pasó a 1350 (suele ser una o dos). Luego:
    savetool.py set partida2.zip POSICION i32 999999
"""
import argparse
import copyreg
import gzip
import io
import json
import math
import os
import pickle
import re
import struct
import sys
import zipfile
import zlib

TYPES = {"i32": "<i", "u32": "<I", "i16": "<h", "u16": "<H", "i64": "<q", "f32": "<f", "f64": "<d"}


# ---------------------------------------------------------------- loading ------------------------------------------

class Blob:
    """The bytes to work on, and how to wrap them back: a zip entry, a zlib/gzip body or the file itself."""

    def __init__(self, path, entry=None):
        self.path = path
        raw = open(path, "rb").read()
        self.kind = "raw"
        self.entry = None
        self.raw = raw
        self.zip_infos = None
        if raw[:4] == b"PK\x03\x04" or raw[:4] == b"PK\x05\x06":
            self.kind = "zip"
            with zipfile.ZipFile(io.BytesIO(raw)) as z:
                self.zip_infos = z.infolist()
                names = [i.filename for i in self.zip_infos if not i.is_dir()]
                if entry is None:
                    if len(names) == 1:
                        entry = names[0]
                    else:
                        data = [n for n in names if not n.startswith("sce_sys/") and not n.endswith((".png", ".jpg"))]
                        if len(data) == 1:
                            entry = data[0]
                if entry is not None:
                    match = [n for n in names if n == entry] or [n for n in names if n.endswith("/" + entry) or os.path.basename(n) == entry]
                    if not match:
                        sys.exit(f"No hay una entrada «{entry}» en el zip. Entradas: {', '.join(names)}")
                    self.entry = match[0]
                    self.data = z.read(self.entry)
                else:
                    self.data = None
                    self.names = names
        else:
            self.data = raw
        self.wrap = None   # "gzip" / "zlib" when the data was compressed in one piece
        if self.data is not None:
            self.data, self.wrap = unwrap(self.data)

    def need_data(self):
        if self.data is None:
            sys.exit("Este zip tiene varias entradas; elige una con --entry. Entradas: " + ", ".join(self.names))
        return self.data


def unwrap(data):
    if data[:2] == b"\x1f\x8b":
        try:
            return gzip.decompress(data), "gzip"
        except Exception:
            pass
    if len(data) > 2 and data[0] == 0x78 and data[1] in (0x01, 0x5E, 0x9C, 0xDA):
        try:
            return zlib.decompress(data), "zlib"
        except Exception:
            pass
    return data, None


def wrap_back(data, wrap):
    if wrap == "gzip":
        return gzip.compress(data)
    if wrap == "zlib":
        return zlib.compress(data, 6)
    return data


def entropy(b):
    if not b:
        return 0.0
    counts = [0] * 256
    for x in b[:1 << 20]:
        counts[x] += 1
    n = min(len(b), 1 << 20)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def describe(data):
    if data[:6] == b"SQLite":
        return "base de datos SQLite"
    if data[:4] == b"\x89PNG":
        return "imagen PNG"
    s = data[:2000].lstrip()
    if s[:1] in (b"{", b"["):
        try:
            json.loads(data.decode("utf-8"))
            return "JSON (texto, se puede editar a mano)"
        except Exception:
            return "parece JSON"
    if s[:5] == b"<?xml" or s[:1] == b"<":
        return "XML (texto)"
    if data[:2] == b"\x80\x02" or data[:2] == b"\x80\x04" or data[:2] == b"\x80\x05":
        return "pickle de Python"
    e = entropy(data)
    if e > 7.5:
        return f"datos muy aleatorios (entropía {e:.2f}): cifrados o comprimidos; no se pueden editar sin descifrar"
    printable = sum(1 for x in data[:4000] if 32 <= x < 127 or x in (9, 10, 13)) / max(1, min(len(data), 4000))
    if printable > 0.9:
        return "texto"
    return f"binario (entropía {e:.2f})"


# ---------------------------------------------------------------- values -------------------------------------------

def parse_value(text, typ):
    if typ in ("f32", "f64"):
        return float(text)
    return int(text, 0)


def pack(typ, value):
    try:
        return struct.pack(TYPES[typ], value)
    except struct.error as e:
        sys.exit(f"El valor {value} no cabe en el tipo {typ}: {e}")


def search(data, value_text, types):
    found = []
    for typ in types:
        try:
            v = parse_value(value_text, typ)
        except ValueError:
            continue
        try:
            pat = struct.pack(TYPES[typ], v)
        except struct.error:
            continue
        i = data.find(pat)
        while i >= 0:
            found.append((i, typ))
            i = data.find(pat, i + 1)
    return sorted(found)


def pick_types(t):
    if t == "all":
        return ["i32", "u32", "f32", "i16", "u16", "i64", "f64"]
    if t not in TYPES:
        sys.exit("Tipos: " + ", ".join(TYPES) + ", all")
    return [t]


# ---------------------------------------------------------------- commands -----------------------------------------

def cmd_info(a):
    b = Blob(a.file, a.entry)
    print(f"{a.file}: {len(b.raw)} bytes")
    if b.kind == "zip":
        print("Es un zip con estas entradas:")
        with zipfile.ZipFile(a.file) as z:
            for i in z.infolist():
                if i.is_dir():
                    continue
                d = z.read(i.filename)
                inner, wrap = unwrap(d)
                print(f"  {i.filename}  {i.file_size} bytes -> {describe(inner)}" + (f" (dentro hay {wrap})" if wrap else ""))
        if b.data is None:
            print("Usa --entry NOMBRE para mirar una entrada con hex/strings/find/set.")
            return
        print(f"\nEntrada elegida: {b.entry}")
    if b.wrap:
        print(f"Está comprimido ({b.wrap}); las órdenes trabajan sobre los datos ya descomprimidos ({len(b.data)} bytes).")
    print("Tipo:", describe(b.data))
    show_strings(b.data, 8, 25)


def show_strings(data, n, limit):
    found = re.findall(rb"[\x20-\x7e]{%d,}" % n, data)
    if found:
        print(f"Texto visible ({min(limit, len(found))} de {len(found)}):")
        for s in found[:limit]:
            print("  ", s.decode()[:100])


def cmd_strings(a):
    b = Blob(a.file, a.entry)
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % a.n, b.need_data()):
        print(f"{m.start():08x}  {m.group().decode()}")


def cmd_hex(a):
    d = Blob(a.file, a.entry).need_data()
    end = min(len(d), a.offset + a.len)
    for o in range(a.offset - a.offset % 16, end, 16):
        chunk = d[o:o + 16]
        print(f"{o:08x}  {chunk.hex(' '):<47}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)}")


def cmd_find(a):
    d = Blob(a.file, a.entry).need_data()
    res = search(d, a.value, pick_types(a.type))
    if not res:
        print("No aparece. Prueba otro --type, o el valor puede estar guardado distinto (multiplicado, cifrado...).")
    for off, typ in res[:200]:
        print(f"  posición {off} (0x{off:x})  como {typ}")
    if len(res) > 200:
        print(f"  ...y {len(res) - 200} más")
    print(f"{len(res)} coincidencias. Con dos partidas distintas, 'trace' deja casi una sola.")


def cmd_trace(a):
    d1 = Blob(a.before, a.entry).need_data()
    d2 = Blob(a.after, a.entry).need_data()
    hits = []
    for typ in pick_types(a.type):
        try:
            p1 = pack(typ, parse_value(a.old, typ))
            p2 = pack(typ, parse_value(a.new, typ))
        except (ValueError, SystemExit):
            continue
        for m in re.finditer(re.escape(p1), d1):
            o = m.start()
            if d2[o:o + len(p2)] == p2:
                hits.append((o, typ))
    if not hits:
        print("Ninguna posición pasó de", a.old, "a", a.new, "(¿los dos guardados son de la misma partida? ¿probaste --type all?)")
        return
    for o, typ in sorted(hits):
        print(f"  posición {o} (0x{o:x})  como {typ}: {a.old} -> {a.new}")
    print(f"{len(hits)} candidatas. Cambia una con: savetool.py set {a.after} POSICION TIPO NUEVO_VALOR")


def cmd_diff(a):
    d1 = Blob(a.before, a.entry).need_data()
    d2 = Blob(a.after, a.entry).need_data()
    if len(d1) != len(d2):
        print(f"Tamaños distintos: {len(d1)} y {len(d2)} (se compara la parte común)")
    n = min(len(d1), len(d2))
    i, ranges = 0, []
    while i < n:
        if d1[i] != d2[i]:
            j = i
            while j < n and (d1[j] != d2[j] or (j + 1 < n and d1[j + 1] != d2[j + 1])):
                j += 1
            ranges.append((i, j))
            i = j
        else:
            i += 1
    print(f"{len(ranges)} zonas distintas:")
    for s, e in ranges[:80]:
        old, new = d1[s:e], d2[s:e]
        extra = ""
        if e - s <= 8 and s % 4 == 0 and e - s <= 4:
            o4, n4 = d1[s:s + 4], d2[s:s + 4]
            if len(o4) == 4:
                extra = f"  (i32 {struct.unpack('<i', o4)[0]} -> {struct.unpack('<i', n4)[0]}, f32 {struct.unpack('<f', o4)[0]:.4g} -> {struct.unpack('<f', n4)[0]:.4g})"
        print(f"  {s} (0x{s:x}) +{e - s}: {old[:16].hex(' ')} -> {new[:16].hex(' ')}{extra}")
    if len(ranges) > 80:
        print(f"  ...y {len(ranges) - 80} más")


def out_path(a):
    return a.output or (a.file + ".editado")


def write_result(b, new_data, out):
    new_data = wrap_back(new_data, b.wrap)
    if b.kind == "zip":
        with zipfile.ZipFile(io.BytesIO(b.raw)) as zin, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                data = new_data if info.filename == b.entry else zin.read(info.filename)
                zi = zipfile.ZipInfo(info.filename, info.date_time)
                zi.compress_type = info.compress_type
                zi.external_attr = info.external_attr
                zout.writestr(zi, data)
    else:
        open(out, "wb").write(new_data)


def cmd_set(a):
    b = Blob(a.file, a.entry)
    d = bytearray(b.need_data())
    off = int(a.offset, 0)
    val = pack(a.type, parse_value(a.value, a.type))
    if off < 0 or off + len(val) > len(d):
        sys.exit(f"La posición {off} queda fuera de los datos ({len(d)} bytes).")
    print(f"En {off}: {bytes(d[off:off + len(val)]).hex(' ')} -> {val.hex(' ')}")
    d[off:off + len(val)] = val
    out = out_path(a)
    if os.path.abspath(out) == os.path.abspath(a.file):
        sys.exit("Por seguridad no se sobrescribe el original: elige otra salida con -o.")
    write_result(b, bytes(d), out)
    print("Guardado en", out)


# ---------------------------------------------------------------- Ren'Py -------------------------------------------

class StubBase(object):
    """Stand-in for a class of the game that is not here. It takes whatever the pickle puts in it (attributes, list
    items, dict entries, constructor arguments) and gives it back the same way when saved again."""
    _stub = True

    def __init__(self, *args):
        if args:
            self.__dict__["_garlic_args"] = args

    def append(self, x):
        self.__dict__.setdefault("_garlic_items", []).append(x)

    def extend(self, xs):
        self.__dict__.setdefault("_garlic_items", []).extend(xs)

    def __setitem__(self, k, v):
        self.__dict__.setdefault("_garlic_pairs", {})[k] = v

    def __setstate__(self, st):
        if isinstance(st, dict):
            self.__dict__.update(st)
        elif isinstance(st, tuple) and len(st) == 2 and (st[0] is None or isinstance(st[0], dict)) and isinstance(st[1], dict):
            if st[0]:
                self.__dict__.update(st[0])
            self.__dict__["_garlic_slots"] = dict(st[1])
        else:
            self.__dict__["_garlic_raw"] = st

    def __reduce_ex__(self, proto):
        d = self.__dict__
        state = {k: v for k, v in d.items() if not k.startswith("_garlic_")}
        if "_garlic_raw" in d:
            state = d["_garlic_raw"]
        elif "_garlic_slots" in d:
            state = (state or None, d["_garlic_slots"])
        items = iter(d["_garlic_items"]) if "_garlic_items" in d else None
        pairs = iter(d["_garlic_pairs"].items()) if "_garlic_pairs" in d else None
        if "_garlic_args" in d:
            return (type(self), d["_garlic_args"], state or None, items, pairs)
        return (copyreg.__newobj__, (type(self),), state or None, items, pairs)


class _Stubs:
    cache = {}

    @classmethod
    def get(cls, module, name):
        key = (module, name)
        if key not in cls.cache:
            cls.cache[key] = type(name, (StubBase,), {"__module__": module})
        return cls.cache[key]


SAFE = {("builtins", "set"), ("builtins", "frozenset"), ("builtins", "object"), ("builtins", "bytearray"),
        ("collections", "OrderedDict"), ("collections", "defaultdict"), ("collections", "deque"),
        ("copyreg", "_reconstructor"), ("copyreg", "__newobj__"), ("_codecs", "encode"),
        ("datetime", "datetime"), ("datetime", "date"), ("datetime", "timedelta"), ("builtins", "complex"),
        ("builtins", "int"), ("builtins", "float"), ("builtins", "str"), ("builtins", "list"), ("builtins", "dict"),
        ("builtins", "tuple"), ("builtins", "bool"), ("builtins", "bytes"), ("builtins", "slice"), ("builtins", "range"),
        ("__builtin__", "set"), ("__builtin__", "object"), ("__builtin__", "frozenset")}


class StubUnpickler(pickle.Unpickler):
    """Reads a pickle without importing the game: its classes become empty stand-ins with the same name.
    Nothing from the file is ever run."""

    def find_class(self, module, name):
        if (module, name) in SAFE:
            if module == "__builtin__":
                module = "builtins"
            return super().find_class(module, name)
        return _Stubs.get(module, name)


class StubPickler(pickle._Pickler):   # (the pure-Python pickler: it lets us write the stand-ins by name)
    def save_global(self, obj, name=None):
        if getattr(obj, "_stub", False):
            module, qual = obj.__module__, obj.__name__
            if self.proto >= 4:
                self.save(module)
                self.save(qual)
                self.write(pickle.STACK_GLOBAL)
            else:
                self.write(pickle.GLOBAL + module.encode("utf-8") + b"\n" + qual.encode("utf-8") + b"\n")
            self.memoize(obj)
            return
        return super().save_global(obj, name)


def load_renpy(path):
    raw = open(path, "rb").read()
    z = zipfile.ZipFile(io.BytesIO(raw))
    if "log" not in z.namelist():
        sys.exit("Esto no parece una partida de Ren'Py (el zip no tiene la entrada «log»). Prueba: savetool.py info " + path)
    blob = z.read("log")
    proto = blob[1] if blob[:1] == b"\x80" else 2
    obj = StubUnpickler(io.BytesIO(blob), encoding="latin-1", errors="surrogateescape").load()
    return raw, z, obj, proto


def walk(obj, path, seen, out, want):
    """Calls out(path, container, key, value) for every dict entry / attribute whose name satisfies want."""
    oid = id(obj)
    if oid in seen:
        return
    seen.add(oid)
    if isinstance(obj, dict):
        items = list(obj.items())
        for k, v in items:
            if isinstance(k, str) and want(k):
                out(path, obj, k, v)
            walk(v, path + [("[", k)], seen, out, want)
    elif isinstance(obj, (list, tuple, set, frozenset)):
        for i, v in enumerate(obj):
            walk(v, path + [("[", i)], seen, out, want)
    if getattr(obj, "__dict__", None) and not isinstance(obj, type):
        for k, v in list(obj.__dict__.items()):
            if want(k):
                out(path, obj.__dict__, k, v)
            walk(v, path + [(".", k)], seen, out, want)


def fmt_path(path):
    return "".join(f".{k}" if t == "." else f"[{k!r}]" for t, k in path) or "(raíz)"


def short(v):
    s = repr(v)
    return s if len(s) < 70 else s[:67] + "..."


def cmd_renpy_dump(a):
    raw, z, obj, proto = load_renpy(a.file)
    print(f"Ren'Py: {a.file}  (pickle protocolo {proto}; entradas: {', '.join(z.namelist())})")
    if "json" in z.namelist():
        try:
            print("Metadatos:", json.dumps(json.loads(z.read("json")), ensure_ascii=False)[:300])
        except Exception:
            pass
    pat = a.grep.lower() if a.grep else None
    rows = []

    def out(path, cont, key, val):
        if isinstance(val, (int, float, str, bool)) or val is None:
            rows.append((key, fmt_path(path), val))

    walk(obj, [], set(), out, lambda k: pat is None or pat in k.lower())
    seen = {}
    for key, path, val in rows:
        seen.setdefault((key, repr(val)), []).append(path)
    shown = 0
    for (key, val), paths in sorted(seen.items()):
        if pat is None and (key.startswith("_") or key.startswith("renpy")):
            continue
        print(f"  {key} = {short(eval(val)) if val and val[0] != '<' else val}   ({len(paths)} sitio{'s' if len(paths) != 1 else ''})")
        shown += 1
        if shown >= 300:
            print("  ... (usa --grep para filtrar)")
            break
    if not shown:
        print("Nada" + (f" que contenga «{a.grep}»." if pat else "."))
    print("\nPara cambiar uno: savetool.py renpy-set", a.file, "NOMBRE VALOR")


def parse_scalar(text, like):
    if isinstance(like, bool):
        if text.lower() in ("true", "1", "yes", "si", "sí"):
            return True
        if text.lower() in ("false", "0", "no"):
            return False
        sys.exit("Para un valor True/False escribe true o false.")
    if isinstance(like, int):
        return int(text, 0)
    if isinstance(like, float):
        return float(text)
    return text


def cmd_renpy_set(a):
    raw, z, obj, proto = load_renpy(a.file)
    changed = []

    def out(path, cont, key, val):
        if key == a.name and isinstance(val, (int, float, str, bool)):
            new = parse_scalar(a.value, val)
            if new != val or type(new) is not type(val):
                cont[key] = new
            changed.append((fmt_path(path), val, new))

    walk(obj, [], set(), out, lambda k: k == a.name)
    if not changed:
        sys.exit(f"No encuentro una variable «{a.name}» con valor simple (número, texto, True/False). Mira: savetool.py renpy-dump {a.file} --grep {a.name}")
    buf = io.BytesIO()
    StubPickler(buf, proto).dump(obj)
    out_file = a.output or (a.file + ".editado")
    if os.path.abspath(out_file) == os.path.abspath(a.file):
        sys.exit("Por seguridad no se sobrescribe el original: elige otra salida con -o.")
    with zipfile.ZipFile(out_file, "w", zipfile.ZIP_DEFLATED) as zo:
        for info in z.infolist():
            data = buf.getvalue() if info.filename == "log" else z.read(info.filename)
            zi = zipfile.ZipInfo(info.filename, info.date_time)
            zi.compress_type = info.compress_type
            zo.writestr(zi, data)
    for p, old, new in changed[:12]:
        print(f"  {p}: {short(old)} -> {short(new)}")
    print(f"{len(changed)} sitio(s) cambiados (también el historial de retroceso, para que el juego no lo deshaga). Guardado en {out_file}")
    print("Nota: Ren'Py puede preguntar si quieres cargar una partida «de otro dispositivo o modificada»: acepta.")


# ---------------------------------------------------------------- main ---------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add(name, fn, *args, **kw):
        s = sub.add_parser(name)
        for flags, opts in args:
            s.add_argument(*flags, **opts)
        s.set_defaults(fn=fn)
        return s

    entry = (["--entry"], {"help": "entrada del zip", "default": None})
    typ = (["--type"], {"default": "all"})
    add("info", cmd_info, (["file"], {}), entry)
    add("strings", cmd_strings, (["file"], {}), entry, (["-n"], {"type": int, "default": 6}))
    add("hex", cmd_hex, (["file"], {}), entry, (["--offset"], {"type": lambda x: int(x, 0), "default": 0}), (["--len"], {"type": int, "default": 256}))
    add("find", cmd_find, (["file"], {}), (["value"], {}), entry, typ)
    add("trace", cmd_trace, (["before"], {}), (["after"], {}), (["old"], {}), (["new"], {}), entry, typ)
    add("diff", cmd_diff, (["before"], {}), (["after"], {}), entry)
    add("set", cmd_set, (["file"], {}), (["offset"], {}), (["type"], {"choices": list(TYPES)}), (["value"], {}), entry, (["-o", "--output"], {"default": None}))
    add("renpy-dump", cmd_renpy_dump, (["file"], {}), (["--grep"], {"default": None}))
    add("renpy-set", cmd_renpy_set, (["file"], {}), (["name"], {}), (["value"], {}), (["-o", "--output"], {"default": None}))
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
