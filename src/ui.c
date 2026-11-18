#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "embedded_font_rounded.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_JOYSTICKS 16
#define AXIS_DEADZONE 16000
#define JOY_MENU 8
#define JOY_START 7
#define PROGRESS_REDRAW_MS 33
#define MENU_ITEM_COUNT 4
#define DEFAULT_WIN_W 640
#define DEFAULT_WIN_H 480

static const SDL_Color COL_BG          = {15, 17, 23, 255};
static const SDL_Color COL_CARD        = {27, 31, 42, 255};
static const SDL_Color COL_CARD_BORDER = {46, 53, 72, 255};
static const SDL_Color COL_SEL_BG      = {38, 46, 64, 255};
static const SDL_Color COL_TRACK       = {18, 22, 32, 255};
static const SDL_Color COL_TEXT        = {255, 255, 255, 255};
static const SDL_Color COL_MUTED       = {154, 164, 178, 255};
static const SDL_Color COL_ACCENT_GOLD = {229, 160, 13, 255};
static const SDL_Color COL_ACCENT_BLUE = {59, 130, 246, 255};
static const SDL_Color COL_ERROR       = {239, 68, 68, 255};

typedef enum {
	UI_ACT_NONE = 0,
	UI_ACT_UP,
	UI_ACT_DOWN,
	UI_ACT_SELECT,
	UI_ACT_BACK,
	UI_ACT_QUIT
} UIAction;

typedef struct {
	SDL_Joystick* stick;
	SDL_JoystickID id;
	Uint8 last_hat;
	int axis_y_dir;
} JoystickState;

typedef struct {
	SDL_Window* window;
	SDL_Renderer* renderer;
	int width;
	int height;
	float scale;

	TTF_Font* font_title;
	TTF_Font* font_sub;
	TTF_Font* font_body;
	TTF_Font* font_small;

	int btn_a;
	int btn_b;
	JoystickState joysticks[MAX_JOYSTICKS];
	int joy_count;
} UIContext;

typedef struct {
	UIContext* ui;
	bool* cancel_requested;
	Uint32 last_draw_ticks;
	const char* last_phase;
} ProgressState;

