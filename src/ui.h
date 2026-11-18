#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	const char* userdata_dir;
	const char* roms_dir;
	const char* fifo_path;
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
	bool cancelled;
	char error_message[256];
} RunStats;

typedef struct {
	const char* phase;
	const char* item_name;
	size_t current;
	size_t total;
	const RunStats* stats;
} SyncProgress;

typedef void (*SyncProgressFn)(const SyncProgress* progress, void* userdata);
typedef bool (*SyncExecuteFn)(const SyncOptions* opts, SyncProgressFn progress_cb,
                              void* cb_userdata, bool* cancel_requested, RunStats* out_stats);

/**
 * Run the native SDL2 + SDL2_ttf UI loop.
 * Returns 0 on normal exit, or 1 if SDL initialization fails.
 */
int ui_run(const SyncOptions* base_opts, const char* username, bool logged_in, SyncExecuteFn sync_fn);

#endif
