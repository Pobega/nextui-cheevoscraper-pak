#define _POSIX_C_SOURCE 200809L
#include "cache_writer.h"

#include <dirent.h>
#include <errno.h>
#ifndef CACHE_WRITER_NO_CRYPTO
#include <openssl/evp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CACHE_WRITER_NO_CRYPTO
#define CACHE_SHA256_SIZE 32
#define CACHE_MAX_BODY_SIZE (8u * 1024u * 1024u)
#endif
#define INDEX_INITIAL_CAP 256
#define DIR_MODE 0755

bool cache_mkdirs(const char* path) {
	if (!path || !*path) {
		return false;
	}

	char buf[CACHE_MAX_PATH];
	int n = snprintf(buf, sizeof(buf), "%s", path);
	if (n < 0 || (size_t)n >= sizeof(buf)) {
		return false;
	}

	size_t len = (size_t)n;
	if (len > 1 && buf[len - 1] == '/') {
		buf[len - 1] = '\0';
	}

	for (char* p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, DIR_MODE) != 0 && errno != EEXIST) {
				return false;
			}
			*p = '/';
		}
	}

	if (mkdir(buf, DIR_MODE) != 0 && errno != EEXIST) {
		return false;
	}
	return true;
}

void cache_login_path(const char* userdata_dir, char* out, size_t out_size) {
	snprintf(out, out_size, "%s/.ra/offline/cache/login.bin", userdata_dir);
}

void cache_achievementsets_path(const char* userdata_dir, const char* game_hash, char* out, size_t out_size) {
	snprintf(out, out_size, "%s/.ra/offline/cache/achievementsets_%s.bin", userdata_dir, game_hash);
}

void cache_startsession_path(const char* userdata_dir, const char* game_hash, char* out, size_t out_size) {
	snprintf(out, out_size, "%s/.ra/offline/cache/startsession_%s.bin", userdata_dir, game_hash);
}

void cache_badge_path(const char* userdata_dir, const char* badge_name, bool locked, char* out, size_t out_size) {
	snprintf(out, out_size, "%s/.ra/badges/%s%s.png",
	         userdata_dir, badge_name, locked ? "_lock" : "");
}

void cache_index_path(const char* userdata_dir, char* out, size_t out_size) {
	snprintf(out, out_size, "%s/.ra/offline/scraper_index.tsv", userdata_dir);
}

#ifndef CACHE_WRITER_NO_CRYPTO
bool cache_write_file(const char* path, const char* body, size_t len) {
	if (!path || !body || len > CACHE_MAX_BODY_SIZE) {
		return false;
	}

	uint8_t digest[CACHE_SHA256_SIZE];
	if (EVP_Digest(body, len, digest, NULL, EVP_sha256(), NULL) != 1) {
		return false;
	}

	char tmp_path[CACHE_MAX_PATH];
	int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		return false;
	}

	FILE* f = fopen(tmp_path, "wb");
	if (!f) {
		return false;
	}

	uint32_t len32 = (uint32_t)len;
	bool ok = fwrite(&len32, sizeof(len32), 1, f) == 1 &&
	          (len == 0 || fwrite(body, 1, len, f) == len) &&
	          fwrite(digest, CACHE_SHA256_SIZE, 1, f) == 1;

	if (ok) {
		fflush(f);
		fsync(fileno(f));
	}
	fclose(f);

	if (!ok || rename(tmp_path, path) != 0) {
		unlink(tmp_path);
		return false;
	}
	return true;
}

bool cache_read_file(const char* path, char** out_body, size_t* out_len) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}

	uint32_t len32 = 0;
	if (fread(&len32, sizeof(len32), 1, f) != 1 || len32 > CACHE_MAX_BODY_SIZE) {
		fclose(f);
		return false;
	}

	char* body = malloc((size_t)len32 + 1);
	if (!body) {
		fclose(f);
		return false;
	}

	if (len32 > 0 && fread(body, 1, len32, f) != len32) {
		free(body);
		fclose(f);
		return false;
	}
	body[len32] = '\0';

	uint8_t stored[CACHE_SHA256_SIZE];
	if (fread(stored, CACHE_SHA256_SIZE, 1, f) != 1) {
		free(body);
		fclose(f);
		return false;
	}
	fclose(f);

	uint8_t computed[CACHE_SHA256_SIZE];
	if (EVP_Digest(body, len32, computed, NULL, EVP_sha256(), NULL) != 1 ||
	    memcmp(stored, computed, CACHE_SHA256_SIZE) != 0) {
		free(body);
		return false;
	}

	if (out_body) {
		*out_body = body;
	} else {
		free(body);
	}
	if (out_len) {
		*out_len = (size_t)len32;
	}
	return true;
}

