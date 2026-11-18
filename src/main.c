#define _POSIX_C_SOURCE 200809L

#include "cache_writer.h"
#include "http_client.h"
#include "ra_consoles.h"
#include "rom_hasher.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <rc_api_runtime.h>
#include <rc_api_user.h>
#include <rc_error.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_USERDATA_PATH "/mnt/SDCARD/.userdata/shared"
#define DEFAULT_ROMS_PATH "/mnt/SDCARD/Roms"
#define RA_BADGE_BASE_URL "https://media.retroachievements.org/Badge/"
#define HTTP_STATUS_OK 200
#define PROGRESS_SCAN_MAX 30
#define PROGRESS_COMPLETE 100
#define SETTINGS_STR_MAX 256
#define INITIAL_LIST_CAP 64
#define JSON_FILE_MAX_BYTES (16u * 1024u * 1024u)

typedef struct {
	const char* userdata_dir;
	const char* roms_dir;
	const char* fifo_path;
	const char* system_folder;
	const char* sync_json_path;
	int delay_ms;
	int limit;
	bool dry_run;
	bool force;
	bool refresh_sessions;
} SyncOptions;

typedef struct {
	size_t roms_scanned;
	size_t index_hits;
	size_t roms_hashed;
	size_t hash_failures;
	size_t unique_games;
	size_t sets_fetched;
	size_t sets_cached;
	size_t sessions_fetched;
	size_t badges_downloaded;
	size_t errors;
} RunStats;

typedef struct {
	char username[SETTINGS_STR_MAX];
	char server_username[SETTINGS_STR_MAX];
	char token[SETTINGS_STR_MAX];
	int hardcore;
} RASettings;

typedef struct {
	char* abs_path;
	char* rel_path;
	int console_id;
	time_t mtime;
	int64_t size;
	char hash[ROM_HASH_LEN];
} RomEntry;

typedef struct {
	RomEntry* items;
	size_t count;
	size_t capacity;
} RomList;

typedef struct {
	char (*hashes)[ROM_HASH_LEN];
	char** sample_names;
	size_t count;
	size_t capacity;
} HashSet;

typedef struct {
	char** names;
	size_t count;
	size_t capacity;
} StringSet;

static int g_fifo_fd = -1;

static void fifo_open(const char* path) {
	if (!path || !*path) {
		return;
	}
	g_fifo_fd = open(path, O_WRONLY | O_NONBLOCK);
}

static void fifo_close(void) {
	if (g_fifo_fd >= 0) {
		close(g_fifo_fd);
		g_fifo_fd = -1;
	}
}

static void fifo_send_line(const char* fifo_path, const char* fmt, ...) {
	if (!fifo_path || !*fifo_path) {
		return;
	}
	if (g_fifo_fd < 0) {
		fifo_open(fifo_path);
		if (g_fifo_fd < 0) {
			return;
		}
	}

	char buf[512];
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
	va_end(args);
	if (n <= 0) {
		return;
	}
	if ((size_t)n >= sizeof(buf) - 1) {
		n = (int)(sizeof(buf) - 2);
	}
	buf[n++] = '\n';
	buf[n] = '\0';

	ssize_t written = write(g_fifo_fd, buf, (size_t)n);
	if (written < 0 && (errno == EPIPE || errno == EBADF)) {
		fifo_close();
		fifo_open(fifo_path);
		if (g_fifo_fd >= 0) {
			(void)write(g_fifo_fd, buf, (size_t)n);
		}
	}
}

static void ui_status(const SyncOptions* opts, int progress, const char* fmt, ...) {
	char msg[384];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	printf("%s\n", msg);
	fflush(stdout);

	if (opts->fifo_path) {
		if (progress >= 0) {
			fifo_send_line(opts->fifo_path, "PROGRESS:%d", progress);
		}
		fifo_send_line(opts->fifo_path, "TEXT:%s", msg);
	}
}

static void strip_eol(char* s) {
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
		s[--len] = '\0';
	}
}

static void copy_str_bounded(char* dst, size_t dst_size, const char* src) {
	if (!dst || dst_size == 0) {
		return;
	}
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t len = strlen(src);
	if (len >= dst_size) {
		len = dst_size - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static bool load_ra_settings(const char* userdata_dir, RASettings* out) {
	memset(out, 0, sizeof(*out));
	char path[CACHE_MAX_PATH];
	snprintf(path, sizeof(path), "%s/minuisettings.txt", userdata_dir);

	FILE* f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "Error: cannot open settings file: %s (%s)\n", path, strerror(errno));
		return false;
	}

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		strip_eol(line);
		if (strncmp(line, "raUsername=", 11) == 0) {
			copy_str_bounded(out->username, sizeof(out->username), line + 11);
		} else if (strncmp(line, "raServerUsername=", 17) == 0) {
			copy_str_bounded(out->server_username, sizeof(out->server_username), line + 17);
		} else if (strncmp(line, "raToken=", 8) == 0) {
			copy_str_bounded(out->token, sizeof(out->token), line + 8);
		} else if (strncmp(line, "raHardcoreMode=", 15) == 0) {
			out->hardcore = (int)strtol(line + 15, NULL, 10);
		}
	}
	fclose(f);
	return true;
}

static const char* effective_username(const RASettings* s) {
	if (s->server_username[0] != '\0') {
		return s->server_username;
	}
	return s->username;
}

