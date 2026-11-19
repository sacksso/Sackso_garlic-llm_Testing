#!/usr/bin/env python3
"""Pruebas de pc/savetool.py con partidas inventadas. Uso: python3 tests/savetool_test.py"""
import io
import os
import pickle
import struct
import subprocess
import sys
import tempfile
import types
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "pc", "savetool.py")
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    fails += not cond


def run(*args):
    r = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


tmp = tempfile.mkdtemp()


def make_bin(money):
    b = bytearray(os.urandom(0) + bytes(range(256)) * 4)   # 1024 bytes, not random: easy to read
    struct.pack_into("<i", b, 300, money)
    struct.pack_into("<f", b, 512, 3.5)
    b[700:708] = b"CityName"
    return bytes(b)


def make_zip(path, data, wrap=None):
    d = zlib.compress(data) if wrap == "zlib" else data
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("sce_sys/param.sfo", b"\x00PSF" + b"\x00" * 60)
        z.writestr("SAVEDATA.bin", d)


# ---- generic: info / trace / set on a zip with a data file, plain and zlib-wrapped
for wrap in (None, "zlib"):
    a, b = os.path.join(tmp, f"a_{wrap}.zip"), os.path.join(tmp, f"b_{wrap}.zip")
    make_zip(a, make_bin(1000), wrap)
    make_zip(b, make_bin(1350), wrap)
    rc, out = run("info", a)
    check(rc == 0 and "SAVEDATA.bin" in out, f"info lists the entries ({wrap})")
    rc, out = run("trace", a, b, "1000", "1350", "--type", "i32")
    check(rc == 0 and "posición 300" in out and "1 candidatas" in out, f"trace finds the offset ({wrap})")
    rc, out = run("diff", a, b)
    check("300 (0x12c)" in out and "i32 1000 -> 1350" in out, f"diff shows the change ({wrap})")
    rc, out = run("find", b, "1350", "--type", "i32")
    check("posición 300" in out, f"find ({wrap})")
    o = os.path.join(tmp, f"edit_{wrap}.zip")
    rc, out = run("set", b, "300", "i32", "999999", "-o", o)
    check(rc == 0 and os.path.exists(o), f"set writes a new file ({wrap})")
    with zipfile.ZipFile(o) as z:
        d = z.read("SAVEDATA.bin")
        d = zlib.decompress(d) if wrap == "zlib" else d
        check(struct.unpack_from("<i", d, 300)[0] == 999999 and d[700:708] == b"CityName", f"the value changed and the rest did not ({wrap})")
        check(z.read("sce_sys/param.sfo").startswith(b"\x00PSF"), f"the other entries are kept ({wrap})")
    rc, out = run("set", b, "300", "i32", "5", "-o", b)
    check(rc != 0 and "sobrescribe" in out, f"refuses to overwrite the original ({wrap})")
    rc, out = run("set", b, "5000", "i32", "5", "-o", o)
    check(rc != 0 and "fuera" in out, f"refuses an offset outside the data ({wrap})")
rc, out = run("hex", os.path.join(tmp, "a_None.zip"), "--offset", "296", "--len", "16")
check("0x" not in out and "2c" in out.lower() or "e8 03 00 00" in out, "hex shows the bytes")
rc, out = run("strings", os.path.join(tmp, "a_None.zip"), "-n", "8")
check("CityName" in out, "strings")