static void set_draw_color(SDL_Renderer* r, SDL_Color c) {
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void fill_rect(SDL_Renderer* r, int x, int y, int w, int h, SDL_Color c) {
	SDL_Rect rect = {x, y, w, h};
	set_draw_color(r, c);
	SDL_RenderFillRect(r, &rect);
}

static void draw_rect_outline(SDL_Renderer* r, int x, int y, int w, int h, SDL_Color c) {
	SDL_Rect rect = {x, y, w, h};
	set_draw_color(r, c);
	SDL_RenderDrawRect(r, &rect);
}

static int scaled_px(const UIContext* ui, int base_720, int min_px) {
	int v = (int)lroundf((float)base_720 * ui->scale);
	return v < min_px ? min_px : v;
}

static TTF_Font* open_embedded_font(int pt_size) {
	SDL_RWops* rw = SDL_RWFromConstMem(RoundedMplus1c_Bold_reduced_ttf,
	                                   (int)RoundedMplus1c_Bold_reduced_ttf_len);
	if (!rw) {
		return NULL;
	}
	return TTF_OpenFontRW(rw, 1, pt_size);
}

static void add_joystick(UIContext* ui, int device_index) {
	if (ui->joy_count >= MAX_JOYSTICKS) {
		return;
	}
	SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
	for (int i = 0; i < ui->joy_count; i++) {
		if (ui->joysticks[i].id == id) {
			return;
		}
	}
	SDL_Joystick* stick = SDL_JoystickOpen(device_index);
	if (!stick) {
		return;
	}
	JoystickState* js = &ui->joysticks[ui->joy_count++];
	js->stick = stick;
	js->id = SDL_JoystickInstanceID(stick);
	js->last_hat = SDL_HAT_CENTERED;
	js->axis_y_dir = 0;
}

static void remove_joystick(UIContext* ui, SDL_JoystickID id) {
	for (int i = 0; i < ui->joy_count; i++) {
		if (ui->joysticks[i].id == id) {
			SDL_JoystickClose(ui->joysticks[i].stick);
			for (int j = i; j < ui->joy_count - 1; j++) {
				ui->joysticks[j] = ui->joysticks[j + 1];
			}
			ui->joy_count--;
			return;
		}
	}
}

static JoystickState* find_joystick(UIContext* ui, SDL_JoystickID id) {
	for (int i = 0; i < ui->joy_count; i++) {
		if (ui->joysticks[i].id == id) {
			return &ui->joysticks[i];
		}
	}
	return NULL;
}

static bool ui_init(UIContext* ui) {
	memset(ui, 0, sizeof(*ui));

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}
	if (TTF_Init() != 0) {
		fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
		SDL_Quit();
		return false;
	}

	SDL_ShowCursor(SDL_DISABLE);
	SDL_JoystickEventState(SDL_ENABLE);

	ui->window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                              0, 0, SDL_WINDOW_SHOWN);
	if (!ui->window) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		TTF_Quit();
		SDL_Quit();
		return false;
	}

	ui->renderer = SDL_CreateRenderer(ui->window, -1,
	                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ui->renderer) {
		ui->renderer = SDL_CreateRenderer(ui->window, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!ui->renderer) {
		fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(ui->window);
		TTF_Quit();
		SDL_Quit();
		return false;
	}

	if (SDL_GetRendererOutputSize(ui->renderer, &ui->width, &ui->height) != 0 ||
	    ui->width <= 0 || ui->height <= 0) {
		SDL_GetWindowSize(ui->window, &ui->width, &ui->height);
	}
	if (ui->width <= 0 || ui->height <= 0) {
		ui->width = DEFAULT_WIN_W;
		ui->height = DEFAULT_WIN_H;
	}

	ui->scale = (float)ui->height / 720.0f;
	if (ui->scale < 0.6f) {
		ui->scale = 0.6f;
	}

	ui->font_title = open_embedded_font(scaled_px(ui, 34, 22));
	ui->font_sub   = open_embedded_font(scaled_px(ui, 22, 15));
	ui->font_body  = open_embedded_font(scaled_px(ui, 24, 16));
	ui->font_small = open_embedded_font(scaled_px(ui, 19, 13));
	if (!ui->font_title || !ui->font_sub || !ui->font_body || !ui->font_small) {
		fprintf(stderr, "TTF_OpenFontRW failed: %s\n", TTF_GetError());
		if (ui->font_title) TTF_CloseFont(ui->font_title);
		if (ui->font_sub) TTF_CloseFont(ui->font_sub);
		if (ui->font_body) TTF_CloseFont(ui->font_body);
		if (ui->font_small) TTF_CloseFont(ui->font_small);
		SDL_DestroyRenderer(ui->renderer);
		SDL_DestroyWindow(ui->window);
		TTF_Quit();
		SDL_Quit();
		return false;
	}

	const char* platform = getenv("PLATFORM");
	bool is_h700 = (platform && strcmp(platform, "h700") == 0);
	ui->btn_a = is_h700 ? 0 : 1;
	ui->btn_b = is_h700 ? 1 : 0;

	int n = SDL_NumJoysticks();
	for (int i = 0; i < n; i++) {
		add_joystick(ui, i);
	}

	return true;
}

static void ui_shutdown(UIContext* ui) {
	for (int i = 0; i < ui->joy_count; i++) {
		SDL_JoystickClose(ui->joysticks[i].stick);
	}
	ui->joy_count = 0;

	if (ui->font_title) TTF_CloseFont(ui->font_title);
	if (ui->font_sub) TTF_CloseFont(ui->font_sub);
	if (ui->font_body) TTF_CloseFont(ui->font_body);
	if (ui->font_small) TTF_CloseFont(ui->font_small);
	if (ui->renderer) SDL_DestroyRenderer(ui->renderer);
	if (ui->window) SDL_DestroyWindow(ui->window);
	TTF_Quit();
	SDL_Quit();
}