bool cache_verify_file(const char* path) {
	return cache_read_file(path, NULL, NULL);
}
#endif

bool cache_file_exists_nonempty(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

#ifndef CACHE_WRITER_NO_CRYPTO
bool cache_has_valid_game(const char* userdata_dir, const char* game_hash) {
	if (!userdata_dir || !game_hash || !*game_hash) {
		return false;
	}
	char ach_path[CACHE_MAX_PATH];
	cache_achievementsets_path(userdata_dir, game_hash, ach_path, sizeof(ach_path));
	return cache_verify_file(ach_path);
}
#endif

void scraper_index_init(ScraperIndex* idx) {
	memset(idx, 0, sizeof(*idx));
}

void scraper_index_free(ScraperIndex* idx) {
	for (size_t i = 0; i < idx->count; i++) {
		free(idx->entries[i].rel_path);
	}
	free(idx->entries);
	memset(idx, 0, sizeof(*idx));
}

static bool scraper_index_push(ScraperIndex* idx, const char* rel_path,
                               int console_id, time_t mtime, int64_t size, const char* hash) {
	if (idx->count == idx->capacity) {
		size_t new_cap = idx->capacity ? idx->capacity * 2 : INDEX_INITIAL_CAP;
		ScraperIndexEntry* grown = realloc(idx->entries, new_cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		idx->entries = grown;
		idx->capacity = new_cap;
	}

	char* path_copy = strdup(rel_path);
	if (!path_copy) {
		return false;
	}

	ScraperIndexEntry* e = &idx->entries[idx->count++];
	e->mtime = mtime;
	e->size = size;
	e->console_id = console_id;
	snprintf(e->hash, sizeof(e->hash), "%s", hash);
	e->rel_path = path_copy;
	return true;
}

bool scraper_index_load(ScraperIndex* idx, const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return false;
	}

	char line[2048];
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		if (len == 0) {
			continue;
		}

		char* p1 = strchr(line, '\t');
		char* p2 = p1 ? strchr(p1 + 1, '\t') : NULL;
		char* p3 = p2 ? strchr(p2 + 1, '\t') : NULL;
		char* p4 = p3 ? strchr(p3 + 1, '\t') : NULL;
		if (!p1 || !p2 || !p3 || !p4) {
			continue;
		}

		*p1 = '\0';
		*p2 = '\0';
		*p3 = '\0';
		*p4 = '\0';

		long long mtime_ll = strtoll(line, NULL, 10);
		long long size_ll = strtoll(p1 + 1, NULL, 10);
		int console_id = (int)strtol(p2 + 1, NULL, 10);
		const char* hash = p3 + 1;
		const char* rel_path = p4 + 1;

		if (!*hash || !*rel_path) {
			continue;
		}
		scraper_index_push(idx, rel_path, console_id, (time_t)mtime_ll, (int64_t)size_ll, hash);
	}

	fclose(f);
	idx->dirty = false;
	return true;
}

bool scraper_index_save(ScraperIndex* idx, const char* path) {
	if (!idx->dirty) {
		return true;
	}

	char tmp_path[CACHE_MAX_PATH];
	int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
		return false;
	}

	FILE* f = fopen(tmp_path, "w");
	if (!f) {
		return false;
	}

	for (size_t i = 0; i < idx->count; i++) {
		const ScraperIndexEntry* e = &idx->entries[i];
		if (fprintf(f, "%lld\t%lld\t%d\t%s\t%s\n",
		            (long long)e->mtime, (long long)e->size,
		            e->console_id, e->hash, e->rel_path) < 0) {
			fclose(f);
			unlink(tmp_path);
			return false;
		}
	}

	fflush(f);
	fsync(fileno(f));
	fclose(f);

	if (rename(tmp_path, path) != 0) {
		unlink(tmp_path);
		return false;
	}
	idx->dirty = false;
	return true;
}

