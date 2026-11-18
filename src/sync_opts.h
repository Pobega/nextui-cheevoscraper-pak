#ifndef SYNC_OPTS_H
#define SYNC_OPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum {
	BADGE_MODE_ALL = 0,
	BADGE_MODE_UNLOCKED = 1,
	BADGE_MODE_NONE = 2
} BadgeMode;

static inline bool parse_badge_mode(const char* s, BadgeMode* out) {
	if (!s || !out) {
		return false;
	}
	if (strcmp(s, "all") == 0) {
		*out = BADGE_MODE_ALL;
		return true;
	}
	if (strcmp(s, "unlocked") == 0) {
		*out = BADGE_MODE_UNLOCKED;
		return true;
	}
	if (strcmp(s, "none") == 0) {
		*out = BADGE_MODE_NONE;
		return true;
	}
	return false;
}

/**
 * Format a relative ROM path for compact list display: strips the final file
 * extension and truncates with "..." to at most max_chars characters.
 */
static inline void ra_format_display_name(const char* rel_path, char* out,
                                          size_t out_size, size_t max_chars) {
	if (!out || out_size == 0) {
		return;
	}
	if (!rel_path || !*rel_path) {
		out[0] = '\0';
		return;
	}

	const char* slash = strrchr(rel_path, '/');
	const char* dot = strrchr(rel_path, '.');
	size_t len = strlen(rel_path);
	if (dot && (!slash || dot > slash) && dot > rel_path) {
		len = (size_t)(dot - rel_path);
	}

	if (max_chars > 3 && len > max_chars && out_size > max_chars) {
		size_t keep = max_chars - 3;
		memcpy(out, rel_path, keep);
		memcpy(out + keep, "...", 3);
		out[max_chars] = '\0';
		return;
	}

	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, rel_path, len);
	out[len] = '\0';
}

#endif
