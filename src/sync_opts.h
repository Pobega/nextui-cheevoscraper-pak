#ifndef SYNC_OPTS_H
#define SYNC_OPTS_H

#include <stdbool.h>
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

#endif
