#define _POSIX_C_SOURCE 200809L

#include "../src/cache_writer.h"
#include "../src/ra_consoles.h"

#include <rc_api_runtime.h>
#include <rc_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define ASSERT_TRUE(expr) do { \
	g_tests_run++; \
	if (!(expr)) { \
		fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
		g_tests_failed++; \
		return; \
	} \
} while (0)

#define ASSERT_EQ_INT(expected, actual) do { \
	g_tests_run++; \
	long long _exp = (long long)(expected); \
	long long _act = (long long)(actual); \
	if (_exp != _act) { \
		fprintf(stderr, "FAIL [%s:%d]: expected %lld, got %lld (%s)\n", \
		        __FILE__, __LINE__, _exp, _act, #actual); \
		g_tests_failed++; \
		return; \
	} \
} while (0)

#define ASSERT_EQ_STR(expected, actual) do { \
	g_tests_run++; \
	const char* _exp = (expected); \
	const char* _act = (actual); \
	if (!_act || strcmp(_exp, _act) != 0) { \
		fprintf(stderr, "FAIL [%s:%d]: expected \"%s\", got \"%s\"\n", \
		        __FILE__, __LINE__, _exp, _act ? _act : "(null)"); \
		g_tests_failed++; \
		return; \
	} \
} while (0)

static void test_ra_extract_emu_tag(void) {
	char tag[64];

	ra_extract_emu_tag("Super Nintendo (SFC)", tag, sizeof(tag));
	ASSERT_EQ_STR("SFC", tag);

	ra_extract_emu_tag("Game Boy Advance (MGBA)", tag, sizeof(tag));
	ASSERT_EQ_STR("MGBA", tag);

	ra_extract_emu_tag("Mega Drive (EU) (MD)", tag, sizeof(tag));
	ASSERT_EQ_STR("MD", tag);

	ra_extract_emu_tag("GBA", tag, sizeof(tag));
	ASSERT_EQ_STR("GBA", tag);

	ra_extract_emu_tag("Empty ()", tag, sizeof(tag));
	ASSERT_EQ_STR("Empty ()", tag);

	ra_extract_emu_tag("", tag, sizeof(tag));
	ASSERT_EQ_STR("", tag);

	ra_extract_emu_tag(NULL, tag, sizeof(tag));
	ASSERT_EQ_STR("", tag);

	char small[4];
	ra_extract_emu_tag("Atari 2600 (A2600)", small, sizeof(small));
	ASSERT_EQ_STR("A26", small);
}

static void test_ra_get_console_id(void) {
	ASSERT_EQ_INT(RC_CONSOLE_SUPER_NINTENDO, ra_get_console_id("SFC"));
	ASSERT_EQ_INT(RC_CONSOLE_SUPER_NINTENDO, ra_get_console_id("SUPA"));
	ASSERT_EQ_INT(RC_CONSOLE_GAMEBOY_ADVANCE, ra_get_console_id("GBA"));
	ASSERT_EQ_INT(RC_CONSOLE_GAMEBOY_ADVANCE, ra_get_console_id("MGBA"));
	ASSERT_EQ_INT(RC_CONSOLE_ARCADE, ra_get_console_id("FBN"));
	ASSERT_EQ_INT(RC_CONSOLE_PLAYSTATION, ra_get_console_id("PS"));
	ASSERT_EQ_INT(RC_CONSOLE_PLAYSTATION, ra_get_console_id("PSX"));
	ASSERT_EQ_INT(RC_CONSOLE_SEGA_CD, ra_get_console_id("SEGACD"));
	ASSERT_EQ_INT(RC_CONSOLE_UNKNOWN, ra_get_console_id("PET"));
	ASSERT_EQ_INT(RC_CONSOLE_UNKNOWN, ra_get_console_id("PLUS4"));
	ASSERT_EQ_INT(RC_CONSOLE_UNKNOWN, ra_get_console_id("PRBOOM"));
	ASSERT_EQ_INT(RC_CONSOLE_UNKNOWN, ra_get_console_id("NONEXISTENT"));
	ASSERT_EQ_INT(RC_CONSOLE_UNKNOWN, ra_get_console_id(""));
	ASSERT_EQ_INT(RC_CONSOLE_UNKNOWN, ra_get_console_id(NULL));

	ASSERT_EQ_STR("Unknown", ra_get_console_name(RC_CONSOLE_UNKNOWN));
	ASSERT_EQ_STR("Super Nintendo Entertainment System", ra_get_console_name(RC_CONSOLE_SUPER_NINTENDO));
}

static void test_cache_paths(void) {
	char buf[CACHE_MAX_PATH];

	cache_login_path("/userdata", buf, sizeof(buf));
	ASSERT_EQ_STR("/userdata/.ra/offline/cache/login.bin", buf);

	cache_achievementsets_path("/userdata", "deadbeef0123", buf, sizeof(buf));
	ASSERT_EQ_STR("/userdata/.ra/offline/cache/achievementsets_deadbeef0123.bin", buf);

	cache_startsession_path("/userdata", "deadbeef0123", buf, sizeof(buf));
	ASSERT_EQ_STR("/userdata/.ra/offline/cache/startsession_deadbeef0123.bin", buf);

	cache_badge_path("/userdata", "12345", false, buf, sizeof(buf));
	ASSERT_EQ_STR("/userdata/.ra/badges/12345.png", buf);

	cache_badge_path("/userdata", "12345", true, buf, sizeof(buf));
	ASSERT_EQ_STR("/userdata/.ra/badges/12345_lock.png", buf);

	cache_index_path("/userdata", buf, sizeof(buf));
	ASSERT_EQ_STR("/userdata/.ra/offline/scraper_index.tsv", buf);
}

