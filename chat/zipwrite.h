// Packs a folder into a .zip (stored, no compression), so the files the AI wrote (/data/garlic_files/out) leave the
// console as ONE file over FTP. Phase 7.
#ifndef ZIPWRITE_H
#define ZIPWRITE_H

// Every regular file under dir (4 levels, at most 300 files, each up to 1 MB) goes into zip_path with its path
// relative to dir. Returns the number of files packed (>= 0) or < 0: -1 cannot create the zip, -2 nothing to pack.
// *bytes gets the size of the zip.
int zipwrite_dir(const char* dir, const char* zip_path, long* bytes);

#endif
