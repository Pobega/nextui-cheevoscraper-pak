#define _POSIX_C_SOURCE 200809L
#include "rom_hasher.h"
#include "chd_reader.h"

#include <ctype.h>
#include <dirent.h>
#include <rc_consoles.h>
#include <rc_hash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zip.h>

#define ROM_ZIP_MAX_BYTES (64u * 1024u * 1024u)
#define CUE_READ_LINE_MAX 1024
#define CDREADER_HANDLE_MAGIC 0x43484448u

typedef struct {
	uint32_t magic;
	bool is_chd;
	void* inner_handle;
} cdreader_handle_t;

static bool cdreader_initialized = false;
static rc_hash_cdreader_t default_cdreader;

static void* wrap_cdreader_handle(void* inner_handle, bool is_chd) {
	if (!inner_handle) {
		return NULL;
	}

	cdreader_handle_t* wrapper = malloc(sizeof(cdreader_handle_t));
	if (!wrapper) {
		if (is_chd) {
			chd_close_track(inner_handle);
		} else if (default_cdreader.close_track) {
			default_cdreader.close_track(inner_handle);
		}
		return NULL;
	}

	wrapper->magic = CDREADER_HANDLE_MAGIC;
	wrapper->is_chd = is_chd;
	wrapper->inner_handle = inner_handle;
	return wrapper;
}

static cdreader_handle_t* unwrap_cdreader_handle(void* handle) {
	if (!handle) {
		return NULL;
	}
	cdreader_handle_t* wrapper = (cdreader_handle_t*)handle;
	if (wrapper->magic != CDREADER_HANDLE_MAGIC) {
		return NULL;
	}
	return wrapper;
}

static void* cdreader_open_track(const char* path, uint32_t track) {
	void* handle = chd_open_track(path, track);
	if (handle) {
		return wrap_cdreader_handle(handle, true);
	}
	if (default_cdreader.open_track) {
		handle = default_cdreader.open_track(path, track);
		if (handle) {
			return wrap_cdreader_handle(handle, false);
		}
	}
	return NULL;
}

static void* cdreader_open_track_iterator(const char* path, uint32_t track, const struct rc_hash_iterator* iterator) {
	void* handle = chd_open_track_iterator(path, track, iterator);
	if (handle) {
		return wrap_cdreader_handle(handle, true);
	}
	if (default_cdreader.open_track_iterator) {
		handle = default_cdreader.open_track_iterator(path, track, iterator);
		if (handle) {
			return wrap_cdreader_handle(handle, false);
		}
	}
	if (default_cdreader.open_track) {
		handle = default_cdreader.open_track(path, track);
		if (handle) {
			return wrap_cdreader_handle(handle, false);
		}
	}
	return NULL;
}

static size_t cdreader_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes) {
	cdreader_handle_t* wrapper = unwrap_cdreader_handle(track_handle);
	if (!wrapper) {
		return 0;
	}
	if (wrapper->is_chd) {
		return chd_read_sector(wrapper->inner_handle, sector, buffer, requested_bytes);
	}
	if (default_cdreader.read_sector) {
		return default_cdreader.read_sector(wrapper->inner_handle, sector, buffer, requested_bytes);
	}
	return 0;
}

static void cdreader_close_track(void* track_handle) {
	cdreader_handle_t* wrapper = unwrap_cdreader_handle(track_handle);
	if (!wrapper) {
		return;
	}
	if (wrapper->is_chd) {
		chd_close_track(wrapper->inner_handle);
	} else if (default_cdreader.close_track) {
		default_cdreader.close_track(wrapper->inner_handle);
	}
	wrapper->magic = 0;
	free(wrapper);
}

static uint32_t cdreader_first_track_sector(void* track_handle) {
	cdreader_handle_t* wrapper = unwrap_cdreader_handle(track_handle);
	if (!wrapper) {
		return 0;
	}
	if (wrapper->is_chd) {
		return chd_first_track_sector(wrapper->inner_handle);
	}
	if (default_cdreader.first_track_sector) {
		return default_cdreader.first_track_sector(wrapper->inner_handle);
	}
	return 0;
}

static void ensure_cdreader_initialized(void) {
	if (cdreader_initialized) {
		return;
	}
	rc_hash_get_default_cdreader(&default_cdreader);

	rc_hash_cdreader_t custom_reader = {
		.open_track = cdreader_open_track,
		.read_sector = cdreader_read_sector,
		.close_track = cdreader_close_track,
		.first_track_sector = cdreader_first_track_sector,
		.open_track_iterator = cdreader_open_track_iterator
	};
	rc_hash_init_custom_cdreader(&custom_reader);
	cdreader_initialized = true;
}

static const char* skip_extensions[] = {
	".txt", ".xml", ".png", ".jpg", ".jpeg", ".sav", ".srm",
	".rtc", ".m3u", ".sub", ".sbi", ".nfo", ".md", ".json",
	".cfg", ".state", ".oops", ".auto", ".diz"
};

#define SKIP_EXT_COUNT (sizeof(skip_extensions) / sizeof(skip_extensions[0]))

static const char* zip_skip_extensions[] = {
	".txt", ".nfo", ".xml", ".png", ".jpg", ".jpeg", ".diz", ".md"
};

#define ZIP_SKIP_EXT_COUNT (sizeof(zip_skip_extensions) / sizeof(zip_skip_extensions[0]))