static void test_scraper_index_lifecycle(void) {
	char tmp_dir[] = "/tmp/ra_cache_test_XXXXXX";
	ASSERT_TRUE(mkdtemp(tmp_dir) != NULL);

	char nested_dir[CACHE_MAX_PATH];
	snprintf(nested_dir, sizeof(nested_dir), "%s/.ra/offline", tmp_dir);
	ASSERT_TRUE(cache_mkdirs(nested_dir));

	char index_path[CACHE_MAX_PATH];
	cache_index_path(tmp_dir, index_path, sizeof(index_path));

	ScraperIndex idx;
	scraper_index_init(&idx);
	ASSERT_TRUE(!idx.dirty);
	ASSERT_TRUE(!scraper_index_load(&idx, index_path));

	const char* hash1 = "0123456789abcdef0123456789abcdef";
	const char* hash2 = "fedcba9876543210fedcba9876543210";
	const char* hash3 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

	ASSERT_TRUE(scraper_index_upsert(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1000, 524288, hash1));
	ASSERT_TRUE(scraper_index_upsert(&idx, "GBA/Metroid Fusion.gba", RC_CONSOLE_GAMEBOY_ADVANCE, 2000, 8388608, hash2));
	ASSERT_TRUE(idx.dirty);
	ASSERT_EQ_INT(2, idx.count);

	ASSERT_EQ_STR(hash1, scraper_index_lookup(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1000, 524288));
	ASSERT_TRUE(scraper_index_lookup(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1001, 524288) == NULL);
	ASSERT_TRUE(scraper_index_lookup(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1000, 524289) == NULL);
	ASSERT_TRUE(scraper_index_lookup(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_GAMEBOY_ADVANCE, 1000, 524288) == NULL);
	ASSERT_TRUE(scraper_index_lookup(&idx, "SFC/Missing.sfc", RC_CONSOLE_SUPER_NINTENDO, 1000, 524288) == NULL);

	/* Upsert existing path+console updates in place rather than duplicating. */
	ASSERT_TRUE(scraper_index_upsert(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1500, 524288, hash3));
	ASSERT_EQ_INT(2, idx.count);
	ASSERT_EQ_STR(hash3, scraper_index_lookup(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1500, 524288));
	ASSERT_TRUE(scraper_index_lookup(&idx, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1000, 524288) == NULL);

	ASSERT_TRUE(scraper_index_save(&idx, index_path));
	ASSERT_TRUE(!idx.dirty);
	ASSERT_TRUE(cache_file_exists_nonempty(index_path));
	scraper_index_free(&idx);

	ScraperIndex reloaded;
	scraper_index_init(&reloaded);
	ASSERT_TRUE(scraper_index_load(&reloaded, index_path));
	ASSERT_TRUE(!reloaded.dirty);
	ASSERT_EQ_INT(2, reloaded.count);
	ASSERT_EQ_STR(hash3, scraper_index_lookup(&reloaded, "SFC/Super Mario World.sfc", RC_CONSOLE_SUPER_NINTENDO, 1500, 524288));
	ASSERT_EQ_STR(hash2, scraper_index_lookup(&reloaded, "GBA/Metroid Fusion.gba", RC_CONSOLE_GAMEBOY_ADVANCE, 2000, 8388608));
	scraper_index_free(&reloaded);

	unlink(index_path);
	rmdir(nested_dir);
	char ra_dir[CACHE_MAX_PATH];
	snprintf(ra_dir, sizeof(ra_dir), "%s/.ra", tmp_dir);
	rmdir(ra_dir);
	rmdir(tmp_dir);
}

static void test_game_sets_unrecognized_detection(void) {
	const char* unrecognized_json =
		"{\"Success\":true,\"GameId\":0,\"Title\":\"\","
		"\"ConsoleId\":0,\"ImageIconUrl\":\"/Images/000001.png\",\"Sets\":[]}";
	rc_api_server_response_t srv_unrec = {
		.body = unrecognized_json,
		.body_length = strlen(unrecognized_json),
		.http_status_code = 200
	};
	rc_api_fetch_game_sets_response_t sets_unrec;
	int rc = rc_api_process_fetch_game_sets_server_response(&sets_unrec, &srv_unrec);
	ASSERT_EQ_INT(RC_OK, rc);
	ASSERT_TRUE(sets_unrec.response.succeeded);
	ASSERT_EQ_INT(0, sets_unrec.id);
	rc_api_destroy_fetch_game_sets_response(&sets_unrec);

	const char* recognized_json =
		"{\"Success\":true,\"GameId\":228,\"Title\":\"Super Mario World\","
		"\"ConsoleId\":3,\"ImageIconUrl\":\"/Images/054719.png\",\"Sets\":[]}";
	rc_api_server_response_t srv_rec = {
		.body = recognized_json,
		.body_length = strlen(recognized_json),
		.http_status_code = 200
	};
	rc_api_fetch_game_sets_response_t sets_rec;
	rc = rc_api_process_fetch_game_sets_server_response(&sets_rec, &srv_rec);
	ASSERT_EQ_INT(RC_OK, rc);
	ASSERT_TRUE(sets_rec.response.succeeded);
	ASSERT_EQ_INT(228, sets_rec.id);
	rc_api_destroy_fetch_game_sets_response(&sets_rec);
}

int main(void) {
	test_ra_extract_emu_tag();
	test_ra_get_console_id();
	test_cache_paths();
	test_scraper_index_lifecycle();
	test_game_sets_unrecognized_detection();

	if (g_tests_failed > 0) {
		fprintf(stderr, "%d/%d assertions failed\n", g_tests_failed, g_tests_run);
		return 1;
	}
	printf("All %d unit test assertions passed.\n", g_tests_run);
	return 0;
}