# ---- Ren'Py: classes of a game that the tool must not need
mod = types.ModuleType("fakegame")
sys.modules["fakegame"] = mod
exec('''
class Player(object):
    def __init__(self):
        self.name = "Ana"
        self.cash = 250
        self.stats = {"charm": 3, "wit": 7}
class Inv(list):
    pass
class Roll(object):
    def __init__(self, store):
        self.stores = store
''', mod.__dict__)
for proto in (2, 4):
    p = mod.Player()
    inv = mod.Inv(["key", "map"])
    shared = {"money": 250, "player": p, "inv": inv, "love": 1.5, "met": True, "title": "Hola"}
    roots = {"store": shared, "_ver": 1}
    log = [mod.Roll({"store": {"money": 100}}), mod.Roll({"store": {"money": 250}})]
    blob = pickle.dumps((roots, log), proto)
    path = os.path.join(tmp, f"slot{proto}.save")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("screenshot.png", b"\x89PNG....")
        z.writestr("json", '{"_save_name": "Test"}')
        z.writestr("log", blob)
        z.writestr("signatures", "sig")
    rc, out = run("renpy-dump", path, "--grep", "money")
    check(rc == 0 and "money = 250" in out and "money = 100" in out, f"renpy-dump finds money in the state and in the rollback ({proto})")
    rc, out = run("renpy-dump", path)
    check("title = 'Hola'" in out and "cash = 250" in out, f"renpy-dump shows attributes of the game's objects ({proto})")
    o = os.path.join(tmp, f"slot{proto}.edit")
    rc, out = run("renpy-set", path, "money", "999999", "-o", o)
    check(rc == 0 and "3 sitio(s)" in out, f"renpy-set changes every copy ({proto}): {out.strip().splitlines()[-2:]}")
    with zipfile.ZipFile(o) as z:
        check(set(z.namelist()) == {"screenshot.png", "json", "log", "signatures"}, f"all entries kept ({proto})")
        new_blob = z.read("log")
        check(z.read("screenshot.png") == b"\x89PNG....", f"screenshot untouched ({proto})")
    # the real game's classes read the edited pickle back
    roots2, log2 = pickle.loads(new_blob)
    check(roots2["store"]["money"] == 999999 and log2[0].stores["store"]["money"] == 999999, f"the game's own classes load the edited save ({proto})")
    check(isinstance(roots2["store"]["player"], mod.Player) and roots2["store"]["player"].cash == 250, f"the objects are intact ({proto})")
    check(isinstance(roots2["store"]["inv"], mod.Inv) and list(roots2["store"]["inv"]) == ["key", "map"], f"list subclass intact ({proto})")
    check(roots2["store"]["love"] == 1.5 and roots2["store"]["met"] is True and roots2["store"]["player"].stats == {"charm": 3, "wit": 7}, f"other values intact ({proto})")
    rc, out = run("renpy-set", path, "cash", "7", "-o", o)
    check(rc == 0, f"renpy-set on an object attribute ({proto})")
    r3, _ = pickle.loads(zipfile.ZipFile(o).read("log"))
    check(r3["store"]["player"].cash == 7, f"attribute changed ({proto})")
    rc, out = run("renpy-set", path, "met", "false", "-o", o)
    check(rc == 0 and pickle.loads(zipfile.ZipFile(o).read("log"))[0]["store"]["met"] is False, f"booleans ({proto})")
    rc, out = run("renpy-set", path, "nope", "1", "-o", o)
    check(rc != 0 and "No encuentro" in out, f"unknown name explained ({proto})")
    rc, out = run("renpy-set", path, "money", "abc", "-o", o)
    check(rc != 0, f"a non-number for a number is refused ({proto})")

# a malicious pickle is not run
class Evil:
    def __reduce__(self):
        return (os.system, ("echo PWNED > " + os.path.join(tmp, "pwned"),))
evil = os.path.join(tmp, "evil.save")
with zipfile.ZipFile(evil, "w") as z:
    z.writestr("log", pickle.dumps(({"store": {"x": Evil()}}, []), 2))
rc, out = run("renpy-dump", evil)
check(not os.path.exists(os.path.join(tmp, "pwned")), "a save with a booby-trapped pickle runs nothing")
check(run("renpy-dump", os.path.join(tmp, "a_None.zip"))[0] != 0, "a zip that is not a Ren'Py save is explained")

print("\n" + ("todo bien" if not fails else f"{fails} fallos"))
sys.exit(1 if fails else 0)