static const char* get_extension(const char* name) {
	const char* dot = strrchr(name, '.');
	return dot ? dot : "";
}

static bool has_extension_in_list(const char* name, const char* const* list, size_t count) {
	const char* ext = get_extension(name);
	if (!*ext) {
		return false;
	}
	for (size_t i = 0; i < count; i++) {
		if (strcasecmp(ext, list[i]) == 0) {
			return true;
		}
	}
	return false;
}

static bool cue_references_bin(const char* cue_path, const char* bin_name) {
	FILE* f = fopen(cue_path, "r");
	if (!f) {
		return false;
	}

	char line[CUE_READ_LINE_MAX];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, bin_name) != NULL) {
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

static bool bin_has_matching_cue(const char* dir_path, const char* bin_name) {
	size_t name_len = strlen(bin_name);
	const char* dot = strrchr(bin_name, '.');
	size_t stem_len = dot ? (size_t)(dot - bin_name) : name_len;

	DIR* dir = opendir(dir_path);
	if (!dir) {
		return false;
	}

	bool matched = false;
	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		const char* ext = get_extension(ent->d_name);
		if (strcasecmp(ext, ".cue") != 0) {
			continue;
		}

		size_t ent_stem_len = strlen(ent->d_name) - strlen(ext);
		if (ent_stem_len == stem_len && strncasecmp(ent->d_name, bin_name, stem_len) == 0) {
			matched = true;
			break;
		}

		char cue_path[1024];
		if (snprintf(cue_path, sizeof(cue_path), "%s/%s", dir_path, ent->d_name) < (int)sizeof(cue_path)) {
			if (cue_references_bin(cue_path, bin_name)) {
				matched = true;
				break;
			}
		}
	}

	closedir(dir);
	return matched;
}

bool rom_should_skip(const char* file_path, const char* dir_path, const char* file_name) {
	(void)file_path;
	if (!file_name || !*file_name || file_name[0] == '.') {
		return true;
	}
	if (has_extension_in_list(file_name, skip_extensions, SKIP_EXT_COUNT)) {
		return true;
	}
	if (strcasecmp(get_extension(file_name), ".bin") == 0) {
		if (bin_has_matching_cue(dir_path, file_name)) {
			return true;
		}
	}
	return false;
}

static bool is_valid_zip_rom_entry(const char* entry_name, zip_uint64_t size) {
	if (!entry_name || !*entry_name || size == 0 || size > ROM_ZIP_MAX_BYTES) {
		return false;
	}
	size_t len = strlen(entry_name);
	if (entry_name[len - 1] == '/') {
		return false;
	}
	if (strncmp(entry_name, "__MACOSX/", 9) == 0) {
		return false;
	}

	const char* slash = strrchr(entry_name, '/');
	const char* base = slash ? slash + 1 : entry_name;
	if (base[0] == '.') {
		return false;
	}
	if (has_extension_in_list(base, zip_skip_extensions, ZIP_SKIP_EXT_COUNT)) {
		return false;
	}
	return true;
}

static bool hash_zip_rom(const char* path, int console_id, char out_hash[ROM_HASH_LEN]) {
	int err = 0;
	zip_t* za = zip_open(path, ZIP_RDONLY, &err);
	if (!za) {
		return false;
	}

	zip_int64_t num_entries = zip_get_num_entries(za, 0);
	zip_int64_t target_idx = -1;
	zip_uint64_t target_size = 0;

	for (zip_int64_t i = 0; i < num_entries; i++) {
		zip_stat_t st;
		if (zip_stat_index(za, (zip_uint64_t)i, 0, &st) != 0) {
			continue;
		}
		if (!(st.valid & ZIP_STAT_NAME) || !(st.valid & ZIP_STAT_SIZE)) {
			continue;
		}
		if (is_valid_zip_rom_entry(st.name, st.size)) {
			target_idx = i;
			target_size = st.size;
			break;
		}
	}

	if (target_idx < 0) {
		zip_close(za);
		return false;
	}

	uint8_t* buf = malloc((size_t)target_size);
	if (!buf) {
		zip_close(za);
		return false;
	}

	zip_file_t* zf = zip_fopen_index(za, (zip_uint64_t)target_idx, 0);
	if (!zf) {
		free(buf);
		zip_close(za);
		return false;
	}

	size_t total_read = 0;
	while (total_read < (size_t)target_size) {
		zip_int64_t n = zip_fread(zf, buf + total_read, (zip_uint64_t)((size_t)target_size - total_read));
		if (n <= 0) {
			break;
		}
		total_read += (size_t)n;
	}

	zip_fclose(zf);
	zip_close(za);

	bool hashed = false;
	if (total_read == (size_t)target_size) {
		hashed = rc_hash_generate_from_buffer(out_hash, (uint32_t)console_id, buf, total_read) != 0;
	}

	free(buf);
	return hashed;
}

bool rom_hash_file(const char* path, int console_id, char out_hash[ROM_HASH_LEN]) {
	out_hash[0] = '\0';
	if (!path || console_id == RC_CONSOLE_UNKNOWN) {
		return false;
	}

	ensure_cdreader_initialized();

	if (strcasecmp(get_extension(path), ".zip") == 0 && console_id != RC_CONSOLE_ARCADE) {
		return hash_zip_rom(path, console_id, out_hash);
	}

	return rc_hash_generate_from_file(out_hash, (uint32_t)console_id, path) != 0;
}
