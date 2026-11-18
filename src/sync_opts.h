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
 * Format a relative ROM path for compact list display, matching NextUI's game
 * overview: takes the basename, strips the file extension and (...) / [...]
 * tags, trims trailing whitespace, and truncates with "..." to max_chars.
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
	const char* base = slash ? (slash + 1) : rel_path;
	if (!*base) {
		out[0] = '\0';
		return;
	}

	const char* dot = strrchr(base, '.');
	size_t base_len = (dot && dot > base) ? (size_t)(dot - base) : strlen(base);

	char clean[256];
	size_t clean_len = 0;
	int paren_depth = 0;
	int bracket_depth = 0;
	for (size_t i = 0; i < base_len && clean_len + 1 < sizeof(clean); i++) {
		char c = base[i];
		if (c == '(') {
			paren_depth++;
		} else if (c == ')' && paren_depth > 0) {
			paren_depth--;
		} else if (c == '[') {
			bracket_depth++;
		} else if (c == ']' && bracket_depth > 0) {
			bracket_depth--;
		} else if (paren_depth == 0 && bracket_depth == 0) {
			clean[clean_len++] = c;
		}
	}
	while (clean_len > 0 && (clean[clean_len - 1] == ' ' || clean[clean_len - 1] == '\t')) {
		clean_len--;
	}
	clean[clean_len] = '\0';

	const char* src = clean;
	size_t len = clean_len;
	if (len == 0) {
		src = base;
		len = base_len;
	}

	if (max_chars > 3 && len > max_chars && out_size > max_chars) {
		size_t keep = max_chars - 3;
		memcpy(out, src, keep);
		memcpy(out + keep, "...", 3);
		out[max_chars] = '\0';
		return;
	}

	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, src, len);
	out[len] = '\0';
}

#endif