static UIAction translate_event(UIContext* ui, const SDL_Event* ev) {
	switch (ev->type) {
	case SDL_QUIT:
		return UI_ACT_QUIT;

	case SDL_JOYDEVICEADDED:
		add_joystick(ui, ev->jdevice.which);
		return UI_ACT_NONE;

	case SDL_JOYDEVICEREMOVED:
		remove_joystick(ui, ev->jdevice.which);
		return UI_ACT_NONE;

	case SDL_KEYDOWN:
		switch (ev->key.keysym.sym) {
		case SDLK_UP:
			return UI_ACT_UP;
		case SDLK_DOWN:
			return UI_ACT_DOWN;
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			return UI_ACT_SELECT;
		case SDLK_ESCAPE:
		case SDLK_BACKSPACE:
			return UI_ACT_BACK;
		default:
			return UI_ACT_NONE;
		}

	case SDL_JOYBUTTONDOWN:
		if (ev->jbutton.button == ui->btn_a || ev->jbutton.button == JOY_START) {
			return UI_ACT_SELECT;
		}
		if (ev->jbutton.button == ui->btn_b || ev->jbutton.button == JOY_MENU) {
			return UI_ACT_BACK;
		}
		return UI_ACT_NONE;

	case SDL_JOYHATMOTION: {
		JoystickState* js = find_joystick(ui, ev->jhat.which);
		Uint8 val = ev->jhat.value;
		Uint8 prev = js ? js->last_hat : SDL_HAT_CENTERED;
		if (js) {
			js->last_hat = val;
		}
		if ((val & SDL_HAT_UP) && !(prev & SDL_HAT_UP)) {
			return UI_ACT_UP;
		}
		if ((val & SDL_HAT_DOWN) && !(prev & SDL_HAT_DOWN)) {
			return UI_ACT_DOWN;
		}
		return UI_ACT_NONE;
	}

	case SDL_JOYAXISMOTION: {
		if (ev->jaxis.axis != 1) {
			return UI_ACT_NONE;
		}
		JoystickState* js = find_joystick(ui, ev->jaxis.which);
		int dir = 0;
		if (ev->jaxis.value < -AXIS_DEADZONE) {
			dir = -1;
		} else if (ev->jaxis.value > AXIS_DEADZONE) {
			dir = 1;
		}
		int prev = js ? js->axis_y_dir : 0;
		if (js) {
			js->axis_y_dir = dir;
		}
		if (dir == -1 && prev != -1) {
			return UI_ACT_UP;
		}
		if (dir == 1 && prev != 1) {
			return UI_ACT_DOWN;
		}
		return UI_ACT_NONE;
	}

	default:
		return UI_ACT_NONE;
	}
}

static void truncate_to_width(TTF_Font* font, const char* src, char* dst, size_t dst_sz, int max_w) {
	if (!src || !*src || dst_sz == 0) {
		if (dst_sz > 0) dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_sz, "%s", src);
	if (max_w <= 0) {
		return;
	}

	int w = 0;
	if (TTF_SizeUTF8(font, dst, &w, NULL) == 0 && w <= max_w) {
		return;
	}

	size_t len = strlen(dst);
	while (len > 1) {
		len--;
		if (len + 4 >= dst_sz) {
			continue;
		}
		memcpy(dst, src, len);
		memcpy(dst + len, "...", 4);
		if (TTF_SizeUTF8(font, dst, &w, NULL) == 0 && w <= max_w) {
			return;
		}
	}
	snprintf(dst, dst_sz, "...");
}

static void draw_text_fit(UIContext* ui, TTF_Font* font, const char* text,
                          int x, int y, int max_w, SDL_Color color, bool right_align) {
	if (!text || !*text) {
		return;
	}
	char buf[256];
	const char* to_draw = text;
	if (max_w > 0) {
		truncate_to_width(font, text, buf, sizeof(buf), max_w);
		to_draw = buf;
	}

	SDL_Surface* surf = TTF_RenderUTF8_Blended(font, to_draw, color);
	if (!surf) {
		return;
	}
	SDL_Texture* tex = SDL_CreateTextureFromSurface(ui->renderer, surf);
	if (tex) {
		int draw_x = right_align ? (x - surf->w) : x;
		SDL_Rect dst = {draw_x, y, surf->w, surf->h};
		SDL_RenderCopy(ui->renderer, tex, NULL, &dst);
		SDL_DestroyTexture(tex);
	}
	SDL_FreeSurface(surf);
}

static bool has_suffix(const char* name, const char* suffix) {
	size_t nlen = strlen(name);
	size_t slen = strlen(suffix);
	return nlen > slen && strcmp(name + nlen - slen, suffix) == 0;
}

