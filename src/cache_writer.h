#ifndef CACHE_WRITER_H
#define CACHE_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CACHE_HASH_LEN 33
#define CACHE_MAX_PATH 1024

typedef struct {
	time_t mtime;
	int64_t size;
	int console_id;
	char hash[CACHE_HASH_LEN];
	char* rel_path;
} ScraperIndexEntry;

typedef struct {
	ScraperIndexEntry* entries;
	size_t count;
	size_t capacity;
	bool dirty;
} ScraperIndex;

/** Ensure directory and all parent directories exist. */
bool cache_mkdirs(const char* path);

/** Build paths under <shared_userdata>/.ra/... */
void cache_login_path(const char* userdata_dir, char* out, size_t out_size);
void cache_achievementsets_path(const char* userdata_dir, const char* game_hash, char* out, size_t out_size);
void cache_startsession_path(const char* userdata_dir, const char* game_hash, char* out, size_t out_size);
void cache_badge_path(const char* userdata_dir, const char* badge_name, bool locked, char* out, size_t out_size);
void cache_index_path(const char* userdata_dir, char* out, size_t out_size);

/**
 * Write NextUI binary cache file: [uint32_t body_len][body][32-byte SHA-256].
 * Writes via temporary file and renames atomically on success.
 */
bool cache_write_file(const char* path, const char* body, size_t len);

/**
 * Read and SHA-256 verify a NextUI binary cache file.
 * On success, allocates NUL-terminated *out_body (caller frees) and sets *out_len.
 */
bool cache_read_file(const char* path, char** out_body, size_t* out_len);

/** Verify an existing binary cache file's SHA-256 without retaining its body. */
bool cache_verify_file(const char* path);

/** Check whether a non-empty file exists on disk (used for badges). */
bool cache_file_exists_nonempty(const char* path);

void scraper_index_init(ScraperIndex* idx);
void scraper_index_free(ScraperIndex* idx);
bool scraper_index_load(ScraperIndex* idx, const char* path);
bool scraper_index_save(ScraperIndex* idx, const char* path);

/**
 * Look up an entry by relative path and console ID. Returns NULL if missing or
 * if mtime/size do not match.
 */
const char* scraper_index_lookup(const ScraperIndex* idx, const char* rel_path,
                                 int console_id, time_t mtime, int64_t size);

/** Insert or update an entry in the scraper index and mark dirty. */
bool scraper_index_upsert(ScraperIndex* idx, const char* rel_path,
                          int console_id, time_t mtime, int64_t size, const char* hash);

#endif