static bool rom_list_push(RomList* list, const char* abs_path, const char* rel_path,
                          int console_id, time_t mtime, int64_t size) {
	if (list->count == list->capacity) {
		size_t new_cap = list->capacity ? list->capacity * 2 : INITIAL_LIST_CAP;
		RomEntry* grown = realloc(list->items, new_cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		list->items = grown;
		list->capacity = new_cap;
	}

	RomEntry* e = &list->items[list->count];
	memset(e, 0, sizeof(*e));
	e->abs_path = strdup(abs_path);
	e->rel_path = strdup(rel_path);
	if (!e->abs_path || !e->rel_path) {
		free(e->abs_path);
		free(e->rel_path);
		return false;
	}
	e->console_id = console_id;
	e->mtime = mtime;
	e->size = size;
	list->count++;
	return true;
}

static void rom_list_free(RomList* list) {
	for (size_t i = 0; i < list->count; i++) {
		free(list->items[i].abs_path);
		free(list->items[i].rel_path);
	}
	free(list->items);
	memset(list, 0, sizeof(*list));
}

static void walk_console_dir(const char* dir_path, const char* rel_prefix,
                             int console_id, int limit, RomList* list, size_t* count) {
	if (list && limit > 0 && list->count >= (size_t)limit) {
		return;
	}

	DIR* dir = opendir(dir_path);
	if (!dir) {
		return;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (list && limit > 0 && list->count >= (size_t)limit) {
			break;
		}
		if (ent->d_name[0] == '.') {
			continue;
		}

		char abs_path[CACHE_MAX_PATH];
		char rel_path[CACHE_MAX_PATH];
		if (snprintf(abs_path, sizeof(abs_path), "%s/%s", dir_path, ent->d_name) >= (int)sizeof(abs_path) ||
		    snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, ent->d_name) >= (int)sizeof(rel_path)) {
			continue;
		}

		struct stat st;
		if (stat(abs_path, &st) != 0) {
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			walk_console_dir(abs_path, rel_path, console_id, limit, list, count);
		} else if (S_ISREG(st.st_mode)) {
			if (!rom_should_skip(abs_path, dir_path, ent->d_name)) {
				if (list) {
					rom_list_push(list, abs_path, rel_path, console_id, st.st_mtime, (int64_t)st.st_size);
				}
				if (count) {
					(*count)++;
				}
			}
		}
	}

	closedir(dir);
}