static void count_cached_assets(const char* userdata_dir, size_t* out_games, size_t* out_badges) {
	*out_games = 0;
	*out_badges = 0;
	if (!userdata_dir || !*userdata_dir) {
		return;
	}

	char path[1024];
	snprintf(path, sizeof(path), "%s/.ra/offline/cache", userdata_dir);
	DIR* dir = opendir(path);
	if (dir) {
		struct dirent* ent;
		while ((ent = readdir(dir)) != NULL) {
			if (strncmp(ent->d_name, "achievementsets_", 16) == 0 &&
			    has_suffix(ent->d_name, ".bin")) {
				(*out_games)++;
			}
		}
		closedir(dir);
	}

	snprintf(path, sizeof(path), "%s/.ra/badges", userdata_dir);
	dir = opendir(path);
	if (dir) {
		struct dirent* ent;
		while ((ent = readdir(dir)) != NULL) {
			if (has_suffix(ent->d_name, ".png")) {
				(*out_badges)++;
			}
		}
		closedir(dir);
	}
}

static void render_header(UIContext* ui, const char* title, const char* sub1,
                          SDL_Color sub1_col, const char* sub2, int* out_card_y) {
	int pad_x = (int)((float)ui->width * 0.06f);
	int top_y = scaled_px(ui, 34, 20);
	int title_h = TTF_FontHeight(ui->font_title);
	int sub_h = TTF_FontHeight(ui->font_sub);
	int max_w = ui->width - 2 * pad_x;

	draw_text_fit(ui, ui->font_title, title, pad_x, top_y, max_w, COL_TEXT, false);
	int y = top_y + title_h + scaled_px(ui, 8, 5);

	if (sub1 && *sub1) {
		draw_text_fit(ui, ui->font_sub, sub1, pad_x, y, max_w, sub1_col, false);
		y += sub_h + scaled_px(ui, 5, 3);
	}
	if (sub2 && *sub2) {
		draw_text_fit(ui, ui->font_sub, sub2, pad_x, y, max_w, COL_MUTED, false);
		y += sub_h + scaled_px(ui, 12, 8);
	}
	*out_card_y = y + scaled_px(ui, 10, 6);
}

static void render_footer(UIContext* ui, const char* text) {
	int pad_x = (int)((float)ui->width * 0.06f);
	int foot_h = TTF_FontHeight(ui->font_small);
	int y = ui->height - foot_h - scaled_px(ui, 22, 14);
	draw_text_fit(ui, ui->font_small, text, pad_x, y, ui->width - 2 * pad_x, COL_MUTED, false);
}

static void render_menu(UIContext* ui, const char* username, bool logged_in,
                        size_t cached_games, size_t cached_badges, int selected) {
	static const char* ITEMS[MENU_ITEM_COUNT] = {
		"Sync New Games",
		"Refresh Sessions",
		"Force Rebuild Cache",
		"Exit"
	};

	set_draw_color(ui->renderer, COL_BG);
	SDL_RenderClear(ui->renderer);

	char sub1[300];
	SDL_Color sub1_col = COL_MUTED;
	if (logged_in && username && *username) {
		snprintf(sub1, sizeof(sub1), "User: %s", username);
	} else {
		snprintf(sub1, sizeof(sub1), "Not logged in — configure RA in NextUI Settings");
		sub1_col = COL_ACCENT_GOLD;
	}

	char sub2[128];
	snprintf(sub2, sizeof(sub2), "Cached: %zu games | %zu badges", cached_games, cached_badges);

	int card_y = 0;
	render_header(ui, "RetroAchievements Cache", sub1, sub1_col, sub2, &card_y);

	int pad_x = (int)((float)ui->width * 0.06f);
	int card_w = ui->width - 2 * pad_x;
	int footer_reserve = TTF_FontHeight(ui->font_small) + scaled_px(ui, 40, 26);
	int card_h = ui->height - card_y - footer_reserve;

	fill_rect(ui->renderer, pad_x, card_y, card_w, card_h, COL_CARD);
	draw_rect_outline(ui->renderer, pad_x, card_y, card_w, card_h, COL_CARD_BORDER);

	int inner_pad = scaled_px(ui, 20, 12);
	int gap = scaled_px(ui, 12, 8);
	int item_w = card_w - 2 * inner_pad;
	int item_h = (card_h - 2 * inner_pad - (MENU_ITEM_COUNT - 1) * gap) / MENU_ITEM_COUNT;
	int body_h = TTF_FontHeight(ui->font_body);
	int accent_w = scaled_px(ui, 6, 4);

	for (int i = 0; i < MENU_ITEM_COUNT; i++) {
		int ix = pad_x + inner_pad;
		int iy = card_y + inner_pad + i * (item_h + gap);
		bool is_sel = (i == selected);

		if (is_sel) {
			fill_rect(ui->renderer, ix, iy, item_w, item_h, COL_SEL_BG);
			fill_rect(ui->renderer, ix, iy, accent_w, item_h, COL_ACCENT_GOLD);
			draw_rect_outline(ui->renderer, ix, iy, item_w, item_h, COL_ACCENT_GOLD);
		} else {
			draw_rect_outline(ui->renderer, ix, iy, item_w, item_h, COL_CARD_BORDER);
		}

		int ty = iy + (item_h - body_h) / 2;
		int tx = ix + scaled_px(ui, 22, 14);
		draw_text_fit(ui, ui->font_body, ITEMS[i], tx, ty, item_w - scaled_px(ui, 36, 24),
		              is_sel ? COL_TEXT : COL_MUTED, false);
	}

	render_footer(ui, "A: Select   B: Exit");
	SDL_RenderPresent(ui->renderer);
}

