#!/usr/bin/env python3
"""Tests of the bit-level saved game tools (chat/bitscan.c) on a synthetic 9 MB save with a bit-packed field and a CRC32.
Run from the repo root: python3 tests/bits_test.py   (needs gcc and python3)."""
import os, random, subprocess, sys, tempfile, zipfile, zlib, shutil

W = tempfile.mkdtemp()
TOOL = os.path.join(W, "bits_tool")
subprocess.check_call(["gcc", "-std=gnu99", "-g", "-w", "-fsanitize=address,undefined", "-Ichat", f'-DBITS_RAW_DIR="{W}/raw"',
                       "tests/bits_tool.c", "chat/bintools.c", "chat/bitscan.c", "chat/zipread.c", "chat/htmltext.c", "-lm", "-o", TOOL])
fails = 0

def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", what)

def run(*a):
    r = subprocess.run([TOOL, *map(str, a)], capture_output=True, text=True)
    if r.returncode not in (0, 5):
        print("tool crashed", a, r.stderr[:300])
        check(False, "tool exit")
    return r.stdout

def setfield(buf, byteoff, bit, width, value, msb):
    for j in range(width):
        idx = bit + j
        pos = byteoff + idx // 8
        k = idx % 8
        sh = 7 - k if msb else k
        vb = (value >> (width - 1 - j)) & 1 if msb else (value >> j) & 1
        buf[pos] = (buf[pos] & ~(1 << sh)) | (vb << sh)

