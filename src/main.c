#define _POSIX_C_SOURCE 200809L

#include "cache_writer.h"
#include "http_client.h"
#include "ra_consoles.h"
#include "rom_hasher.h"

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

typedef struct {
	char username[SETTINGS_STR_MAX];
	char server_username[SETTINGS_STR_MAX];
	char token[SETTINGS_STR_MAX];
	int hardcore;
} RASettings;

typedef struct {
	const char* userdata_dir;
	const char* roms_dir;
	const char* fifo_path;
	int delay_ms;
	int limit;
	bool dry_run;
	bool force;
	bool refresh_sessions;
} CliOptions;

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

static void ui_status(const CliOptions* opts, int progress, const char* fmt, ...) {
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

static void scan_console_dir(const char* dir_path, const char* rel_prefix,
                             int console_id, int limit, RomList* list) {
	if (limit > 0 && list->count >= (size_t)limit) {
		return;
	}

	DIR* dir = opendir(dir_path);
	if (!dir) {
		return;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (limit > 0 && list->count >= (size_t)limit) {
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
			scan_console_dir(abs_path, rel_path, console_id, limit, list);
		} else if (S_ISREG(st.st_mode)) {
			if (!rom_should_skip(abs_path, dir_path, ent->d_name)) {
				rom_list_push(list, abs_path, rel_path, console_id, st.st_mtime, (int64_t)st.st_size);
			}
		}
	}

	closedir(dir);
}

static void collect_roms(const char* roms_dir, int limit, RomList* list) {
	DIR* root = opendir(roms_dir);
	if (!root) {
		fprintf(stderr, "Error: cannot open ROMs directory: %s (%s)\n", roms_dir, strerror(errno));
		return;
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

		scan_console_dir(folder_path, ent->d_name, console_id, limit, list);
	}

	closedir(root);
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

static bool perform_login(const CliOptions* opts, HttpClient* http,
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

static bool fetch_or_load_game_sets(const CliOptions* opts, HttpClient* http,
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

static void cache_startsession(const CliOptions* opts, HttpClient* http,
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

static void download_badge_variant(const CliOptions* opts, HttpClient* http,
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

static void cache_game_badges(const CliOptions* opts, HttpClient* http,
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

static void print_usage(const char* prog) {
	printf("Usage: %s [options]\n"
	       "  --userdata <dir>     Shared userdata dir (default: $SHARED_USERDATA_PATH or %s)\n"
	       "  --roms <dir>         ROMs root dir (default: $SDCARD_PATH/Roms or %s)\n"
	       "  --fifo <path>        FIFO path for show2.elf TEXT/PROGRESS updates\n"
	       "  --delay-ms <ms>      Delay between HTTP requests (default: %d)\n"
	       "  --limit <n>          Maximum ROMs to scan\n"
	       "  --dry-run            Scan and hash only; do not write cache or hit network\n"
	       "  --force              Re-hash ROMs and re-fetch existing cache files\n"
	       "  --refresh-sessions   Re-fetch startsession cache files for all matched games\n"
	       "  --help               Show this help message\n",
	       prog, DEFAULT_USERDATA_PATH, DEFAULT_ROMS_PATH, HTTP_DEFAULT_DELAY_MS);
}

static bool parse_args(int argc, char** argv, CliOptions* opts, char* default_roms_buf, size_t roms_buf_sz) {
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
	opts->delay_ms = HTTP_DEFAULT_DELAY_MS;
	opts->limit = 0;
	opts->dry_run = false;
	opts->force = false;
	opts->refresh_sessions = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--userdata") == 0 && i + 1 < argc) {
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
	return true;
}

int main(int argc, char** argv) {
	char default_roms_buf[CACHE_MAX_PATH];
	CliOptions opts;
	if (!parse_args(argc, argv, &opts, default_roms_buf, sizeof(default_roms_buf))) {
		return 1;
	}

	RASettings settings;
	if (!load_ra_settings(opts.userdata_dir, &settings)) {
		ui_status(&opts, -1, "Missing NextUI settings file");
		fifo_close();
		return 1;
	}

	const char* username = effective_username(&settings);
	if (!*username || !*settings.token) {
		ui_status(&opts, -1, "No RetroAchievements login in NextUI settings");
		fifo_close();
		return 1;
	}

	if (!opts.dry_run && !ensure_cache_dirs(opts.userdata_dir)) {
		ui_status(&opts, -1, "Failed to create .ra cache directories");
		fifo_close();
		return 1;
	}

	HttpClient http;
	http_client_init(&http, opts.delay_ms);

	ui_status(&opts, 0, "Verifying RetroAchievements login...");
	if (!perform_login(&opts, &http, username, settings.token)) {
		ui_status(&opts, -1, "RetroAchievements login failed");
		fifo_close();
		return 1;
	}

	ui_status(&opts, 5, "Scanning ROM folders...");
	RomList roms = {0};
	collect_roms(opts.roms_dir, opts.limit, &roms);

	ScraperIndex index;
	scraper_index_init(&index);
	char index_path[CACHE_MAX_PATH];
	cache_index_path(opts.userdata_dir, index_path, sizeof(index_path));
	if (!opts.force) {
		scraper_index_load(&index, index_path);
	}

	RunStats stats = {0};
	stats.roms_scanned = roms.count;

	HashSet unique_hashes = {0};
	for (size_t i = 0; i < roms.count; i++) {
		RomEntry* rom = &roms.items[i];
		int pct = roms.count > 0
			? 5 + (int)(((i + 1) * (PROGRESS_SCAN_MAX - 5)) / roms.count)
			: PROGRESS_SCAN_MAX;

		const char* cached_hash = opts.force ? NULL
			: scraper_index_lookup(&index, rom->rel_path, rom->console_id, rom->mtime, rom->size);

		if (cached_hash) {
			snprintf(rom->hash, sizeof(rom->hash), "%s", cached_hash);
			stats.index_hits++;
		} else {
			ui_status(&opts, pct, "Hashing (%zu/%zu): %s", i + 1, roms.count, basename_ptr(rom->rel_path));
			if (!rom_hash_file(rom->abs_path, rom->console_id, rom->hash)) {
				stats.hash_failures++;
				continue;
			}
			stats.roms_hashed++;
			scraper_index_upsert(&index, rom->rel_path, rom->console_id, rom->mtime, rom->size, rom->hash);
		}

		hash_set_add(&unique_hashes, rom->hash, basename_ptr(rom->rel_path));
	}

	if (!opts.dry_run && !scraper_index_save(&index, index_path)) {
		fprintf(stderr, "Warning: failed to save scraper index %s\n", index_path);
	}
	scraper_index_free(&index);
	rom_list_free(&roms);

	stats.unique_games = unique_hashes.count;
	StringSet seen_badges = {0};

	for (size_t i = 0; i < unique_hashes.count; i++) {
		const char* hash = unique_hashes.hashes[i];
		const char* name = unique_hashes.sample_names[i];
		int pct = PROGRESS_SCAN_MAX +
			(int)(((i + 1) * (PROGRESS_COMPLETE - PROGRESS_SCAN_MAX)) / unique_hashes.count);

		ui_status(&opts, pct, "Caching (%zu/%zu): %s", i + 1, unique_hashes.count, name);

		rc_api_fetch_game_sets_response_t sets;
		if (!fetch_or_load_game_sets(&opts, &http, username, settings.token, hash, &sets, &stats)) {
			continue;
		}

		if (sets.id != 0) {
			uint32_t session_gid = sets.session_game_id ? sets.session_game_id : sets.id;
			cache_startsession(&opts, &http, username, settings.token,
			                   settings.hardcore, session_gid, hash, &stats);
			cache_game_badges(&opts, &http, &sets, &seen_badges, &stats);
		}

		rc_api_destroy_fetch_game_sets_response(&sets);
	}

	string_set_free(&seen_badges);
	hash_set_free(&unique_hashes);

	ui_status(&opts, PROGRESS_COMPLETE,
	          "Done! %zu games (%zu fetched, %zu cached), %zu badges, %zu errors",
	          stats.unique_games, stats.sets_fetched, stats.sets_cached,
	          stats.badges_downloaded, stats.errors);

	printf("Summary: scanned=%zu index_hits=%zu hashed=%zu hash_fail=%zu unique=%zu "
	       "sets_fetched=%zu sets_cached=%zu sessions=%zu badges=%zu errors=%zu\n",
	       stats.roms_scanned, stats.index_hits, stats.roms_hashed, stats.hash_failures,
	       stats.unique_games, stats.sets_fetched, stats.sets_cached,
	       stats.sessions_fetched, stats.badges_downloaded, stats.errors);

	fifo_close();
	return 0;
}