static void render_stat_pair(UIContext* ui, const char* label, size_t value,
                             int x, int y, int col_w, SDL_Color val_col) {
	char val_buf[32];
	snprintf(val_buf, sizeof(val_buf), "%zu", value);
	draw_text_fit(ui, ui->font_sub, label, x, y, col_w / 2, COL_MUTED, false);
	draw_text_fit(ui, ui->font_sub, val_buf, x + col_w, y, col_w / 2, val_col, true);
}

static void render_progress(UIContext* ui, const SyncProgress* p, bool cancelling) {
	set_draw_color(ui->renderer, COL_BG);
	SDL_RenderClear(ui->renderer);

	int card_y = 0;
	render_header(ui, "RetroAchievements Cache", NULL, COL_MUTED, NULL, &card_y);

	int pad_x = (int)((float)ui->width * 0.06f);
	int card_w = ui->width - 2 * pad_x;
	int footer_reserve = TTF_FontHeight(ui->font_small) + scaled_px(ui, 40, 26);
	int card_h = ui->height - card_y - footer_reserve;

	fill_rect(ui->renderer, pad_x, card_y, card_w, card_h, COL_CARD);
	draw_rect_outline(ui->renderer, pad_x, card_y, card_w, card_h, COL_CARD_BORDER);

	int inner_pad = scaled_px(ui, 28, 18);
	int content_x = pad_x + inner_pad;
	int content_w = card_w - 2 * inner_pad;
	int y = card_y + inner_pad;

	const char* phase = (p->phase && *p->phase) ? p->phase : "Syncing...";
	draw_text_fit(ui, ui->font_body, phase, content_x, y, content_w, COL_ACCENT_GOLD, false);
	y += TTF_FontHeight(ui->font_body) + scaled_px(ui, 10, 6);

	const char* item = (p->item_name && *p->item_name) ? p->item_name : " ";
	draw_text_fit(ui, ui->font_sub, item, content_x, y, content_w, COL_TEXT, false);
	y += TTF_FontHeight(ui->font_sub) + scaled_px(ui, 18, 12);

	int bar_h = scaled_px(ui, 26, 18);
	fill_rect(ui->renderer, content_x, y, content_w, bar_h, COL_TRACK);
	draw_rect_outline(ui->renderer, content_x, y, content_w, bar_h, COL_CARD_BORDER);

	double ratio = 0.0;
	if (p->total > 0) {
		ratio = (double)p->current / (double)p->total;
		if (ratio < 0.0) ratio = 0.0;
		if (ratio > 1.0) ratio = 1.0;
	}
	int fill_w = (int)lround((double)(content_w - 2) * ratio);
	if (fill_w > 0) {
		fill_rect(ui->renderer, content_x + 1, y + 1, fill_w, bar_h - 2, COL_ACCENT_BLUE);
	}
	y += bar_h + scaled_px(ui, 10, 6);

	char pct_buf[64];
	int pct = (int)lround(ratio * 100.0);
	if (p->total > 0) {
		snprintf(pct_buf, sizeof(pct_buf), "%d%%  (%zu / %zu)", pct, p->current, p->total);
	} else {
		snprintf(pct_buf, sizeof(pct_buf), "Working...");
	}
	draw_text_fit(ui, ui->font_small, pct_buf, content_x, y, content_w, COL_MUTED, false);
	y += TTF_FontHeight(ui->font_small) + scaled_px(ui, 18, 12);

	fill_rect(ui->renderer, content_x, y, content_w, 1, COL_CARD_BORDER);
	y += scaled_px(ui, 18, 12);

	RunStats zero_stats = {0};
	const RunStats* st = p->stats ? p->stats : &zero_stats;
	int col_gap = scaled_px(ui, 48, 24);
	int col_w = (content_w - col_gap) / 2;
	int row_step = TTF_FontHeight(ui->font_sub) + scaled_px(ui, 14, 9);
	int col2_x = content_x + col_w + col_gap;

	render_stat_pair(ui, "Scanned", st->roms_scanned, content_x, y, col_w, COL_TEXT);
	render_stat_pair(ui, "Matched", st->unique_games, col2_x, y, col_w, COL_TEXT);
	y += row_step;

	render_stat_pair(ui, "Cached", st->sets_fetched + st->sets_cached, content_x, y, col_w, COL_TEXT);
	render_stat_pair(ui, "Badges", st->badges_downloaded, col2_x, y, col_w, COL_TEXT);
	y += row_step;

	render_stat_pair(ui, "Errors", st->errors, content_x, y, col_w,
	                 st->errors > 0 ? COL_ERROR : COL_TEXT);

	render_footer(ui, cancelling ? "Cancelling..." : "B: Cancel");
	SDL_RenderPresent(ui->renderer);
}

