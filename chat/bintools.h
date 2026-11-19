// Binary files without a model that can read bytes (phase 10): saved games, config blobs, anything that is not text.
// The small model cannot do hex or arithmetic, so the app does it and hands the model short, exact facts:
//   @save.zip                 a list that also shows binary files (when nothing else is in the zip)
//   @save.zip:file            a binary file: a report (format, strings, entropy, first bytes in hex)
//   @save.zip:file:0x100-0x1ff   the bytes of that range in hex, with what they mean as numbers (u16/u32/f32...)
//   @save.zip?=12500          where the number 12500 is in every file of the zip (u16/u32/i32/u64/float/double)
//   @save.zip:file?=12500     the same, in one file
//   @before.zip~after.zip     which bytes changed between two saves (spend some money in the game, save again)
//   /poke OFFSET TYPE VALUE FILE [zip]   writes a NEW zip (name-mod.zip) with those bytes changed; the original is never touched
// No console calls here: it builds on zipread.h only, so the same code runs (and is tested) on a PC.
#ifndef BINTOOLS_H
#define BINTOOLS_H

#include "zipread.h"

#define BIN_MAX (6L * 1024 * 1024)   // the most of one file that is loaded

// 1 when the first bytes look like binary data (the test files.c uses for text).
int bin_looks_binary(const unsigned char* d, int n);

// A binary entry of a zip as text for the model. Without a range (ranged = 0): the report. With a range (byte
// offsets a..b, inclusive): the hex dump of those bytes (at most 512 per request). Returns the length written.
int bin_entry_view(const char* zip, const ZipEnt* e, long a, long b, int ranged, char* out, int max);

// Where a number is. file = NULL searches every entry. value: "12500", "-3", "0x1F4" or "12.5". Returns the length or < 0
// (-1 zip not readable, -2 bad value, -3 no such file in the zip).
int bin_zip_find(const char* zip, const char* file, const char* value, char* out, int max);

// The bytes that differ between the same files of two zips. Returns the length or < 0 (-1 cannot read one of them).
int bin_zip_compare(const char* za, const char* zb, char* out, int max);

// Writes dst (a copy of src, every entry stored) with the bytes of `entry` at `off` replaced. type: u8 u16 u32 u64 i8
// i16 i32 i64 f32 f64 (little endian, like the PS4's CPU); value as in bin_zip_find. msg gets a line for the screen.
// Returns 0 on success (the new zip was read back and the bytes checked) or < 0.
int bin_poke(const char* src, const char* dst, const char* entry, const char* off, const char* type, const char* value,
             char* msg, int msg_max);

// "12500" or "0x1F4" (also with a sign) -> number; returns 1 when the whole text was a number.
int bin_parse_num(const char* s, long long* v);

#endif