static bool resolve_system_folder(const char* roms_dir, const char* folder_name,
                                  char* out_path, size_t out_size, int* out_console_id) {
	if (!folder_name || !*folder_name) {
		return false;
	}
	if (snprintf(out_path, out_size, "%s/%s", roms_dir, folder_name) >= (int)out_size) {
		return false;
	}

	struct stat st;
	if (stat(out_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Error: system folder not found: %s\n", out_path);
		return false;
	}

	char emu_tag[64];
	ra_extract_emu_tag(folder_name, emu_tag, sizeof(emu_tag));
	int console_id = ra_get_console_id(emu_tag);
	if (console_id == RC_CONSOLE_UNKNOWN) {
		fprintf(stderr, "Error: unsupported console tag '%s' for folder '%s'\n", emu_tag, folder_name);
		return false;
	}

	*out_console_id = console_id;
	return true;
}

static bool collect_roms(const char* roms_dir, const char* system_folder, int limit, RomList* list) {
	if (system_folder && *system_folder) {
		char folder_path[CACHE_MAX_PATH];
		int console_id = RC_CONSOLE_UNKNOWN;
		if (!resolve_system_folder(roms_dir, system_folder, folder_path, sizeof(folder_path), &console_id)) {
			return false;
		}
		walk_console_dir(folder_path, system_folder, console_id, limit, list, NULL);
		return true;
	}

	DIR* root = opendir(roms_dir);
	if (!root) {
		fprintf(stderr, "Error: cannot open ROMs directory: %s (%s)\n", roms_dir, strerror(errno));
		return false;
	}

	struct dirent* ent;
	while ((ent = readdir(root)) != NULL) {
		if (limit > 0 && list->count >= (size_t)limit) {
			break;
		}
		if (ent->d_name[0] == '.') {
			continue;
		}

		char folder_path[CACHE_MAX_PATH];
		if (snprintf(folder_path, sizeof(folder_path), "%s/%s", roms_dir, ent->d_name) >= (int)sizeof(folder_path)) {
			continue;
		}

		struct stat st;
		if (stat(folder_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}

		char emu_tag[64];
		ra_extract_emu_tag(ent->d_name, emu_tag, sizeof(emu_tag));
		int console_id = ra_get_console_id(emu_tag);
		if (console_id == RC_CONSOLE_UNKNOWN) {
			continue;
		}

		walk_console_dir(folder_path, ent->d_name, console_id, limit, list, NULL);
	}

	closedir(root);
	return true;
}

static const char* basename_ptr(const char* path) {
	const char* slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void hash_set_add(HashSet* set, const char* hash, const char* sample_name) {
	for (size_t i = 0; i < set->count; i++) {
		if (strcmp(set->hashes[i], hash) == 0) {
			return;
		}
	}

	if (set->count == set->capacity) {
		size_t new_cap = set->capacity ? set->capacity * 2 : INITIAL_LIST_CAP;
		char (*new_hashes)[ROM_HASH_LEN] = realloc(set->hashes, new_cap * sizeof(*new_hashes));
		char** new_names = realloc(set->sample_names, new_cap * sizeof(*new_names));
		if (!new_hashes || !new_names) {
			free(new_hashes ? new_hashes : set->hashes);
			free(new_names ? new_names : set->sample_names);
			set->hashes = NULL;
			set->sample_names = NULL;
			return;
		}
		set->hashes = new_hashes;
		set->sample_names = new_names;
		set->capacity = new_cap;
	}

	snprintf(set->hashes[set->count], ROM_HASH_LEN, "%s", hash);
	set->sample_names[set->count] = strdup(sample_name);
	set->count++;
}

static void hash_set_free(HashSet* set) {
	for (size_t i = 0; i < set->count; i++) {
		free(set->sample_names[i]);
	}
	free(set->hashes);
	free(set->sample_names);
	memset(set, 0, sizeof(*set));
}

static bool string_set_insert(StringSet* set, const char* value) {
	for (size_t i = 0; i < set->count; i++) {
		if (strcmp(set->names[i], value) == 0) {
			return false;
		}
	}
	if (set->count == set->capacity) {
		size_t new_cap = set->capacity ? set->capacity * 2 : INITIAL_LIST_CAP;
		char** grown = realloc(set->names, new_cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		set->names = grown;
		set->capacity = new_cap;
	}
	char* copy = strdup(value);
	if (!copy) {
		return false;
	}
	set->names[set->count++] = copy;
	return true;
}

static void string_set_free(StringSet* set) {
	for (size_t i = 0; i < set->count; i++) {
		free(set->names[i]);
	}
	free(set->names);
	memset(set, 0, sizeof(*set));
}

static bool ensure_cache_dirs(const char* userdata_dir) {
	char cache_dir[CACHE_MAX_PATH];
	char badge_dir[CACHE_MAX_PATH];
	snprintf(cache_dir, sizeof(cache_dir), "%s/.ra/offline/cache", userdata_dir);
	snprintf(badge_dir, sizeof(badge_dir), "%s/.ra/badges", userdata_dir);
	return cache_mkdirs(cache_dir) && cache_mkdirs(badge_dir);
}

static bool perform_login(const SyncOptions* opts, HttpClient* http,
                          const char* username, const char* token) {
	char login_path[CACHE_MAX_PATH];
	cache_login_path(opts->userdata_dir, login_path, sizeof(login_path));

	if (!opts->force && cache_verify_file(login_path)) {
		printf("Login cache valid: %s\n", login_path);
		return true;
	}

	if (opts->dry_run) {
		printf("[dry-run] Would authenticate user '%s' and write %s\n", username, login_path);
		return true;
	}

	rc_api_login_request_t params = {
		.username = username,
		.api_token = token,
		.password = NULL
	};
	rc_api_request_t req;
	int rc = rc_api_init_login_request(&req, &params);
	if (rc != RC_OK) {
		fprintf(stderr, "Error: rc_api_init_login_request failed: %s\n", rc_error_str(rc));
		return false;
	}

	HttpResponse http_resp;
	bool posted = http_post_form(http, req.url, req.post_data, &http_resp);
	rc_api_destroy_request(&req);
	if (!posted) {
		fprintf(stderr, "Error: HTTP request failed during login\n");
		return false;
	}

	rc_api_server_response_t srv = {
		.body = http_resp.body,
		.body_length = http_resp.body_len,
		.http_status_code = http_resp.status_code
	};
	rc_api_login_response_t login_resp;
	rc = rc_api_process_login_server_response(&login_resp, &srv);
	bool ok = (rc == RC_OK && login_resp.response.succeeded);
	if (ok) {
		if (!cache_write_file(login_path, http_resp.body, http_resp.body_len)) {
			fprintf(stderr, "Error: failed to write %s\n", login_path);
			ok = false;
		} else {
			printf("Cached login response for %s\n", login_resp.username ? login_resp.username : username);
		}
	} else {
		fprintf(stderr, "Error: RetroAchievements login failed: %s\n",
		        login_resp.response.error_message ? login_resp.response.error_message : rc_error_str(rc));
	}

	rc_api_destroy_login_response(&login_resp);
	http_response_free(&http_resp);
	return ok;
}

static bool fetch_or_load_game_sets(const SyncOptions* opts, HttpClient* http,
                                    const char* username, const char* token,
                                    const char* hash, rc_api_fetch_game_sets_response_t* out_sets,
                                    RunStats* stats) {
	char ach_path[CACHE_MAX_PATH];
	cache_achievementsets_path(opts->userdata_dir, hash, ach_path, sizeof(ach_path));

	if (!opts->force) {
		char* cached_body = NULL;
		size_t cached_len = 0;
		if (cache_read_file(ach_path, &cached_body, &cached_len)) {
			rc_api_server_response_t srv = {
				.body = cached_body,
				.body_length = cached_len,
				.http_status_code = HTTP_STATUS_OK
			};
			int rc = rc_api_process_fetch_game_sets_server_response(out_sets, &srv);
			free(cached_body);
			if (rc == RC_OK) {
				stats->sets_cached++;
				return true;
			}
			rc_api_destroy_fetch_game_sets_response(out_sets);
		}
	}

	if (opts->dry_run) {
		printf("[dry-run] Would fetch achievementsets for hash %s\n", hash);
		memset(out_sets, 0, sizeof(*out_sets));
		return false;
	}

	rc_api_fetch_game_sets_request_t params = {
		.username = username,
		.api_token = token,
		.game_id = 0,
		.game_hash = hash
	};
	rc_api_request_t req;
	int rc = rc_api_init_fetch_game_sets_request(&req, &params);
	if (rc != RC_OK) {
		fprintf(stderr, "Error: init_fetch_game_sets failed for %s: %s\n", hash, rc_error_str(rc));
		stats->errors++;
		return false;
	}

	HttpResponse http_resp;
	bool posted = http_post_form(http, req.url, req.post_data, &http_resp);
	rc_api_destroy_request(&req);
	if (!posted) {
		fprintf(stderr, "Error: HTTP request failed for achievementsets %s\n", hash);
		stats->errors++;
		return false;
	}

	rc_api_server_response_t srv = {
		.body = http_resp.body,
		.body_length = http_resp.body_len,
		.http_status_code = http_resp.status_code
	};
	rc = rc_api_process_fetch_game_sets_server_response(out_sets, &srv);
	if (rc == RC_OK && out_sets->response.succeeded) {
		if (!cache_write_file(ach_path, http_resp.body, http_resp.body_len)) {
			fprintf(stderr, "Warning: failed to write %s\n", ach_path);
			stats->errors++;
		} else {
			stats->sets_fetched++;
		}
		http_response_free(&http_resp);
		return true;
	}

	fprintf(stderr, "Warning: achievementsets failed for %s: %s\n",
	        hash, out_sets->response.error_message ? out_sets->response.error_message : rc_error_str(rc));
	rc_api_destroy_fetch_game_sets_response(out_sets);
	http_response_free(&http_resp);
	stats->errors++;
	return false;
}

static void cache_startsession(const SyncOptions* opts, HttpClient* http,
                               const char* username, const char* token, int hardcore,
                               uint32_t game_id, const char* hash, RunStats* stats) {
	char ss_path[CACHE_MAX_PATH];
	cache_startsession_path(opts->userdata_dir, hash, ss_path, sizeof(ss_path));

	if (!opts->force && !opts->refresh_sessions && cache_verify_file(ss_path)) {
		return;
	}

	if (opts->dry_run) {
		printf("[dry-run] Would fetch startsession for game %u (%s)\n", game_id, hash);
		return;
	}

	rc_api_start_session_request_t params = {
		.username = username,
		.api_token = token,
		.game_id = game_id,
		.game_hash = hash,
		.hardcore = (uint32_t)(hardcore ? 1 : 0)
	};
	rc_api_request_t req;
	int rc = rc_api_init_start_session_request(&req, &params);
	if (rc != RC_OK) {
		stats->errors++;
		return;
	}

	HttpResponse http_resp;
	bool posted = http_post_form(http, req.url, req.post_data, &http_resp);
	rc_api_destroy_request(&req);
	if (!posted) {
		stats->errors++;
		return;
	}

	rc_api_server_response_t srv = {
		.body = http_resp.body,
		.body_length = http_resp.body_len,
		.http_status_code = http_resp.status_code
	};
	rc_api_start_session_response_t ss_resp;
	rc = rc_api_process_start_session_server_response(&ss_resp, &srv);
	if (rc == RC_OK && ss_resp.response.succeeded) {
		if (cache_write_file(ss_path, http_resp.body, http_resp.body_len)) {
			stats->sessions_fetched++;
		} else {
			stats->errors++;
		}
	} else {
		stats->errors++;
	}

	rc_api_destroy_start_session_response(&ss_resp);
	http_response_free(&http_resp);
}

static void download_badge_variant(const SyncOptions* opts, HttpClient* http,
                                   const char* badge_name, bool locked, RunStats* stats) {
	char dest[CACHE_MAX_PATH];
	cache_badge_path(opts->userdata_dir, badge_name, locked, dest, sizeof(dest));

	if (!opts->force && cache_file_exists_nonempty(dest)) {
		return;
	}
	if (opts->dry_run) {
		printf("[dry-run] Would download badge %s%s.png\n", badge_name, locked ? "_lock" : "");
		return;
	}

	char url[512];
	snprintf(url, sizeof(url), "%s%s%s.png",
	         RA_BADGE_BASE_URL, badge_name, locked ? "_lock" : "");
	int code = http_download_file(http, url, dest);
	if (code == HTTP_STATUS_OK) {
		stats->badges_downloaded++;
	} else {
		fprintf(stderr, "Warning: failed to download badge %s (HTTP %d)\n", url, code);
		stats->errors++;
	}
}

static bool has_suffix(const char* name, const char* suffix) {
	size_t nlen = strlen(name);
	size_t slen = strlen(suffix);
	return nlen > slen && strcmp(name + nlen - slen, suffix) == 0;
}

static void count_ra_dir(const char* dir_path, size_t* out_games, size_t* out_badges) {
	DIR* dir = opendir(dir_path);
	if (!dir) {
		return;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') {
			continue;
		}

		char full_path[CACHE_MAX_PATH];
		if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name) >= (int)sizeof(full_path)) {
			continue;
		}

		struct stat st;
		if (stat(full_path, &st) != 0) {
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			count_ra_dir(full_path, out_games, out_badges);
		} else if (S_ISREG(st.st_mode)) {
			if (strncmp(ent->d_name, "achievementsets_", 16) == 0 &&
			    has_suffix(ent->d_name, ".bin")) {
				(*out_games)++;
			} else if (has_suffix(ent->d_name, ".png")) {
				(*out_badges)++;
			}
		}
	}

	closedir(dir);
}

static int print_status(const SyncOptions* opts) {
	RASettings settings;
	bool loaded = load_ra_settings(opts->userdata_dir, &settings);
	const char* username = effective_username(&settings);
	if (!loaded || !*username || !*settings.token) {
		printf("Not logged in (configure RA in NextUI Settings)\n");
		return 2;
	}

	char ra_dir[CACHE_MAX_PATH];
	size_t games = 0;
	size_t badges = 0;
	if (snprintf(ra_dir, sizeof(ra_dir), "%s/.ra", opts->userdata_dir) < (int)sizeof(ra_dir)) {
		count_ra_dir(ra_dir, &games, &badges);
	}

	printf("User: %s | Cached: %zu games, %zu badges\n", username, games, badges);
	return 0;
}

static void cache_game_badges(const SyncOptions* opts, HttpClient* http,
                              const rc_api_fetch_game_sets_response_t* sets,
                              StringSet* seen_badges, RunStats* stats) {
	for (uint32_t s = 0; s < sets->num_sets; s++) {
		const rc_api_achievement_set_definition_t* set = &sets->sets[s];
		for (uint32_t a = 0; a < set->num_achievements; a++) {
			const char* badge = set->achievements[a].badge_name;
			if (!badge || !*badge) {
				continue;
			}
			if (!string_set_insert(seen_badges, badge)) {
				continue;
			}
			download_badge_variant(opts, http, badge, false, stats);
			download_badge_variant(opts, http, badge, true, stats);
		}
	}
}

typedef struct {
	char* folder_name;
	char tag[64];
	size_t count;
} SystemEntry;

static int system_entry_cmp(const void* a, const void* b) {
	const SystemEntry* sa = (const SystemEntry*)a;
	const SystemEntry* sb = (const SystemEntry*)b;
	return strcmp(sa->folder_name, sb->folder_name);
}

static int rom_entry_cmp(const void* a, const void* b) {
	const RomEntry* ra = (const RomEntry*)a;
	const RomEntry* rb = (const RomEntry*)b;
	return strcmp(ra->rel_path, rb->rel_path);
}

static void print_json_escaped(FILE* out, const char* s) {
	for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
		unsigned char c = *p;
		switch (c) {
			case '\\': fputs("\\\\", out); break;
			case '"':  fputs("\\\"", out); break;
			case '\b': fputs("\\b", out); break;
			case '\f': fputs("\\f", out); break;
			case '\n': fputs("\\n", out); break;
			case '\r': fputs("\\r", out); break;
			case '\t': fputs("\\t", out); break;
			default:
				if (c < 0x20) {
					fprintf(out, "\\u%04x", (unsigned int)c);
				} else {
					fputc((int)c, out);
				}
				break;
		}
	}
}

