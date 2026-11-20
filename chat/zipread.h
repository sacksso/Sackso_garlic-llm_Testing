// Reading .zip files (and the text of docx / pptx / xlsx / odt) without any library: a small inflate of our own.
// Only what a repo or a document needs: stored and deflate entries, no zip64, no encryption. Entries are read
// from the file on demand; only the first part of a big entry is decompressed (the caller says how much it wants).
#ifndef ZIPREAD_H
#define ZIPREAD_H

#include <stdint.h>

#define ZIP_NAME_MAX 120
#define ZIP_MAX_ENTS 2000

typedef struct {
    char name[ZIP_NAME_MAX];
    uint32_t csize, usize, off;
    uint16_t method;   // 0 stored, 8 deflate, 0xFFFF unsupported (encrypted or another method)
} ZipEnt;

// Reads the central directory of the zip at path. *ents points to an internal table (valid until the next call);
// *total is the real number of entries (the table keeps the first ZIP_MAX_ENTS files, directories left out).
// Returns the number of entries in the table, or: -1 cannot open, -2 not a zip, -3 zip64 / directory too big.
int zip_open_dir(const char* path, const ZipEnt** ents, int* total);

// Decompresses the start of an entry into out (NUL added). Returns the bytes written (>= 0), or < 0 on a damaged or
// unsupported entry. *cut = 1 when the entry is longer than what was written.
int zip_read(const char* path, const ZipEnt* e, char* out, int max, int* cut);

// Streaming: the whole uncompressed content of an entry of any size, in pieces, without holding it in memory (only a
// 32 KB window and two small buffers). sink gets consecutive pieces; it returns nonzero to stop early. Returns the bytes
// delivered (>= 0; compare with e->usize to know it was complete) or < 0: -1 cannot open, -2 bad local header, -3 damaged
// stream, -4 unsupported method. Not reentrant (do not call it from the sink).
typedef int (*ZipSink)(void* ctx, const uint8_t* p, int n);
long zip_stream(const char* path, const ZipEnt* e, ZipSink sink, void* ctx);

// The whole content of an entry written to the file dst (made, or replaced). 0 on success, < 0 as zip_stream, -5 when the
// size does not match what the zip says or the disk is full (dst is removed then).
int zip_extract(const char* path, const ZipEnt* e, const char* dst);

// Text of an office document: docx (word/document.xml), pptx (the slides), xlsx (the sheets, cells split by tabs), odt/ods/odp
// (content.xml). Returns the text length, or < 0 when path is not one of those or cannot be read. *cut as above.
int zip_office_text(const char* path, char* out, int max, int* cut);

#endif