static void render_summary(UIContext* ui, const RunStats* st, bool ok) {
	set_draw_color(ui->renderer, COL_BG);
	SDL_RenderClear(ui->renderer);

	const char* title = "Sync Complete";
	if (!ok || st->error_message[0] != '\0') {
		title = "Sync Failed";
	} else if (st->cancelled) {
		title = "Sync Cancelled";
	}

	int card_y = 0;
	render_header(ui, title, NULL, COL_MUTED, NULL, &card_y);

	int pad_x = (int)((float)ui->width * 0.06f);
	int card_w = ui->width - 2 * pad_x;
	int footer_reserve = TTF_FontHeight(ui->font_small) + scaled_px(ui, 40, 26);
	int card_h = ui->height - card_y - footer_reserve;

	fill_rect(ui->renderer, pad_x, card_y, card_w, card_h, COL_CARD);
	draw_rect_outline(ui->renderer, pad_x, card_y, card_w, card_h, COL_CARD_BORDER);

	int inner_pad = scaled_px(ui, 28, 18);
	int content_x = pad_x + inner_pad;
	int content_w = card_w - 2 * inner_pad;
	int y = card_y + inner_pad;

	if (!ok && st->error_message[0] != '\0') {
		draw_text_fit(ui, ui->font_sub, st->error_message, content_x, y, content_w, COL_ERROR, false);
		y += TTF_FontHeight(ui->font_sub) + scaled_px(ui, 16, 10);
	} else if (st->cancelled) {
		draw_text_fit(ui, ui->font_sub, "Cancelled — partial progress has been saved.",
		              content_x, y, content_w, COL_ACCENT_GOLD, false);
		y += TTF_FontHeight(ui->font_sub) + scaled_px(ui, 16, 10);
	}

	int row_h = TTF_FontHeight(ui->font_body) + scaled_px(ui, 16, 10);
	struct {
		const char* label;
		size_t value;
		SDL_Color color;
	} rows[5] = {
		{"Scanned",      st->roms_scanned,      COL_TEXT},
		{"Matched",      st->unique_games,      COL_TEXT},
		{"Newly Cached", st->sets_fetched,      COL_ACCENT_BLUE},
		{"Badges",       st->badges_downloaded, COL_TEXT},
		{"Errors",       st->errors,            st->errors > 0 ? COL_ERROR : COL_TEXT}
	};

	for (int i = 0; i < 5; i++) {
		char val_buf[32];
		snprintf(val_buf, sizeof(val_buf), "%zu", rows[i].value);
		draw_text_fit(ui, ui->font_body, rows[i].label, content_x, y, content_w / 2, COL_MUTED, false);
		draw_text_fit(ui, ui->font_body, val_buf, content_x + content_w, y, content_w / 2,
		              rows[i].color, true);
		y += row_h;
		if (i < 4) {
			fill_rect(ui->renderer, content_x, y - scaled_px(ui, 8, 5), content_w, 1, COL_CARD_BORDER);
		}
	}

	render_footer(ui, "Press A or B to continue");
	SDL_RenderPresent(ui->renderer);
}