static int list_systems(const SyncOptions* opts) {
	DIR* root = opendir(opts->roms_dir);
	if (!root) {
		fprintf(stderr, "Error: cannot open ROMs directory: %s (%s)\n", opts->roms_dir, strerror(errno));
		return 1;
	}

	SystemEntry* entries = NULL;
	size_t count = 0;
	size_t cap = 0;

	struct dirent* ent;
	while ((ent = readdir(root)) != NULL) {
		if (ent->d_name[0] == '.') {
			continue;
		}

		char folder_path[CACHE_MAX_PATH];
		if (snprintf(folder_path, sizeof(folder_path), "%s/%s", opts->roms_dir, ent->d_name) >= (int)sizeof(folder_path)) {
			continue;
		}

		struct stat st;
		if (stat(folder_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			continue;
		}

		char emu_tag[64];
		ra_extract_emu_tag(ent->d_name, emu_tag, sizeof(emu_tag));
		int console_id = ra_get_console_id(emu_tag);
		if (console_id == RC_CONSOLE_UNKNOWN) {
			continue;
		}

		size_t rom_count = 0;
		walk_console_dir(folder_path, ent->d_name, console_id, 0, NULL, &rom_count);
		if (rom_count == 0) {
			continue;
		}

		if (count == cap) {
			size_t new_cap = cap ? cap * 2 : 32;
			SystemEntry* grown = realloc(entries, new_cap * sizeof(*grown));
			if (!grown) {
				for (size_t i = 0; i < count; i++) {
					free(entries[i].folder_name);
				}
				free(entries);
				closedir(root);
				return 1;
			}
			entries = grown;
			cap = new_cap;
		}

		entries[count].folder_name = strdup(ent->d_name);
		if (!entries[count].folder_name) {
			continue;
		}
		copy_str_bounded(entries[count].tag, sizeof(entries[count].tag), emu_tag);
		entries[count].count = rom_count;
		count++;
	}

	closedir(root);

	if (count > 1) {
		qsort(entries, count, sizeof(*entries), system_entry_cmp);
	}

	for (size_t i = 0; i < count; i++) {
		printf("%s\t%s\t%zu\n", entries[i].folder_name, entries[i].tag, entries[i].count);
		free(entries[i].folder_name);
	}
	free(entries);
	return 0;
}

static int list_games_json(const SyncOptions* opts, const char* folder_name) {
	char folder_path[CACHE_MAX_PATH];
	int console_id = RC_CONSOLE_UNKNOWN;
	if (!resolve_system_folder(opts->roms_dir, folder_name, folder_path, sizeof(folder_path), &console_id)) {
		return 1;
	}

	RomList roms = {0};
	walk_console_dir(folder_path, folder_name, console_id, 0, &roms, NULL);
	if (roms.count > 1) {
		qsort(roms.items, roms.count, sizeof(*roms.items), rom_entry_cmp);
	}

	ScraperIndex index;
	scraper_index_init(&index);
	char index_path[CACHE_MAX_PATH];
	cache_index_path(opts->userdata_dir, index_path, sizeof(index_path));
	scraper_index_load(&index, index_path);

	size_t prefix_len = strlen(folder_name) + 1;
	fputs("{\"games\":[", stdout);
	for (size_t i = 0; i < roms.count; i++) {
		const RomEntry* rom = &roms.items[i];
		const char* sub_rel = (strlen(rom->rel_path) > prefix_len)
			? (rom->rel_path + prefix_len)
			: rom->rel_path;

		const char* hash = scraper_index_lookup(&index, rom->rel_path, rom->console_id, rom->mtime, rom->size);
		bool cached = hash && cache_has_valid_game(opts->userdata_dir, hash);

		if (i > 0) {
			fputc(',', stdout);
		}
		fputs("{\"name\":\"", stdout);
		print_json_escaped(stdout, sub_rel);
		if (cached) {
			fputs("\",\"options\":[\"Skip (Cached)\",\"Sync\"],\"selected\":0}", stdout);
		} else {
			fputs("\",\"options\":[\"Skip\",\"Sync\"],\"selected\":0}", stdout);
		}
	}
	fputs("]}\n", stdout);
	fflush(stdout);

	scraper_index_free(&index);
	rom_list_free(&roms);
	return 0;
}

static void json_skip_ws(const char** p) {
	while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') {
		(*p)++;
	}
}

