// Saved games and other big binary files, at the level of bits (phase 13).
// The first binary tools (bintools.c) read a whole entry into memory (6 MB at most) and only saw the first 128 KB of
// what a zip compresses; a real saved game (EA FC: 15.7 MB) is bigger than that, and its fields are often packed in
// bits, not bytes, so searching for a number by bytes finds nothing. This module:
//   - extracts an entry once to a cache file (zip_extract: streaming, any size) and works on that file with pread;
//   - reports what a file is made of (zones, text, signatures) from the whole file, not its first 4 KB;
//   - searches a number at every bit position, in both bit orders (and says how many leading zeros could belong to it);
//   - compares two saves and reads each changed spot as a bit field (old -> new);
//   - identifies a checksum (CRC32, Adler32, sums) given where it is stored;
//   - writes a patched copy of the zip (bit field or bytes), never touching the original.
// Bit order: "LSB-first" numbers the bits of a byte from the least significant one (0 = lowest), the usual for packed
// little-endian data; "MSB-first" from the most significant one (0 = highest), like a bit stream read from the left.
// No console calls: it builds on zipread.h only, so it is tested on a PC.
#ifndef BITSCAN_H
#define BITSCAN_H

#include "zipread.h"

#define BITS_BIG (192L * 1024)   // entries bigger than this go through the cache file and the streaming tools

// The entry as a plain file in the cache (extracted the first time; the last few are kept). 0 on success, < 0 as zip_extract.
int bits_raw(const char* zip, const ZipEnt* e, char* raw, int max);

// What the file is made of. Returns the length written.
int bits_report(const char* raw, const char* label, char* out, int max);

// Where a number is stored as a bit field. spec: "5000" or "0x1388", optionally "+0x3A41F" (or @ or ,) to look only near that offset
// (+-64 bytes). Returns the length, or < 0 (-1 cannot read, -2 bad value).
int bits_find(const char* raw, const char* label, const char* spec, char* out, int max);

// What differs between two files, each changed spot read as a bit field. hint (or NULL): "OLD/NEW" (or OLD>NEW), the value the game
// showed before and after (e.g. "5000/5100"): the spots where some field of up to 32 bits reads exactly like that are listed
// first with their exact position and width. Returns the length, or -1.
int bits_diff(const char* rawa, const char* rawb, const char* label, const char* hint, char* out, int max);

// Which checksum is stored (4 bytes, little or big endian) at byte offset field. Returns the length, or -1.
int bits_checksum(const char* raw, long field, char* out, int max);

// rawout = rawin with `width` bits (1..64) at byte `byteoff`, bit `bit` (0..7, in the order's own numbering) set to value.
// *old gets the previous value. 0 on success; -1 cannot read/write, -2 value does not fit, -3 outside the file.
int bits_patch(const char* rawin, const char* rawout, long byteoff, int bit, int width, unsigned long long value, int msb,
               unsigned long long* old);
// The same with whole bytes.
int bits_patch_bytes(const char* rawin, const char* rawout, long off, const unsigned char* b, int n);

// dst = a copy of the zip src (every entry stored) where `entry` has the contents of the file newraw. The new zip is
// read back and compared. 0 on success, < 0 with a sentence in msg.
int bits_zip_replace(const char* src, const char* dst, const char* entry, const char* newraw, char* msg, int msgmax);

#endif