const char* scraper_index_lookup(const ScraperIndex* idx, const char* rel_path,
                                 int console_id, time_t mtime, int64_t size) {
	for (size_t i = 0; i < idx->count; i++) {
		const ScraperIndexEntry* e = &idx->entries[i];
		if (e->console_id == console_id &&
		    e->mtime == mtime &&
		    e->size == size &&
		    strcmp(e->rel_path, rel_path) == 0) {
			return e->hash;
		}
	}
	return NULL;
}

bool scraper_index_upsert(ScraperIndex* idx, const char* rel_path,
                          int console_id, time_t mtime, int64_t size, const char* hash) {
	for (size_t i = 0; i < idx->count; i++) {
		ScraperIndexEntry* e = &idx->entries[i];
		if (e->console_id == console_id && strcmp(e->rel_path, rel_path) == 0) {
			e->mtime = mtime;
			e->size = size;
			snprintf(e->hash, sizeof(e->hash), "%s", hash);
			idx->dirty = true;
			return true;
		}
	}
	if (!scraper_index_push(idx, rel_path, console_id, mtime, size, hash)) {
		return false;
	}
	idx->dirty = true;
	return true;
}

static bool str_ends_with(const char* name, const char* suffix) {
	size_t nlen = strlen(name);
	size_t slen = strlen(suffix);
	return nlen > slen && strcmp(name + nlen - slen, suffix) == 0;
}

void cache_format_size(uint64_t bytes, char* out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	const uint64_t one_mib = 1024ULL * 1024ULL;
	if (bytes < one_mib) {
		uint64_t kb = (bytes + 512ULL) / 1024ULL;
		if (bytes > 0 && kb == 0) {
			kb = 1;
		}
		snprintf(out, out_size, "%llu KB", (unsigned long long)kb);
	} else {
		double mb = (double)bytes / (double)one_mib;
		snprintf(out, out_size, "%.1f MB", mb);
	}
}

static void scan_cache_bin_dir(const char* cache_dir, CacheClassifyGameFn classify_game, CacheStats* out) {
	DIR* dir = opendir(cache_dir);
	if (!dir) {
		return;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.' || !str_ends_with(ent->d_name, ".bin")) {
			continue;
		}

		char full_path[CACHE_MAX_PATH];
		if (snprintf(full_path, sizeof(full_path), "%s/%s", cache_dir, ent->d_name) >= (int)sizeof(full_path)) {
			continue;
		}

		struct stat st;
		if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}

		out->total_bytes += (uint64_t)st.st_size;

		if (strncmp(ent->d_name, "achievementsets_", 16) == 0) {
			int cls = classify_game ? classify_game(full_path) : 1;
			if (cls > 0) {
				out->recognized_games++;
			} else if (cls == 0) {
				out->unrecognized_games++;
			}
		}
	}

	closedir(dir);
}

static void scan_badges_dir(const char* badges_dir, CacheStats* out) {
	DIR* dir = opendir(badges_dir);
	if (!dir) {
		return;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.' || !str_ends_with(ent->d_name, ".png")) {
			continue;
		}

		char full_path[CACHE_MAX_PATH];
		if (snprintf(full_path, sizeof(full_path), "%s/%s", badges_dir, ent->d_name) >= (int)sizeof(full_path)) {
			continue;
		}

		struct stat st;
		if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}

		out->cached_badges++;
		out->total_bytes += (uint64_t)st.st_size;
	}

	closedir(dir);
}

void cache_collect_stats(const char* userdata_dir, CacheClassifyGameFn classify_game, CacheStats* out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!userdata_dir || !*userdata_dir) {
		return;
	}

	char cache_dir[CACHE_MAX_PATH];
	if (snprintf(cache_dir, sizeof(cache_dir), "%s/.ra/offline/cache", userdata_dir) < (int)sizeof(cache_dir)) {
		scan_cache_bin_dir(cache_dir, classify_game, out);
	}

	char badges_dir[CACHE_MAX_PATH];
	if (snprintf(badges_dir, sizeof(badges_dir), "%s/.ra/badges", userdata_dir) < (int)sizeof(badges_dir)) {
		scan_badges_dir(badges_dir, out);
	}

	char index_path[CACHE_MAX_PATH];
	cache_index_path(userdata_dir, index_path, sizeof(index_path));
	ScraperIndex idx;
	scraper_index_init(&idx);
	if (scraper_index_load(&idx, index_path)) {
		out->indexed_roms = idx.count;
	}
	scraper_index_free(&idx);
}
