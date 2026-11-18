#ifndef ROM_HASHER_H
#define ROM_HASHER_H

#include <stdbool.h>

#define ROM_HASH_LEN 33

/**
 * Return true if the file should be skipped during ROM scanning
 * (hidden files, non-ROM metadata/save extensions, or .bin files covered by a .cue).
 */
bool rom_should_skip(const char* file_path, const char* dir_path, const char* file_name);

/**
 * Compute the RetroAchievements MD5 hash for a ROM file or .zip archive.
 * out_hash must have space for at least ROM_HASH_LEN (33) bytes.
 */
bool rom_hash_file(const char* path, int console_id, char out_hash[ROM_HASH_LEN]);

#endif
