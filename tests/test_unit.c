#define _POSIX_C_SOURCE 200809L

#include "../src/cache_writer.h"
#include "../src/ra_consoles.h"
#include "../src/sync_opts.h"

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

static void test_cache_format_size(void) {
	char buf[32];

	cache_format_size(0, buf, sizeof(buf));
	ASSERT_EQ_STR("0 KB", buf);

	cache_format_size(1, buf, sizeof(buf));
	ASSERT_EQ_STR("1 KB", buf);

	cache_format_size(102400, buf, sizeof(buf));
	ASSERT_EQ_STR("100 KB", buf);

	cache_format_size((1024ULL * 1024ULL) - 1024ULL, buf, sizeof(buf));
	ASSERT_EQ_STR("1023 KB", buf);

	cache_format_size(1024ULL * 1024ULL, buf, sizeof(buf));
	ASSERT_EQ_STR("1.0 MB", buf);

	cache_format_size(1572864ULL, buf, sizeof(buf));
	ASSERT_EQ_STR("1.5 MB", buf);

	cache_format_size(12ULL * 1024ULL * 1024ULL, buf, sizeof(buf));
	ASSERT_EQ_STR("12.0 MB", buf);
}

static bool write_dummy_file(const char* path, size_t size, const char* content) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	if (content) {
		fwrite(content, 1, strlen(content), f);
	} else {
		for (size_t i = 0; i < size; i++) {
			fputc('x', f);
		}
	}
	fclose(f);
	return true;
}

static int test_classify_game(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return -1;
	}
	char buf[512];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	rc_api_server_response_t srv = {
		.body = buf,
		.body_length = n,
		.http_status_code = 200
	};
	rc_api_fetch_game_sets_response_t sets;
	int rc = rc_api_process_fetch_game_sets_server_response(&sets, &srv);
	int result = -1;
	if (rc == RC_OK && sets.response.succeeded) {
		result = (sets.id != 0) ? 1 : 0;
	}
	rc_api_destroy_fetch_game_sets_response(&sets);
	return result;
}

static void test_cache_collect_stats(void) {
	char tmp_dir[] = "/tmp/ra_stats_test_XXXXXX";
	ASSERT_TRUE(mkdtemp(tmp_dir) != NULL);

	CacheStats empty_stats;
	cache_collect_stats(tmp_dir, test_classify_game, &empty_stats);
	ASSERT_EQ_INT(0, empty_stats.recognized_games);
	ASSERT_EQ_INT(0, empty_stats.unrecognized_games);
	ASSERT_EQ_INT(0, empty_stats.indexed_roms);
	ASSERT_EQ_INT(0, empty_stats.cached_badges);
	ASSERT_EQ_INT(0, empty_stats.total_bytes);

	char cache_dir[256];
	char badges_dir[256];
	snprintf(cache_dir, sizeof(cache_dir), "%s/.ra/offline/cache", tmp_dir);
	snprintf(badges_dir, sizeof(badges_dir), "%s/.ra/badges", tmp_dir);
	ASSERT_TRUE(cache_mkdirs(cache_dir));
	ASSERT_TRUE(cache_mkdirs(badges_dir));

	const char* rec_json =
		"{\"Success\":true,\"GameId\":228,\"Title\":\"SMW\","
		"\"ConsoleId\":3,\"ImageIconUrl\":\"/Images/1.png\",\"Sets\":[]}";
	const char* unrec_json =
		"{\"Success\":true,\"GameId\":0,\"Title\":\"\","
		"\"ConsoleId\":0,\"ImageIconUrl\":\"/Images/0.png\",\"Sets\":[]}";

	char f_rec1[512], f_rec2[512], f_unrec[512];
	char f_login[512], f_badge1[512], f_badge2[512];
	char f_ignored[512];
	snprintf(f_rec1, sizeof(f_rec1), "%s/achievementsets_aaa.bin", cache_dir);
	snprintf(f_rec2, sizeof(f_rec2), "%s/achievementsets_bbb.bin", cache_dir);
	snprintf(f_unrec, sizeof(f_unrec), "%s/achievementsets_ccc.bin", cache_dir);
	snprintf(f_login, sizeof(f_login), "%s/login.bin", cache_dir);
	snprintf(f_badge1, sizeof(f_badge1), "%s/100.png", badges_dir);
	snprintf(f_badge2, sizeof(f_badge2), "%s/100_lock.png", badges_dir);
	snprintf(f_ignored, sizeof(f_ignored), "%s/notes.txt", badges_dir);

	ASSERT_TRUE(write_dummy_file(f_rec1, 0, rec_json));
	ASSERT_TRUE(write_dummy_file(f_rec2, 0, rec_json));
	ASSERT_TRUE(write_dummy_file(f_unrec, 0, unrec_json));
	ASSERT_TRUE(write_dummy_file(f_login, 200, NULL));
	ASSERT_TRUE(write_dummy_file(f_badge1, 500, NULL));
	ASSERT_TRUE(write_dummy_file(f_badge2, 600, NULL));
	ASSERT_TRUE(write_dummy_file(f_ignored, 999, NULL));

	char index_path[CACHE_MAX_PATH];
	cache_index_path(tmp_dir, index_path, sizeof(index_path));
	ScraperIndex idx;
	scraper_index_init(&idx);
	ASSERT_TRUE(scraper_index_upsert(&idx, "SFC/a.sfc", RC_CONSOLE_SUPER_NINTENDO, 10, 100, "0123456789abcdef0123456789abcdef"));
	ASSERT_TRUE(scraper_index_upsert(&idx, "SFC/b.sfc", RC_CONSOLE_SUPER_NINTENDO, 20, 200, "fedcba9876543210fedcba9876543210"));
	ASSERT_TRUE(scraper_index_upsert(&idx, "SFC/c.sfc", RC_CONSOLE_SUPER_NINTENDO, 30, 300, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	ASSERT_TRUE(scraper_index_save(&idx, index_path));
	scraper_index_free(&idx);

	CacheStats stats;
	cache_collect_stats(tmp_dir, test_classify_game, &stats);
	ASSERT_EQ_INT(2, stats.recognized_games);
	ASSERT_EQ_INT(1, stats.unrecognized_games);
	ASSERT_EQ_INT(3, stats.indexed_roms);
	ASSERT_EQ_INT(2, stats.cached_badges);

	uint64_t expected_bytes = strlen(rec_json) * 2 + strlen(unrec_json) + 200 + 500 + 600;
	ASSERT_EQ_INT(expected_bytes, stats.total_bytes);

	unlink(f_rec1);
	unlink(f_rec2);
	unlink(f_unrec);
	unlink(f_login);
	unlink(f_badge1);
	unlink(f_badge2);
	unlink(f_ignored);
	unlink(index_path);
	rmdir(cache_dir);
	rmdir(badges_dir);
	char offline_dir[CACHE_MAX_PATH], ra_dir[CACHE_MAX_PATH];
	snprintf(offline_dir, sizeof(offline_dir), "%s/.ra/offline", tmp_dir);
	snprintf(ra_dir, sizeof(ra_dir), "%s/.ra", tmp_dir);
	rmdir(offline_dir);
	rmdir(ra_dir);
	rmdir(tmp_dir);
}

static void test_ra_is_disc_console(void) {
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_PLAYSTATION));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_PLAYSTATION_2));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_PSP));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_SEGA_CD));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_SATURN));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_DREAMCAST));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_PC_ENGINE_CD));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_NEO_GEO_CD));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_3DO));
	ASSERT_TRUE(ra_is_disc_console(RC_CONSOLE_GAMECUBE));

	ASSERT_TRUE(!ra_is_disc_console(RC_CONSOLE_SUPER_NINTENDO));
	ASSERT_TRUE(!ra_is_disc_console(RC_CONSOLE_GAMEBOY_ADVANCE));
	ASSERT_TRUE(!ra_is_disc_console(RC_CONSOLE_MEGA_DRIVE));
	ASSERT_TRUE(!ra_is_disc_console(RC_CONSOLE_ARCADE));
	ASSERT_TRUE(!ra_is_disc_console(RC_CONSOLE_UNKNOWN));
}