static void on_sync_progress(const SyncProgress* progress, void* userdata) {
	ProgressState* ps = (ProgressState*)userdata;
	bool was_cancelled = *ps->cancel_requested;

	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		UIAction act = translate_event(ps->ui, &ev);
		if (act == UI_ACT_BACK || act == UI_ACT_QUIT) {
			*ps->cancel_requested = true;
		}
	}

	Uint32 now = SDL_GetTicks();
	bool phase_changed = (!ps->last_phase || !progress->phase ||
	                      strcmp(ps->last_phase, progress->phase) != 0);
	bool finished_step = (progress->total > 0 && progress->current >= progress->total);
	bool cancel_flipped = (!was_cancelled && *ps->cancel_requested);

	if (ps->last_draw_ticks == 0 || phase_changed || finished_step || cancel_flipped ||
	    (now - ps->last_draw_ticks >= PROGRESS_REDRAW_MS)) {
		render_progress(ps->ui, progress, *ps->cancel_requested);
		ps->last_draw_ticks = now;
		ps->last_phase = progress->phase;
	}
}

static void show_summary_loop(UIContext* ui, const RunStats* stats, bool ok) {
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		(void)translate_event(ui, &ev);
	}

	bool needs_draw = true;
	bool waiting = true;
	while (waiting) {
		if (needs_draw) {
			render_summary(ui, stats, ok);
			needs_draw = false;
		}
		if (SDL_WaitEventTimeout(&ev, 50)) {
			do {
				if (ev.type == SDL_WINDOWEVENT &&
				    ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
					needs_draw = true;
				}
				UIAction act = translate_event(ui, &ev);
				if (act == UI_ACT_SELECT || act == UI_ACT_BACK || act == UI_ACT_QUIT) {
					waiting = false;
					break;
				}
			} while (SDL_PollEvent(&ev));
		}
	}
}

int ui_run(const SyncOptions* base_opts, const char* username, bool logged_in, SyncExecuteFn sync_fn) {
	UIContext ui;
	if (!ui_init(&ui)) {
		return 1;
	}

	int selected = 0;
	size_t cached_games = 0;
	size_t cached_badges = 0;
	count_cached_assets(base_opts->userdata_dir, &cached_games, &cached_badges);

	bool running = true;
	bool needs_draw = true;

	while (running) {
		if (needs_draw) {
			render_menu(&ui, username, logged_in, cached_games, cached_badges, selected);
			needs_draw = false;
		}

		SDL_Event ev;
		if (!SDL_WaitEventTimeout(&ev, 50)) {
			continue;
		}

		do {
			if (ev.type == SDL_WINDOWEVENT &&
			    ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
				needs_draw = true;
			}
			UIAction act = translate_event(&ui, &ev);
			if (act == UI_ACT_UP) {
				selected = (selected + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
				needs_draw = true;
			} else if (act == UI_ACT_DOWN) {
				selected = (selected + 1) % MENU_ITEM_COUNT;
				needs_draw = true;
			} else if (act == UI_ACT_BACK || act == UI_ACT_QUIT) {
				running = false;
				break;
			} else if (act == UI_ACT_SELECT) {
				if (selected == 3) {
					running = false;
					break;
				}

				SyncOptions run_opts = *base_opts;
				run_opts.force = (selected == 2);
				run_opts.refresh_sessions = (selected == 1);

				bool cancel_requested = false;
				ProgressState ps = {
					.ui = &ui,
					.cancel_requested = &cancel_requested,
					.last_draw_ticks = 0,
					.last_phase = NULL
				};

				RunStats stats;
				memset(&stats, 0, sizeof(stats));
				SyncProgress initial = {
					.phase = "Starting Sync",
					.item_name = "",
					.current = 0,
					.total = 0,
					.stats = &stats
				};
				render_progress(&ui, &initial, false);

				bool ok = sync_fn(&run_opts, on_sync_progress, &ps, &cancel_requested, &stats);
				show_summary_loop(&ui, &stats, ok);

				count_cached_assets(base_opts->userdata_dir, &cached_games, &cached_badges);
				needs_draw = true;
				break;
			}
		} while (SDL_PollEvent(&ev));
	}

	ui_shutdown(&ui);
	return 0;
}