static bool json_parse_hex4(const char* p, uint32_t* out) {
	uint32_t val = 0;
	for (int i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)p[i];
		if (!isxdigit(c)) {
			return false;
		}
		val <<= 4;
		if (c >= '0' && c <= '9') {
			val |= (uint32_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			val |= (uint32_t)(c - 'a' + 10);
		} else {
			val |= (uint32_t)(c - 'A' + 10);
		}
	}
	*out = val;
	return true;
}

static bool json_append_utf8(char** buf, size_t* len, size_t* cap, uint32_t cp) {
	char tmp[4];
	size_t n = 0;
	if (cp <= 0x7F) {
		tmp[0] = (char)cp;
		n = 1;
	} else if (cp <= 0x7FF) {
		tmp[0] = (char)(0xC0 | (cp >> 6));
		tmp[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp <= 0xFFFF) {
		tmp[0] = (char)(0xE0 | (cp >> 12));
		tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else if (cp <= 0x10FFFF) {
		tmp[0] = (char)(0xF0 | (cp >> 18));
		tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	} else {
		return false;
	}

	if (*len + n + 1 > *cap) {
		size_t new_cap = (*cap == 0) ? 256 : (*cap * 2);
		while (new_cap < *len + n + 1) {
			new_cap *= 2;
		}
		char* grown = realloc(*buf, new_cap);
		if (!grown) {
			return false;
		}
		*buf = grown;
		*cap = new_cap;
	}

	memcpy(*buf + *len, tmp, n);
	*len += n;
	(*buf)[*len] = '\0';
	return true;
}

static bool json_parse_string(const char** p, char** out) {
	if (**p != '"') {
		return false;
	}
	(*p)++;

	char* buf = NULL;
	size_t len = 0;
	size_t cap = 0;
	if (!json_append_utf8(&buf, &len, &cap, 0)) {
		return false;
	}
	len = 0;
	buf[0] = '\0';

	while (**p != '\0' && **p != '"') {
		unsigned char c = (unsigned char)**p;
		(*p)++;
		if (c == '\\') {
			char esc = **p;
			if (!esc) {
				free(buf);
				return false;
			}
			(*p)++;
			uint32_t cp = 0;
			switch (esc) {
				case '"':  cp = '"'; break;
				case '\\': cp = '\\'; break;
				case '/':  cp = '/'; break;
				case 'b':  cp = '\b'; break;
				case 'f':  cp = '\f'; break;
				case 'n':  cp = '\n'; break;
				case 'r':  cp = '\r'; break;
				case 't':  cp = '\t'; break;
				case 'u': {
					if (!json_parse_hex4(*p, &cp)) {
						free(buf);
						return false;
					}
					*p += 4;
					if (cp >= 0xD800 && cp <= 0xDBFF && (*p)[0] == '\\' && (*p)[1] == 'u') {
						uint32_t low = 0;
						if (json_parse_hex4(*p + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
							cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
							*p += 6;
						}
					}
					break;
				}
				default:
					free(buf);
					return false;
			}
			if (!json_append_utf8(&buf, &len, &cap, cp)) {
				free(buf);
				return false;
			}
		} else {
			if (!json_append_utf8(&buf, &len, &cap, c)) {
				free(buf);
				return false;
			}
		}
	}

	if (**p != '"') {
		free(buf);
		return false;
	}
	(*p)++;
	*out = buf;
	return true;
}

static bool json_skip_value(const char** p);

static bool json_skip_string(const char** p) {
	if (**p != '"') {
		return false;
	}
	(*p)++;
	while (**p != '\0' && **p != '"') {
		if (**p == '\\') {
			(*p)++;
			if (**p == '\0') {
				return false;
			}
		}
		(*p)++;
	}
	if (**p != '"') {
		return false;
	}
	(*p)++;
	return true;
}

static bool json_skip_value(const char** p) {
	json_skip_ws(p);
	char c = **p;
	if (c == '"') {
		return json_skip_string(p);
	}
	if (c == '{') {
		(*p)++;
		json_skip_ws(p);
		if (**p == '}') {
			(*p)++;
			return true;
		}
		while (**p != '\0') {
			json_skip_ws(p);
			if (!json_skip_string(p)) {
				return false;
			}
			json_skip_ws(p);
			if (**p != ':') {
				return false;
			}
			(*p)++;
			if (!json_skip_value(p)) {
				return false;
			}
			json_skip_ws(p);
			if (**p == ',') {
				(*p)++;
			} else if (**p == '}') {
				(*p)++;
				return true;
			} else {
				return false;
			}
		}
		return false;
	}
	if (c == '[') {
		(*p)++;
		json_skip_ws(p);
		if (**p == ']') {
			(*p)++;
			return true;
		}
		while (**p != '\0') {
			if (!json_skip_value(p)) {
				return false;
			}
			json_skip_ws(p);
			if (**p == ',') {
				(*p)++;
			} else if (**p == ']') {
				(*p)++;
				return true;
			} else {
				return false;
			}
		}
		return false;
	}
	if (c == '\0') {
		return false;
	}
	while (**p != '\0' && **p != ',' && **p != ']' && **p != '}' &&
	       **p != ' ' && **p != '\t' && **p != '\n' && **p != '\r') {
		(*p)++;
	}
	return true;
}

static bool json_parse_game_object(const char** p, StringSet* out_names) {
	json_skip_ws(p);
	if (**p != '{') {
		return false;
	}
	(*p)++;
	json_skip_ws(p);
	if (**p == '}') {
		(*p)++;
		return true;
	}

	char* name = NULL;
	bool selected = false;

	while (**p != '\0') {
		json_skip_ws(p);
		char* key = NULL;
		if (!json_parse_string(p, &key)) {
			free(name);
			return false;
		}
		json_skip_ws(p);
		if (**p != ':') {
			free(key);
			free(name);
			return false;
		}
		(*p)++;
		json_skip_ws(p);

		if (strcmp(key, "name") == 0 && **p == '"') {
			free(name);
			name = NULL;
			if (!json_parse_string(p, &name)) {
				free(key);
				return false;
			}
		} else if (strcmp(key, "selected") == 0) {
			if (**p == '-' || isdigit((unsigned char)**p)) {
				char* end = NULL;
				long val = strtol(*p, &end, 10);
				*p = end;
				selected = (val == 1);
			} else if (strncmp(*p, "true", 4) == 0) {
				*p += 4;
				selected = true;
			} else if (!json_skip_value(p)) {
				free(key);
				free(name);
				return false;
			}
		} else if (!json_skip_value(p)) {
			free(key);
			free(name);
			return false;
		}
		free(key);

		json_skip_ws(p);
		if (**p == ',') {
			(*p)++;
		} else if (**p == '}') {
			(*p)++;
			if (selected && name && *name) {
				string_set_insert(out_names, name);
			}
			free(name);
			return true;
		} else {
			free(name);
			return false;
		}
	}

	free(name);
	return false;
}

static bool parse_selected_games_json(const char* json_path, StringSet* out_names) {
	FILE* f = fopen(json_path, "rb");
	if (!f) {
		fprintf(stderr, "Error: cannot open JSON file %s: %s\n", json_path, strerror(errno));
		return false;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	long fsize = ftell(f);
	if (fsize < 0 || (unsigned long)fsize > JSON_FILE_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}

	char* buf = malloc((size_t)fsize + 1);
	if (!buf) {
		fclose(f);
		return false;
	}
	size_t nread = fread(buf, 1, (size_t)fsize, f);
	fclose(f);
	buf[nread] = '\0';

	const char* p = buf;
	json_skip_ws(&p);
	if (*p != '{') {
		free(buf);
		return false;
	}
	p++;
	json_skip_ws(&p);
	if (*p == '}') {
		free(buf);
		return true;
	}

	bool ok = true;
	while (*p != '\0') {
		json_skip_ws(&p);
		char* key = NULL;
		if (!json_parse_string(&p, &key)) {
			ok = false;
			break;
		}
		json_skip_ws(&p);
		if (*p != ':') {
			free(key);
			ok = false;
			break;
		}
		p++;
		json_skip_ws(&p);

		if (strcmp(key, "games") == 0 && *p == '[') {
			p++;
			json_skip_ws(&p);
			if (*p != ']') {
				while (*p != '\0') {
					if (!json_parse_game_object(&p, out_names)) {
						ok = false;
						break;
					}
					json_skip_ws(&p);
					if (*p == ',') {
						p++;
					} else if (*p == ']') {
						p++;
						break;
					} else {
						ok = false;
						break;
					}
				}
			} else {
				p++;
			}
		} else if (!json_skip_value(&p)) {
			ok = false;
		}
		free(key);

		if (!ok) {
			break;
		}
		json_skip_ws(&p);
		if (*p == ',') {
			p++;
		} else if (*p == '}') {
			p++;
			break;
		} else {
			ok = false;
			break;
		}
	}

	free(buf);
	return ok;
}

static bool collect_roms_from_json(const SyncOptions* opts, RomList* list, bool* out_empty) {
	*out_empty = false;

	char folder_path[CACHE_MAX_PATH];
	int console_id = RC_CONSOLE_UNKNOWN;
	if (!resolve_system_folder(opts->roms_dir, opts->system_folder, folder_path, sizeof(folder_path), &console_id)) {
		return false;
	}

	StringSet selected = {0};
	if (!parse_selected_games_json(opts->sync_json_path, &selected)) {
		fprintf(stderr, "Error: failed to parse selected games from %s\n", opts->sync_json_path);
		string_set_free(&selected);
		return false;
	}

	if (selected.count == 0) {
		*out_empty = true;
		string_set_free(&selected);
		return true;
	}

	for (size_t i = 0; i < selected.count; i++) {
		if (opts->limit > 0 && list->count >= (size_t)opts->limit) {
			break;
		}

		const char* sub_rel = selected.names[i];
		char abs_path[CACHE_MAX_PATH];
		char rel_path[CACHE_MAX_PATH];
		if (snprintf(abs_path, sizeof(abs_path), "%s/%s", folder_path, sub_rel) >= (int)sizeof(abs_path) ||
		    snprintf(rel_path, sizeof(rel_path), "%s/%s", opts->system_folder, sub_rel) >= (int)sizeof(rel_path)) {
			continue;
		}

		struct stat st;
		if (stat(abs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
			fprintf(stderr, "Warning: selected ROM not found: %s\n", abs_path);
			continue;
		}

		rom_list_push(list, abs_path, rel_path, console_id, st.st_mtime, (int64_t)st.st_size);
	}

	string_set_free(&selected);
	return true;
}

static void normalize_folder_arg(const char* src, char* dst, size_t dst_sz) {
	copy_str_bounded(dst, dst_sz, src);
	size_t len = strlen(dst);
	while (len > 1 && dst[len - 1] == '/') {
		dst[--len] = '\0';
	}
}

static void print_usage(const char* prog) {
	printf("Usage: %s [options]\n"
	       "  --status                   Print RA login and cache status (exit 2 if not logged in)\n"
	       "  --list-systems             List supported ROM folders (<folder>\\t<tag>\\t<count>)\n"
	       "  --list-games-json <dir>    Print minui-list JSON for ROMs in <dir>\n"
	       "  --system <dir>             Limit sync to a single ROM folder under --roms\n"
	       "  --sync-json <path>         Sync only games with selected=1 in <path> (requires --system)\n"
	       "  --userdata <dir>           Shared userdata dir (default: $SHARED_USERDATA_PATH or %s)\n"
	       "  --roms <dir>               ROMs root dir (default: $SDCARD_PATH/Roms or %s)\n"
	       "  --fifo <path>              FIFO path for show2.elf TEXT/PROGRESS updates\n"
	       "  --delay-ms <ms>            Delay between HTTP requests (default: %d)\n"
	       "  --limit <n>                Maximum ROMs to scan\n"
	       "  --dry-run                  Scan and hash only; do not write cache or hit network\n"
	       "  --force                    Re-hash ROMs and re-fetch existing cache files\n"
	       "  --refresh-sessions         Re-fetch startsession cache files for all matched games\n"
	       "  --help                     Show this help message\n",
	       prog, DEFAULT_USERDATA_PATH, DEFAULT_ROMS_PATH, HTTP_DEFAULT_DELAY_MS);
}

static bool parse_args(int argc, char** argv, SyncOptions* opts, bool* status_mode,
                       bool* list_systems_mode, char* list_games_buf, size_t list_games_sz,
                       char* system_buf, size_t system_sz,
                       char* default_roms_buf, size_t roms_buf_sz) {
	const char* env_userdata = getenv("SHARED_USERDATA_PATH");
	const char* env_sdcard = getenv("SDCARD_PATH");

	opts->userdata_dir = (env_userdata && *env_userdata) ? env_userdata : DEFAULT_USERDATA_PATH;
	if (env_sdcard && *env_sdcard) {
		snprintf(default_roms_buf, roms_buf_sz, "%s/Roms", env_sdcard);
		opts->roms_dir = default_roms_buf;
	} else {
		opts->roms_dir = DEFAULT_ROMS_PATH;
	}
	opts->fifo_path = NULL;
	opts->system_folder = NULL;
	opts->sync_json_path = NULL;
	opts->delay_ms = HTTP_DEFAULT_DELAY_MS;
	opts->limit = 0;
	opts->dry_run = false;
	opts->force = false;
	opts->refresh_sessions = false;
	*status_mode = false;
	*list_systems_mode = false;
	list_games_buf[0] = '\0';
	system_buf[0] = '\0';

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--status") == 0) {
			*status_mode = true;
		} else if (strcmp(argv[i], "--list-systems") == 0) {
			*list_systems_mode = true;
		} else if (strcmp(argv[i], "--list-games-json") == 0 && i + 1 < argc) {
			normalize_folder_arg(argv[++i], list_games_buf, list_games_sz);
		} else if (strcmp(argv[i], "--system") == 0 && i + 1 < argc) {
			normalize_folder_arg(argv[++i], system_buf, system_sz);
			opts->system_folder = system_buf;
		} else if (strcmp(argv[i], "--sync-json") == 0 && i + 1 < argc) {
			opts->sync_json_path = argv[++i];
		} else if (strcmp(argv[i], "--userdata") == 0 && i + 1 < argc) {
			opts->userdata_dir = argv[++i];
		} else if (strcmp(argv[i], "--roms") == 0 && i + 1 < argc) {
			opts->roms_dir = argv[++i];
		} else if (strcmp(argv[i], "--fifo") == 0 && i + 1 < argc) {
			opts->fifo_path = argv[++i];
		} else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
			opts->delay_ms = (int)strtol(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
			opts->limit = (int)strtol(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--dry-run") == 0) {
			opts->dry_run = true;
		} else if (strcmp(argv[i], "--force") == 0) {
			opts->force = true;
		} else if (strcmp(argv[i], "--refresh-sessions") == 0) {
			opts->refresh_sessions = true;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			exit(0);
		} else {
			fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
			print_usage(argv[0]);
			return false;
		}
	}

	if (opts->sync_json_path && (!opts->system_folder || !*opts->system_folder)) {
		fprintf(stderr, "Error: --sync-json requires --system <folder_name>\n");
		return false;
	}
	return true;
}

static bool run_sync(const SyncOptions* opts, RunStats* out_stats) {
	memset(out_stats, 0, sizeof(*out_stats));

	RomList roms = {0};
	if (opts->sync_json_path) {
		bool json_empty = false;
		if (!collect_roms_from_json(opts, &roms, &json_empty)) {
			ui_status(opts, -1, "Failed to read selected games");
			rom_list_free(&roms);
			return false;
		}
		if (json_empty) {
			ui_status(opts, PROGRESS_COMPLETE, "No games selected");
			printf("SUMMARY: Scanned 0 | Matched 0 | Cached 0 | Badges 0 | Errors 0\n");
			fflush(stdout);
			rom_list_free(&roms);
			return true;
		}
	}

	RASettings settings;
	if (!load_ra_settings(opts->userdata_dir, &settings)) {
		ui_status(opts, -1, "Missing NextUI settings file");
		rom_list_free(&roms);
		return false;
	}

	const char* username = effective_username(&settings);
	if (!*username || !*settings.token) {
		ui_status(opts, -1, "No RetroAchievements login in NextUI settings");
		rom_list_free(&roms);
		return false;
	}

	if (!opts->dry_run && !ensure_cache_dirs(opts->userdata_dir)) {
		ui_status(opts, -1, "Failed to create .ra cache directories");
		rom_list_free(&roms);
		return false;
	}

	HttpClient http;
	http_client_init(&http, opts->delay_ms);

	ui_status(opts, 0, "Verifying RetroAchievements login...");
	if (!perform_login(opts, &http, username, settings.token)) {
		ui_status(opts, -1, "RetroAchievements login failed");
		rom_list_free(&roms);
		return false;
	}

	if (!opts->sync_json_path) {
		ui_status(opts, 5, "Scanning ROM folders...");
		if (!collect_roms(opts->roms_dir, opts->system_folder, opts->limit, &roms)) {
			ui_status(opts, -1, "Failed to scan ROM folders");
			rom_list_free(&roms);
			return false;
		}
	}

	ScraperIndex index;
	scraper_index_init(&index);
	char index_path[CACHE_MAX_PATH];
	cache_index_path(opts->userdata_dir, index_path, sizeof(index_path));
	if (!opts->force || opts->system_folder || opts->sync_json_path) {
		scraper_index_load(&index, index_path);
	}

	out_stats->roms_scanned = roms.count;

	HashSet unique_hashes = {0};
	for (size_t i = 0; i < roms.count; i++) {
		RomEntry* rom = &roms.items[i];
		const char* rom_name = basename_ptr(rom->rel_path);
		int pct = roms.count > 0
			? 5 + (int)(((i + 1) * (PROGRESS_SCAN_MAX - 5)) / roms.count)
			: PROGRESS_SCAN_MAX;

		const char* cached_hash = opts->force ? NULL
			: scraper_index_lookup(&index, rom->rel_path, rom->console_id, rom->mtime, rom->size);

		if (cached_hash) {
			snprintf(rom->hash, sizeof(rom->hash), "%s", cached_hash);
			out_stats->index_hits++;
		} else {
			ui_status(opts, pct, "Hashing (%zu/%zu): %s", i + 1, roms.count, rom_name);
			if (!rom_hash_file(rom->abs_path, rom->console_id, rom->hash)) {
				out_stats->hash_failures++;
				continue;
			}
			out_stats->roms_hashed++;
			scraper_index_upsert(&index, rom->rel_path, rom->console_id, rom->mtime, rom->size, rom->hash);
		}

		hash_set_add(&unique_hashes, rom->hash, rom_name);
		out_stats->unique_games = unique_hashes.count;
	}

	if (!opts->dry_run && !scraper_index_save(&index, index_path)) {
		fprintf(stderr, "Warning: failed to save scraper index %s\n", index_path);
	}
	scraper_index_free(&index);
	rom_list_free(&roms);

	out_stats->unique_games = unique_hashes.count;
	StringSet seen_badges = {0};

	for (size_t i = 0; i < unique_hashes.count; i++) {
		const char* hash = unique_hashes.hashes[i];
		const char* name = unique_hashes.sample_names[i];
		int pct = PROGRESS_SCAN_MAX +
			(int)(((i + 1) * (PROGRESS_COMPLETE - PROGRESS_SCAN_MAX)) / unique_hashes.count);

		ui_status(opts, pct, "Caching (%zu/%zu): %s", i + 1, unique_hashes.count, name);

		rc_api_fetch_game_sets_response_t sets;
		if (!fetch_or_load_game_sets(opts, &http, username, settings.token, hash, &sets, out_stats)) {
			continue;
		}

		if (sets.id != 0) {
			uint32_t session_gid = sets.session_game_id ? sets.session_game_id : sets.id;
			cache_startsession(opts, &http, username, settings.token,
			                   settings.hardcore, session_gid, hash, out_stats);
			cache_game_badges(opts, &http, &sets, &seen_badges, out_stats);
		}

		rc_api_destroy_fetch_game_sets_response(&sets);
	}

	string_set_free(&seen_badges);
	hash_set_free(&unique_hashes);

	ui_status(opts, PROGRESS_COMPLETE,
	          "Done! %zu games (%zu fetched, %zu cached), %zu badges, %zu errors",
	          out_stats->unique_games, out_stats->sets_fetched, out_stats->sets_cached,
	          out_stats->badges_downloaded, out_stats->errors);

	printf("Summary: scanned=%zu index_hits=%zu hashed=%zu hash_fail=%zu unique=%zu "
	       "sets_fetched=%zu sets_cached=%zu sessions=%zu badges=%zu errors=%zu\n",
	       out_stats->roms_scanned, out_stats->index_hits, out_stats->roms_hashed, out_stats->hash_failures,
	       out_stats->unique_games, out_stats->sets_fetched, out_stats->sets_cached,
	       out_stats->sessions_fetched, out_stats->badges_downloaded, out_stats->errors);
	printf("SUMMARY: Scanned %zu | Matched %zu | Cached %zu | Badges %zu | Errors %zu\n",
	       out_stats->roms_scanned, out_stats->unique_games,
	       out_stats->sets_fetched + out_stats->sets_cached,
	       out_stats->badges_downloaded, out_stats->errors);
	fflush(stdout);

	return true;
}

int main(int argc, char** argv) {
	char default_roms_buf[CACHE_MAX_PATH];
	char list_games_buf[CACHE_MAX_PATH];
	char system_buf[CACHE_MAX_PATH];
	SyncOptions opts;
	bool status_mode = false;
	bool list_systems_mode = false;
	if (!parse_args(argc, argv, &opts, &status_mode, &list_systems_mode,
	                list_games_buf, sizeof(list_games_buf),
	                system_buf, sizeof(system_buf),
	                default_roms_buf, sizeof(default_roms_buf))) {
		return 1;
	}

	if (status_mode) {
		return print_status(&opts);
	}
	if (list_systems_mode) {
		return list_systems(&opts);
	}
	if (list_games_buf[0] != '\0') {
		return list_games_json(&opts, list_games_buf);
	}

	RunStats stats;
	bool ok = run_sync(&opts, &stats);
	fifo_close();
	return ok ? 0 : 1;
}