static void test_parse_badge_mode(void) {
	BadgeMode mode = BADGE_MODE_NONE;

	ASSERT_TRUE(parse_badge_mode("all", &mode));
	ASSERT_EQ_INT(BADGE_MODE_ALL, mode);

	ASSERT_TRUE(parse_badge_mode("unlocked", &mode));
	ASSERT_EQ_INT(BADGE_MODE_UNLOCKED, mode);

	ASSERT_TRUE(parse_badge_mode("none", &mode));
	ASSERT_EQ_INT(BADGE_MODE_NONE, mode);

	ASSERT_TRUE(!parse_badge_mode("invalid", &mode));
	ASSERT_TRUE(!parse_badge_mode("", &mode));
	ASSERT_TRUE(!parse_badge_mode(NULL, &mode));
	ASSERT_TRUE(!parse_badge_mode("all", NULL));
}

static void test_ra_format_display_name(void) {
	char buf[64];

	ra_format_display_name("Super Mario World (USA).sfc", buf, sizeof(buf), 24);
	ASSERT_EQ_STR("Super Mario World", buf);

	ra_format_display_name("Capcom Play System 2/Super Street Fighter II (USA) (Rev 1).zip", buf, sizeof(buf), 24);
	ASSERT_EQ_STR("Super Street Fighter II", buf);

	ra_format_display_name("Mario/Super Mario World Very Long Subtitle (USA) [!].zip", buf, sizeof(buf), 24);
	ASSERT_EQ_STR("Super Mario World Ver...", buf);

	ra_format_display_name("Short.gb", buf, sizeof(buf), 24);
	ASSERT_EQ_STR("Short", buf);

	ra_format_display_name("(Demo).gb", buf, sizeof(buf), 24);
	ASSERT_EQ_STR("(Demo)", buf);

	ra_format_display_name("", buf, sizeof(buf), 24);
	ASSERT_EQ_STR("", buf);
}

int main(void) {
	test_ra_extract_emu_tag();
	test_ra_get_console_id();
	test_cache_paths();
	test_scraper_index_lifecycle();
	test_game_sets_unrecognized_detection();
	test_cache_format_size();
	test_cache_collect_stats();
	test_ra_is_disc_console();
	test_parse_badge_mode();
	test_ra_format_display_name();

	if (g_tests_failed > 0) {
		fprintf(stderr, "%d/%d assertions failed\n", g_tests_failed, g_tests_run);
		return 1;
	}
	printf("All %d unit test assertions passed.\n", g_tests_run);
	return 0;
}
