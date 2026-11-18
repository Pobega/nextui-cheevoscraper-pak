#ifndef RA_CONSOLES_H
#define RA_CONSOLES_H

#include <rc_consoles.h>
#include <stddef.h>
#include <string.h>

typedef struct {
	const char* emu_tag;
	int console_id;
} RA_ConsoleMapping;

static const RA_ConsoleMapping ra_console_table[] = {
	{ "A2600",  RC_CONSOLE_ATARI_2600 },
	{ "A5200",  RC_CONSOLE_ATARI_5200 },
	{ "A7800",  RC_CONSOLE_ATARI_7800 },
	{ "32X",    RC_CONSOLE_SEGA_32X },
	{ "C128",   RC_CONSOLE_COMMODORE_64 },
	{ "C64",    RC_CONSOLE_COMMODORE_64 },
	{ "COLECO", RC_CONSOLE_COLECOVISION },
	{ "CPC",    RC_CONSOLE_AMSTRAD_PC },
	{ "FC",     RC_CONSOLE_NINTENDO },
	{ "FBN",    RC_CONSOLE_ARCADE },
	{ "FDS",    RC_CONSOLE_FAMICOM_DISK_SYSTEM },
	{ "GB",     RC_CONSOLE_GAMEBOY },
	{ "GBA",    RC_CONSOLE_GAMEBOY_ADVANCE },
	{ "GBC",    RC_CONSOLE_GAMEBOY_COLOR },
	{ "GG",     RC_CONSOLE_GAME_GEAR },
	{ "LYNX",   RC_CONSOLE_ATARI_LYNX },
	{ "MD",     RC_CONSOLE_MEGA_DRIVE },
	{ "MGBA",   RC_CONSOLE_GAMEBOY_ADVANCE },
	{ "MSX",    RC_CONSOLE_MSX },
	{ "NGP",    RC_CONSOLE_NEOGEO_POCKET },
	{ "NGPC",   RC_CONSOLE_NEOGEO_POCKET },
	{ "P8",     RC_CONSOLE_PICO },
	{ "PCE",    RC_CONSOLE_PC_ENGINE },
	{ "PET",    RC_CONSOLE_UNKNOWN },
	{ "PKM",    RC_CONSOLE_POKEMON_MINI },
	{ "PLUS4",  RC_CONSOLE_UNKNOWN },
	{ "PRBOOM", RC_CONSOLE_UNKNOWN },
	{ "PS",     RC_CONSOLE_PLAYSTATION },
	{ "PSX",    RC_CONSOLE_PLAYSTATION },
	{ "PUAE",   RC_CONSOLE_AMIGA },
	{ "SEGACD", RC_CONSOLE_SEGA_CD },
	{ "SFC",    RC_CONSOLE_SUPER_NINTENDO },
	{ "SG1000", RC_CONSOLE_SG1000 },
	{ "SGB",    RC_CONSOLE_GAMEBOY },
	{ "SMS",    RC_CONSOLE_MASTER_SYSTEM },
	{ "SUPA",   RC_CONSOLE_SUPER_NINTENDO },
	{ "VB",     RC_CONSOLE_VIRTUAL_BOY },
	{ "VIC",    RC_CONSOLE_VIC20 },
};

#define RA_CONSOLE_TABLE_SIZE (sizeof(ra_console_table) / sizeof(ra_console_table[0]))

static inline int ra_get_console_id(const char* emu_tag) {
	if (!emu_tag || !*emu_tag) {
		return RC_CONSOLE_UNKNOWN;
	}
	for (size_t i = 0; i < RA_CONSOLE_TABLE_SIZE; i++) {
		if (strcmp(emu_tag, ra_console_table[i].emu_tag) == 0) {
			return ra_console_table[i].console_id;
		}
	}
	return RC_CONSOLE_UNKNOWN;
}

static inline const char* ra_get_console_name(int console_id) {
	if (console_id == RC_CONSOLE_UNKNOWN) {
		return "Unknown";
	}
	return rc_console_name(console_id);
}

/**
 * Extract the EMU_TAG from a ROM folder name (e.g. "Super Nintendo (SFC)" -> "SFC").
 * Falls back to the full folder name if no parenthesized tag is present.
 */
static inline void ra_extract_emu_tag(const char* folder_name, char* out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (!folder_name || !*folder_name) {
		return;
	}

	const char* lparen = strrchr(folder_name, '(');
	const char* rparen = lparen ? strchr(lparen + 1, ')') : NULL;
	if (lparen && rparen && rparen > lparen + 1) {
		size_t len = (size_t)(rparen - (lparen + 1));
		if (len >= out_size) {
			len = out_size - 1;
		}
		memcpy(out, lparen + 1, len);
		out[len] = '\0';
		return;
	}

	size_t len = strlen(folder_name);
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, folder_name, len);
	out[len] = '\0';
}

#endif