def getfield(buf, byteoff, bit, width, msb):
    v = 0
    for j in range(width):
        idx = bit + j
        k = idx % 8
        sh = 7 - k if msb else k
        b = (buf[byteoff + idx // 8] >> sh) & 1
        v |= (b << (width - 1 - j)) if msb else (b << j)
    return v

random.seed(5)
names = [b"Real Madrid", b"FC Barcelona", b"Aston Villa", b"J. Smith", b"Hamburger SV", b"Sevilla FC", b"Team FC", b"Career Mode"]
# 3 MB of dense but structured data (about 30% zero bytes, like a real table), not pure noise
_r = int.from_bytes(os.urandom(3_000_000), "big")
_m = int.from_bytes(bytes(0 if b < 77 else 255 for b in os.urandom(3_000_000)), "big")
body = bytearray((_r & _m).to_bytes(3_000_000, "big"))
for i in range(400):   # plain names here and there, like a career save
    n = random.choice(names)
    at = random.randrange(1000, 2_900_000)
    body[at:at + len(n)] = n
base = body + bytes(9_000_000 - len(body))   # 3 MB of data, the rest zeros: 9 MB, bigger than the 6 MB the old tools loaded
MONEY_AT, MONEY_BIT, MONEY_W = 0x5A3C7, 3, 17
BIG_AT, BIG_BIT, BIG_W, BIG_V = 0x2F1A5, 6, 25, 20_000_000   # a big number: few random look-alikes
CRC_AT = 0x40

def make_save(path, money, counter):
    d = bytearray(base)
    d[0x10:0x14] = counter.to_bytes(4, "little")
    d[0x200000:0x200004] = bytes.fromhex("EFBEADDE")      # 0xDEADBEEF, byte aligned
    d[0xFFFFE:0x100002] = bytes.fromhex("EFBEADDE")       # and across the 1 MB boundary the search reads by
    setfield(d, MONEY_AT, MONEY_BIT, MONEY_W, money, False)
    setfield(d, BIG_AT, BIG_BIT, BIG_W, BIG_V, False)
    d[CRC_AT:CRC_AT + 4] = bytes(4)
    d[CRC_AT:CRC_AT + 4] = zlib.crc32(bytes(d)).to_bytes(4, "little")   # CRC32 of the whole file with the field zeroed
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr("SAVE/sce_sys/param.sfo", b"\x00PSF" + bytes(100))
        z.writestr("SAVE/NAME", b"Career 1")
        z.writestr("SAVE/DATA", bytes(d))
    return d

A = os.path.join(W, "a.zip"); B = os.path.join(W, "b.zip")
da = make_save(A, 123456, 7)
db = make_save(B, 123999, 8)
E = "SAVE/DATA"

# 1. the report looks at the whole file
out = run("info", A, E)
check("9000000 bytes" in out and "Plain text" in out, "info: size and text")
check("NOT encrypted" in out, "info: verdict not encrypted\n" + out)
import re
check(re.search(r"Data from 0x[0-9A-F]+ to 0x2[0-9A-F]{5}\b", out) is not None, "info: the data ends where the zeros begin\n" + out)

# 2. bit-level search finds the field the byte search cannot
out = run("find", A, E, 123456)
check("LSB-first" in out and "too common to tell which is real" in out, "find: a 17-bit number has too many look-alikes and it says so\n" + out)
out = run("find", A, E, BIG_V)
check(f"byte 0x{BIG_AT:X} bit {BIG_BIT}: 25 bits" in out, "find: a 25-bit number is found by itself\n" + out)
out = run("find", A, E, f"123456+0x{MONEY_AT:X}")
check(f"byte 0x{MONEY_AT:X} bit {MONEY_BIT}" in out and "LSB-first: 1 hit" in out, "find near: exactly the field\n" + out)
out = run("find", A, E, "123456@0x100000")
check("LSB-first: 0 hit" in out, "find near elsewhere: nothing\n" + out)
check(run("find", A, E, "banana").startswith("err -2"), "find: bad value")

# 3. comparing two saves
out = run("diff", A, B, E)
check(f"0x{MONEY_AT:X}" in out and "LSB-first" in out, "diff: the money spot\n" + out)
check("[start of the file" in out, "diff: the counter/crc near the start are flagged")
out = run("diff", A, B, E, "123456>123999")
check(f"FIELD FOUND (LSB-first): byte 0x{MONEY_AT:X} bit {MONEY_BIT}, 17 bits: 123456 -> 123999" in out, "diff hint: exact field\n" + out)
out = run("diff", A, B, E, "5/6")
check("No changed spot reads as 5 -> 6" in out, "diff hint: no match is said\n" + out)
out = run("diff", A, A, E)
check("no byte differs" in out, "diff: identical")

# 4. which checksum
out = run("sum", A, E, CRC_AT)
check("MATCH: CRC32 over the whole file" in out, "checksum: CRC32 found\n" + out)
out = run("sum", A, E, 0x5000)
check("No match" in out, "checksum: a non checksum spot")

# 5. patches: the new zip has exactly the new bits and nothing else changed
P = os.path.join(W, "p.zip")
out = run("patch", A, E, hex(MONEY_AT), MONEY_BIT, MONEY_W, 100000, "lsb", P)
check("old=123456 replace=0" in out, "patch: done\n" + out)
with zipfile.ZipFile(P) as z:
    check(z.testzip() is None, "patched zip: CRCs are valid")
    dp = z.read(E)
    check(sorted(z.namelist()) == sorted(["SAVE/sce_sys/param.sfo", "SAVE/NAME", "SAVE/DATA"]), "patched zip: same entries")
    check(z.read("SAVE/NAME") == b"Career 1", "patched zip: other entries intact")
check(getfield(dp, MONEY_AT, MONEY_BIT, MONEY_W, False) == 100000, "patched zip: the field has the new value")
exp = bytearray(da); setfield(exp, MONEY_AT, MONEY_BIT, MONEY_W, 100000, False)
check(dp == bytes(exp), "patched zip: nothing else changed")
# MSB-first, odd place and width
out = run("patch", A, E, "0x1234", 5, 12, 3000, "msb", P)
with zipfile.ZipFile(P) as z:
    dp = z.read(E)
check("replace=0" in out and getfield(dp, 0x1234, 5, 12, True) == 3000, "patch msb: value read back\n" + out)
exp = bytearray(da); setfield(exp, 0x1234, 5, 12, 3000, True)
check(dp == bytes(exp), "patch msb: nothing else changed")
check("patch err -2" in run("patch", A, E, "0x1234", 0, 8, 300, "lsb", P), "patch: value does not fit")
check("patch err -3" in run("patch", A, E, "0x1000009", 0, 8, 1, "lsb", P), "patch: outside the file")
check("Refusing to overwrite" in run("patch", A, E, "0x10", 0, 8, 1, "lsb", A), "patch: never over the original")
with zipfile.ZipFile(A) as z:
    check(z.read(E) == bytes(da), "the original is untouched")

# 6. the entry points the app calls (bintools.c), on the 9 MB entry
out = run("b_find", A, E, "3735928559")   # 0xDEADBEEF
check("0x200000" in out and "0xFFFFE" in out, "bin_zip_find: byte search over a big entry, also across a chunk boundary\n" + out)
out = run("b_bits", A, E, f"{BIG_V}@0x{BIG_AT:X}")
check(f"byte 0x{BIG_AT:X} bit {BIG_BIT}: 25 bits" in out, "bin_zip_bits\n" + out)
check("ret=-3" in run("b_bits", A, "SAVE/NOPE", "5"), "bin_zip_bits: no such file")
out = run("b_hex", A, E, hex(BIG_AT), hex(BIG_AT + 20))
check(f"{BIG_AT & ~15:08X}" in out and "u32le=" in out, "bin_entry_view: hex of a range far inside a big entry\n" + out)
out = run("b_cmp", A, B, "123456>123999")
check("FIELD FOUND (LSB-first)" in out and "1 changed" in out, "bin_zip_compare_hint\n" + out)
out = run("b_sum", A, E, "0x40")
check("MATCH: CRC32 over the whole file" in out, "bin_checksum\n" + out)
check("ret=-2" in run("b_sum", A, E, "banana"), "bin_checksum: bad offset")
P2 = os.path.join(W, "p2.zip")
out = run("b_pokebits", A, E, f"0x{MONEY_AT:X}:{MONEY_BIT}", MONEY_W, 100000, "lsb", P2)
check("ret=0" in out and "123456 -> 100000" in out, "bin_pokebits\n" + out)
with zipfile.ZipFile(P2) as z:
    check(getfield(z.read(E), MONEY_AT, MONEY_BIT, MONEY_W, False) == 100000, "bin_pokebits: the zip has the value")
check("ret=-1" in run("b_pokebits", A, E, "0x1:9", 8, 1, "lsb", P2), "bin_pokebits: bad bit")
check("ret=-1" in run("b_pokebits", A, E, "0x1:0", 8, 1, "xyz", P2), "bin_pokebits: bad order")
out = run("b_poke", A, E, "0x100000", "u16", "4660", P2)   # 0x1234: /poke on an entry over 6 MB
with zipfile.ZipFile(P2) as z:
    dz = z.read(E)
check("ret=0" in out and dz[0x100000:0x100002] == bytes([0x34, 0x12]) and dz[:0x100000] == bytes(da)[:0x100000] and dz[0x100002:] == bytes(da)[0x100002:], "bin_poke over a 9 MB entry (was refused above 6 MB)\n" + out)

shutil.rmtree(W)
print("bits_test: FAILED" if fails else "bits_test: ok")
sys.exit(1 if fails else 0)
