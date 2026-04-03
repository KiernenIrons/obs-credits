#include "credits-source.h"
#include "credits-parser.h"
#include "credits-renderer.h"
#include "discord-fetch.h"
#include "youtube-chat.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void credits_update(void *data, obs_data_t *settings);

struct credits_source {
	obs_source_t *self;

	/* Settings */
	float scroll_speed;
	bool loop;
	uint32_t width;
	uint32_t height;
	char *default_font_face;
	int default_font_size;

	/* Delay settings */
	float start_delay;
	float loop_delay;
	float delay_timer;

	/* State - two layers for flicker-free updates.
	 * Both render every frame. display_layer picks which is shown.
	 * On rebuild, the non-displayed layer gets the new layout.
	 * After 3 frames, display_layer swaps. */
	struct credits_data *data;
	struct {
		struct credits_layout *layout;
		gs_texrender_t *texrender;
	} layer[2];
	int display_layer;  /* 0 or 1: which layer is shown */
	int rebuild_warmup; /* counts up after rebuild, swaps at 3 */
	bool needs_rebuild;
	float scroll_offset;
	float current_speed;
	float total_height;
	bool scrolling;
	bool started;
	bool waiting_loop;
	bool paused;

	/* Hotkeys */
	obs_hotkey_id hotkey_start;
	obs_hotkey_id hotkey_pause;
	obs_hotkey_id hotkey_stop;

	/* Discord - fetched data stored as text for injection into sections */
	char *discord_sections[MAX_DISCORD_SECTIONS]; /* per-role names */
	bool discord_fetching;
	bool discord_has_fetched;

	/* YouTube chat */
	struct yt_chat_ctx yt_chat;
	bool yt_enabled;
	size_t yt_last_count;  /* track when new chatters arrive to trigger update */
	float yt_poll_timer;   /* accumulates seconds between count checks */

	pthread_mutex_t mutex;
};

/* ---- Helpers to build sections array from flat settings ---- */

static obs_data_array_t *
settings_to_sections_array(obs_data_t *settings)
{
	int count = (int)obs_data_get_int(settings, "section_count");
	if (count <= 0)
		return NULL;

	obs_data_array_t *arr = obs_data_array_create();
	char key[64];

	for (int i = 0; i < count; i++) {
		obs_data_t *sec = obs_data_create();

		snprintf(key, sizeof(key), "section_%d_heading", i);
		obs_data_set_string(sec, "heading",
				    obs_data_get_string(settings, key));

		snprintf(key, sizeof(key), "section_%d_subheading", i);
		obs_data_set_string(sec, "subheading",
				    obs_data_get_string(settings, key));

		snprintf(key, sizeof(key), "section_%d_names", i);
		obs_data_set_string(sec, "names",
				    obs_data_get_string(settings, key));

		snprintf(key, sizeof(key), "section_%d_roles", i);
		obs_data_set_string(sec, "roles",
				    obs_data_get_string(settings, key));

		snprintf(key, sizeof(key), "section_%d_alignment", i);
		obs_data_set_string(sec, "alignment",
				    obs_data_get_string(settings, key));

		/* Per-field font pickers (obj with face, size, flags, style) */
		const char *font_fields[] = {"heading_font", "sub_font",
					     "entry_font"};
		for (int f = 0; f < 3; f++) {
			snprintf(key, sizeof(key), "section_%d_%s", i,
				 font_fields[f]);
			obs_data_t *fobj =
				obs_data_get_obj(settings, key);
			if (fobj) {
				obs_data_set_obj(sec, font_fields[f], fobj);
				obs_data_release(fobj);
			}
		}

		/* Per-section colors */
		snprintf(key, sizeof(key), "section_%d_heading_color", i);
		obs_data_set_int(sec, "heading_color",
				 obs_data_get_int(settings, key));

		snprintf(key, sizeof(key), "section_%d_sub_color", i);
		obs_data_set_int(sec, "sub_color",
				 obs_data_get_int(settings, key));

		snprintf(key, sizeof(key), "section_%d_text_color", i);
		obs_data_set_int(sec, "text_color",
				 obs_data_get_int(settings, key));

		/* Per-section outline */
		snprintf(key, sizeof(key), "section_%d_outline_enabled", i);
		obs_data_set_bool(sec, "outline_enabled",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_outline_size", i);
		obs_data_set_int(sec, "outline_size",
				 obs_data_get_int(settings, key));

		snprintf(key, sizeof(key), "section_%d_outline_color", i);
		obs_data_set_int(sec, "outline_color",
				 obs_data_get_int(settings, key));

		snprintf(key, sizeof(key), "section_%d_outline_heading", i);
		obs_data_set_bool(sec, "outline_heading",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_outline_sub", i);
		obs_data_set_bool(sec, "outline_sub",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_outline_entries", i);
		obs_data_set_bool(sec, "outline_entries",
				  obs_data_get_bool(settings, key));

		/* Per-section shadow */
		snprintf(key, sizeof(key), "section_%d_shadow_enabled", i);
		obs_data_set_bool(sec, "shadow_enabled",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_shadow_color", i);
		obs_data_set_int(sec, "shadow_color",
				 obs_data_get_int(settings, key));

		snprintf(key, sizeof(key), "section_%d_shadow_offset_x", i);
		obs_data_set_double(sec, "shadow_offset_x",
				    obs_data_get_double(settings, key));

		snprintf(key, sizeof(key), "section_%d_shadow_offset_y", i);
		obs_data_set_double(sec, "shadow_offset_y",
				    obs_data_get_double(settings, key));

		snprintf(key, sizeof(key), "section_%d_shadow_heading", i);
		obs_data_set_bool(sec, "shadow_heading",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_shadow_sub", i);
		obs_data_set_bool(sec, "shadow_sub",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_shadow_entries", i);
		obs_data_set_bool(sec, "shadow_entries",
				  obs_data_get_bool(settings, key));

		/* Per-section spacing */
		snprintf(key, sizeof(key), "section_%d_heading_spacing", i);
		obs_data_set_double(sec, "heading_spacing",
				    obs_data_get_double(settings, key));

		snprintf(key, sizeof(key), "section_%d_sub_spacing", i);
		obs_data_set_double(sec, "sub_spacing",
				    obs_data_get_double(settings, key));

		snprintf(key, sizeof(key), "section_%d_entry_spacing", i);
		obs_data_set_double(sec, "entry_spacing",
				    obs_data_get_double(settings, key));

		snprintf(key, sizeof(key), "section_%d_section_spacing", i);
		obs_data_set_double(sec, "section_spacing",
				    obs_data_get_double(settings, key));

		obs_data_array_push_back(arr, sec);
		obs_data_release(sec);
	}

	return arr;
}

/* Helper: shift all per-section keys from src index to dst index */
static void shift_section_keys(obs_data_t *settings, int dst, int src)
{
	char sk[64], dk[64];

	/* String fields */
	const char *str_fields[] = {"heading", "subheading", "names", "roles",
				    "alignment"};
	for (int f = 0; f < 5; f++) {
		snprintf(sk, sizeof(sk), "section_%d_%s", src, str_fields[f]);
		snprintf(dk, sizeof(dk), "section_%d_%s", dst, str_fields[f]);
		obs_data_set_string(settings, dk,
				    obs_data_get_string(settings, sk));
	}

	/* Font picker objects */
	const char *font_fields[] = {"heading_font", "sub_font",
				     "entry_font"};
	for (int f = 0; f < 3; f++) {
		snprintf(sk, sizeof(sk), "section_%d_%s", src, font_fields[f]);
		snprintf(dk, sizeof(dk), "section_%d_%s", dst, font_fields[f]);
		obs_data_t *fobj = obs_data_get_obj(settings, sk);
		if (fobj) {
			obs_data_set_obj(settings, dk, fobj);
			obs_data_release(fobj);
		}
	}

	/* Per-section color ints */
	const char *int_fields[] = {"heading_color", "sub_color", "text_color",
				    "outline_size", "outline_color",
				    "shadow_color"};
	for (int f = 0; f < 6; f++) {
		snprintf(sk, sizeof(sk), "section_%d_%s", src, int_fields[f]);
		snprintf(dk, sizeof(dk), "section_%d_%s", dst, int_fields[f]);
		obs_data_set_int(settings, dk,
				 obs_data_get_int(settings, sk));
	}

	/* Per-section bools */
	const char *bool_fields[] = {"outline_enabled", "shadow_enabled",
				     "outline_heading", "outline_sub",
				     "outline_entries", "shadow_heading",
				     "shadow_sub", "shadow_entries"};
	for (int f = 0; f < 8; f++) {
		snprintf(sk, sizeof(sk), "section_%d_%s", src, bool_fields[f]);
		snprintf(dk, sizeof(dk), "section_%d_%s", dst, bool_fields[f]);
		obs_data_set_bool(settings, dk,
				  obs_data_get_bool(settings, sk));
	}

	/* Per-section doubles (shadow offsets + spacing) */
	const char *dbl_fields[] = {"shadow_offset_x", "shadow_offset_y",
				    "heading_spacing", "sub_spacing",
				    "entry_spacing", "section_spacing"};
	for (int f = 0; f < 6; f++) {
		snprintf(sk, sizeof(sk), "section_%d_%s", src, dbl_fields[f]);
		snprintf(dk, sizeof(dk), "section_%d_%s", dst, dbl_fields[f]);
		obs_data_set_double(settings, dk,
				    obs_data_get_double(settings, sk));
	}

	/* Expand state */
	snprintf(sk, sizeof(sk), "section_%d_expand", src);
	snprintf(dk, sizeof(dk), "section_%d_expand", dst);
	obs_data_set_bool(settings, dk,
			  obs_data_get_bool(settings, sk));
}

/* ---- Section add/remove via visibility toggling ---- */

/*
 * OBS does not rebuild the properties panel structure when a button
 * callback returns true - it only refreshes values and visibility.
 * Calling obs_source_update_properties crashes Qt (use-after-free).
 *
 * Solution: pre-create all section groups (up to MAX_SECTIONS) in
 * get_properties and hide the unused ones. Add/remove just changes
 * section_count and toggles visibility. No panel rebuild needed.
 */
#define MAX_SECTIONS 20

static void set_section_visibility(obs_properties_t *props, int idx,
				   bool visible)
{
	char key[64];
	snprintf(key, sizeof(key), "section_%d_group", idx);
	obs_property_t *p = obs_properties_get(props, key);
	if (p)
		obs_property_set_visible(p, visible);
}

static void update_remove_buttons(obs_properties_t *props, int count)
{
	/* Show remove buttons only when count > 1 */
	for (int i = 0; i < MAX_SECTIONS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "section_%d_remove", i);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, i < count && count > 1);
	}
}

static bool on_add_section(obs_properties_t *props, obs_property_t *prop,
			   void *data)
{
	UNUSED_PARAMETER(prop);

	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	int count = (int)obs_data_get_int(settings, "section_count");
	if (count >= MAX_SECTIONS) {
		obs_data_release(settings);
		return false;
	}

	int new_idx = count;
	obs_data_set_int(settings, "section_count", count + 1);

	/* Clear fields for new section */
	char key[64];
	const char *str_fields[] = {"heading", "subheading", "names", "roles"};
	for (int f = 0; f < 4; f++) {
		snprintf(key, sizeof(key), "section_%d_%s", new_idx,
			 str_fields[f]);
		obs_data_set_string(settings, key, "");
	}
	snprintf(key, sizeof(key), "section_%d_alignment", new_idx);
	obs_data_set_string(settings, key, "center");

	/* Set reasonable default font objects for new section */
	const char *font_fields[] = {"heading_font", "sub_font",
				     "entry_font"};
	int def_sizes[] = {72, 36, 36};
	for (int f = 0; f < 3; f++) {
		snprintf(key, sizeof(key), "section_%d_%s", new_idx,
			 font_fields[f]);
		obs_data_t *fobj = obs_data_create();
		obs_data_set_string(fobj, "face", "Arial");
		obs_data_set_int(fobj, "size", def_sizes[f]);
		obs_data_set_int(fobj, "flags", 0);
		obs_data_set_string(fobj, "style", "Regular");
		obs_data_set_obj(settings, key, fobj);
		obs_data_release(fobj);
	}

	/* Set per-section styling defaults */
	snprintf(key, sizeof(key), "section_%d_heading_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00FFD700);
	snprintf(key, sizeof(key), "section_%d_sub_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00CCCCCC);
	snprintf(key, sizeof(key), "section_%d_text_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00FFFFFF);

	snprintf(key, sizeof(key), "section_%d_outline_enabled", new_idx);
	obs_data_set_bool(settings, key, false);
	snprintf(key, sizeof(key), "section_%d_outline_size", new_idx);
	obs_data_set_int(settings, key, 2);
	snprintf(key, sizeof(key), "section_%d_outline_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00000000);
	snprintf(key, sizeof(key), "section_%d_outline_heading", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "section_%d_outline_sub", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "section_%d_outline_entries", new_idx);
	obs_data_set_bool(settings, key, true);

	snprintf(key, sizeof(key), "section_%d_shadow_enabled", new_idx);
	obs_data_set_bool(settings, key, false);
	snprintf(key, sizeof(key), "section_%d_shadow_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00333333);
	snprintf(key, sizeof(key), "section_%d_shadow_offset_x", new_idx);
	obs_data_set_double(settings, key, 2.0);
	snprintf(key, sizeof(key), "section_%d_shadow_offset_y", new_idx);
	obs_data_set_double(settings, key, 2.0);
	snprintf(key, sizeof(key), "section_%d_shadow_heading", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "section_%d_shadow_sub", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "section_%d_shadow_entries", new_idx);
	obs_data_set_bool(settings, key, true);

	snprintf(key, sizeof(key), "section_%d_heading_spacing", new_idx);
	obs_data_set_double(settings, key, 0.0);
	snprintf(key, sizeof(key), "section_%d_sub_spacing", new_idx);
	obs_data_set_double(settings, key, 0.0);
	snprintf(key, sizeof(key), "section_%d_entry_spacing", new_idx);
	obs_data_set_double(settings, key, 0.0);
	snprintf(key, sizeof(key), "section_%d_section_spacing", new_idx);
	obs_data_set_double(settings, key, 0.0);

	/* Ensure the new section starts expanded */
	snprintf(key, sizeof(key), "section_%d_expand", new_idx);
	obs_data_set_bool(settings, key, true);

	/* Show the newly visible section group */
	set_section_visibility(props, new_idx, true);
	update_remove_buttons(props, count + 1);

	obs_source_update(ctx->self, settings);
	obs_data_release(settings);
	return true;
}

static bool on_remove_section(obs_properties_t *props, obs_property_t *prop,
			      void *data)
{
	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "section_%d_remove", &idx) != 1) {
		obs_data_release(settings);
		return false;
	}

	int count = (int)obs_data_get_int(settings, "section_count");
	if (idx >= count || count <= 1) {
		obs_data_release(settings);
		return false;
	}

	/* Shift data from sections after idx down by one */
	for (int i = idx; i < count - 1; i++)
		shift_section_keys(settings, i, i + 1);

	obs_data_set_int(settings, "section_count", count - 1);

	/* Hide the last section group (now unused) */
	set_section_visibility(props, count - 1, false);
	update_remove_buttons(props, count - 1);

	obs_source_update(ctx->self, settings);
	obs_data_release(settings);
	return true;
}

/* ---- Discord section add/remove via visibility toggling ---- */

static void set_discord_section_visibility(obs_properties_t *props, int idx,
					   bool visible)
{
	char key[64];
	snprintf(key, sizeof(key), "dsection_%d_group", idx);
	obs_property_t *p = obs_properties_get(props, key);
	if (p)
		obs_property_set_visible(p, visible);
}

static void update_discord_remove_buttons(obs_properties_t *props, int count)
{
	for (int i = 0; i < MAX_DISCORD_SECTIONS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "dsection_%d_remove", i);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, i < count);
	}
}

/* Helper: shift all per-discord-section keys from src index to dst index */
static void shift_dsection_keys(obs_data_t *settings, int dst, int src)
{
	char sk[64], dk[64];

	/* String fields */
	const char *str_fields[] = {"heading", "role_id", "alignment"};
	for (int f = 0; f < 3; f++) {
		snprintf(sk, sizeof(sk), "dsection_%d_%s", src, str_fields[f]);
		snprintf(dk, sizeof(dk), "dsection_%d_%s", dst, str_fields[f]);
		obs_data_set_string(settings, dk,
				    obs_data_get_string(settings, sk));
	}

	/* Bool fields */
	const char *bool_fields[] = {"expand",
				     "outline_enabled", "shadow_enabled",
				     "outline_heading", "outline_sub",
				     "outline_entries", "shadow_heading",
				     "shadow_sub", "shadow_entries"};
	for (int f = 0; f < 9; f++) {
		snprintf(sk, sizeof(sk), "dsection_%d_%s", src, bool_fields[f]);
		snprintf(dk, sizeof(dk), "dsection_%d_%s", dst, bool_fields[f]);
		obs_data_set_bool(settings, dk,
				  obs_data_get_bool(settings, sk));
	}

	/* Font picker objects */
	const char *font_fields[] = {"heading_font", "entry_font"};
	for (int f = 0; f < 2; f++) {
		snprintf(sk, sizeof(sk), "dsection_%d_%s", src, font_fields[f]);
		snprintf(dk, sizeof(dk), "dsection_%d_%s", dst, font_fields[f]);
		obs_data_t *fobj = obs_data_get_obj(settings, sk);
		if (fobj) {
			obs_data_set_obj(settings, dk, fobj);
			obs_data_release(fobj);
		}
	}

	/* Int fields (colors + outline size) */
	const char *int_fields[] = {"heading_color", "text_color",
				    "outline_size", "outline_color",
				    "shadow_color"};
	for (int f = 0; f < 5; f++) {
		snprintf(sk, sizeof(sk), "dsection_%d_%s", src, int_fields[f]);
		snprintf(dk, sizeof(dk), "dsection_%d_%s", dst, int_fields[f]);
		obs_data_set_int(settings, dk,
				 obs_data_get_int(settings, sk));
	}

	/* Double fields */
	const char *dbl_fields[] = {"shadow_offset_x", "shadow_offset_y",
				    "heading_spacing", "entry_spacing",
				    "section_spacing"};
	for (int f = 0; f < 5; f++) {
		snprintf(sk, sizeof(sk), "dsection_%d_%s", src, dbl_fields[f]);
		snprintf(dk, sizeof(dk), "dsection_%d_%s", dst, dbl_fields[f]);
		obs_data_set_double(settings, dk,
				    obs_data_get_double(settings, sk));
	}
}

static bool on_add_discord_section(obs_properties_t *props,
				   obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(prop);

	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	int count = (int)obs_data_get_int(settings, "discord_section_count");
	if (count >= MAX_DISCORD_SECTIONS) {
		obs_data_release(settings);
		return false;
	}

	int new_idx = count;
	obs_data_set_int(settings, "discord_section_count", count + 1);

	/* Set defaults for new Discord section */
	char key[64];

	snprintf(key, sizeof(key), "dsection_%d_role_id", new_idx);
	obs_data_set_string(settings, key, "");
	snprintf(key, sizeof(key), "dsection_%d_heading", new_idx);
	obs_data_set_string(settings, key, "Members");
	snprintf(key, sizeof(key), "dsection_%d_alignment", new_idx);
	obs_data_set_string(settings, key, "center");

	/* Default font objects */
	const char *font_fields[] = {"heading_font", "entry_font"};
	int def_sizes[] = {72, 36};
	for (int f = 0; f < 2; f++) {
		snprintf(key, sizeof(key), "dsection_%d_%s", new_idx,
			 font_fields[f]);
		obs_data_t *fobj = obs_data_create();
		obs_data_set_string(fobj, "face", "Arial");
		obs_data_set_int(fobj, "size", def_sizes[f]);
		obs_data_set_int(fobj, "flags", 0);
		obs_data_set_string(fobj, "style", "Regular");
		obs_data_set_obj(settings, key, fobj);
		obs_data_release(fobj);
	}

	/* Default colors */
	snprintf(key, sizeof(key), "dsection_%d_heading_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00FFD700);
	snprintf(key, sizeof(key), "dsection_%d_text_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00FFFFFF);

	/* Default outline */
	snprintf(key, sizeof(key), "dsection_%d_outline_enabled", new_idx);
	obs_data_set_bool(settings, key, false);
	snprintf(key, sizeof(key), "dsection_%d_outline_size", new_idx);
	obs_data_set_int(settings, key, 2);
	snprintf(key, sizeof(key), "dsection_%d_outline_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00000000);
	snprintf(key, sizeof(key), "dsection_%d_outline_heading", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "dsection_%d_outline_sub", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "dsection_%d_outline_entries", new_idx);
	obs_data_set_bool(settings, key, true);

	/* Default shadow */
	snprintf(key, sizeof(key), "dsection_%d_shadow_enabled", new_idx);
	obs_data_set_bool(settings, key, false);
	snprintf(key, sizeof(key), "dsection_%d_shadow_color", new_idx);
	obs_data_set_int(settings, key, (long long)0x00333333);
	snprintf(key, sizeof(key), "dsection_%d_shadow_offset_x", new_idx);
	obs_data_set_double(settings, key, 2.0);
	snprintf(key, sizeof(key), "dsection_%d_shadow_offset_y", new_idx);
	obs_data_set_double(settings, key, 2.0);
	snprintf(key, sizeof(key), "dsection_%d_shadow_heading", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "dsection_%d_shadow_sub", new_idx);
	obs_data_set_bool(settings, key, true);
	snprintf(key, sizeof(key), "dsection_%d_shadow_entries", new_idx);
	obs_data_set_bool(settings, key, true);

	/* Default spacing */
	snprintf(key, sizeof(key), "dsection_%d_heading_spacing", new_idx);
	obs_data_set_double(settings, key, 0.0);
	snprintf(key, sizeof(key), "dsection_%d_entry_spacing", new_idx);
	obs_data_set_double(settings, key, 0.0);
	snprintf(key, sizeof(key), "dsection_%d_section_spacing", new_idx);
	obs_data_set_double(settings, key, 0.0);

	/* Start expanded */
	snprintf(key, sizeof(key), "dsection_%d_expand", new_idx);
	obs_data_set_bool(settings, key, true);

	/* Show the newly visible discord section group */
	set_discord_section_visibility(props, new_idx, true);
	update_discord_remove_buttons(props, count + 1);

	obs_source_update(ctx->self, settings);
	obs_data_release(settings);
	return true;
}

static bool on_remove_discord_section(obs_properties_t *props,
				      obs_property_t *prop, void *data)
{
	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "dsection_%d_remove", &idx) != 1) {
		obs_data_release(settings);
		return false;
	}

	int count = (int)obs_data_get_int(settings, "discord_section_count");
	if (idx >= count || count <= 0) {
		obs_data_release(settings);
		return false;
	}

	/* Shift data from sections after idx down by one */
	for (int i = idx; i < count - 1; i++)
		shift_dsection_keys(settings, i, i + 1);

	/* Also shift the fetched discord data */
	pthread_mutex_lock(&ctx->mutex);
	bfree(ctx->discord_sections[idx]);
	for (int i = idx; i < count - 1; i++)
		ctx->discord_sections[i] = ctx->discord_sections[i + 1];
	ctx->discord_sections[count - 1] = NULL;
	pthread_mutex_unlock(&ctx->mutex);

	obs_data_set_int(settings, "discord_section_count", count - 1);

	/* Hide the last discord section group (now unused) */
	set_discord_section_visibility(props, count - 1, false);
	update_discord_remove_buttons(props, count - 1);

	obs_source_update(ctx->self, settings);
	obs_data_release(settings);
	return true;
}

/* ---- Discord fetch (background thread) ---- */

struct discord_fetch_args {
	struct credits_source *ctx;
	char *bot_token;
	char *guild_id;
	struct discord_fetch_config configs[MAX_DISCORD_SECTIONS];
	int num_configs;
};

/* Helper: build a newline-separated string from a discord_member array.
 * Returns NULL if there are no members. Caller must bfree the result. */
static char *members_to_text(const struct discord_member *members,
			     size_t count)
{
	if (count == 0)
		return NULL;

	size_t buf_size = 0;
	for (size_t i = 0; i < count; i++)
		buf_size += strlen(members[i].display_name) + 1; /* +1 for \n */

	char *text = bmalloc(buf_size + 1);
	text[0] = '\0';
	for (size_t i = 0; i < count; i++) {
		if (i > 0)
			strcat(text, "\n");
		strcat(text, members[i].display_name);
	}
	return text;
}

static void *discord_fetch_thread(void *arg)
{
	struct discord_fetch_args *a = arg;
	struct credits_source *ctx = a->ctx;

	blog(LOG_INFO, "[obs-credits] Discord fetch started...");

	struct discord_result *res =
		discord_fetch(a->bot_token, a->guild_id,
			      a->configs, a->num_configs);

	if (res->error) {
		blog(LOG_WARNING, "[obs-credits] Discord fetch error: %s",
		     res->error);
	} else {
		char *section_texts[MAX_DISCORD_SECTIONS];
		memset(section_texts, 0, sizeof(section_texts));

		for (int i = 0; i < res->num_sections; i++) {
			section_texts[i] =
				members_to_text(res->sections[i].members,
						res->sections[i].num_members);
		}

		pthread_mutex_lock(&ctx->mutex);
		for (int i = 0; i < MAX_DISCORD_SECTIONS; i++) {
			bfree(ctx->discord_sections[i]);
			ctx->discord_sections[i] = section_texts[i];
		}
		ctx->discord_has_fetched = true;
		pthread_mutex_unlock(&ctx->mutex);

		size_t total_members = 0;
		for (int i = 0; i < res->num_sections; i++)
			total_members += res->sections[i].num_members;

		blog(LOG_INFO,
		     "[obs-credits] Discord fetch complete: %d sections, "
		     "%zu total matched members",
		     res->num_sections, total_members);
	}

	discord_result_free(res);
	bfree(a->bot_token);
	bfree(a->guild_id);
	for (int i = 0; i < a->num_configs; i++)
		bfree(a->configs[i].role_id);
	bfree(a);

	pthread_mutex_lock(&ctx->mutex);
	ctx->discord_fetching = false;
	pthread_mutex_unlock(&ctx->mutex);

	/* Trigger update to rebuild layout with fetched data */
	obs_data_t *settings = obs_source_get_settings(ctx->self);
	obs_source_update(ctx->self, settings);
	obs_data_release(settings);

	return NULL;
}

/* Shared helper: start Discord fetch on background thread */
static void start_discord_fetch(struct credits_source *ctx)
{
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->discord_fetching) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	ctx->discord_fetching = true;
	pthread_mutex_unlock(&ctx->mutex);

	obs_data_t *settings = obs_source_get_settings(ctx->self);

	if (!obs_data_get_bool(settings, "discord_enabled")) {
		obs_data_release(settings);
		pthread_mutex_lock(&ctx->mutex);
		ctx->discord_fetching = false;
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	const char *token = obs_data_get_string(settings, "discord_token");
	const char *guild = obs_data_get_string(settings, "discord_guild_id");

	if (!token || token[0] == '\0' || !guild || guild[0] == '\0') {
		obs_data_release(settings);
		pthread_mutex_lock(&ctx->mutex);
		ctx->discord_fetching = false;
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	int dscount =
		(int)obs_data_get_int(settings, "discord_section_count");
	if (dscount > MAX_DISCORD_SECTIONS)
		dscount = MAX_DISCORD_SECTIONS;

	/* Need at least one role section to fetch */
	if (dscount <= 0) {
		obs_data_release(settings);
		pthread_mutex_lock(&ctx->mutex);
		ctx->discord_fetching = false;
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	struct discord_fetch_args *args =
		bzalloc(sizeof(struct discord_fetch_args));
	args->ctx = ctx;
	args->bot_token = bstrdup(token);
	args->guild_id = bstrdup(guild);

	char key[64];
	for (int i = 0; i < dscount && i < MAX_DISCORD_SECTIONS; i++) {
		snprintf(key, sizeof(key), "dsection_%d_role_id", i);
		const char *rid = obs_data_get_string(settings, key);
		blog(LOG_INFO,
		     "[obs-credits] Discord config %d: role_id=%s",
		     i, rid ? rid : "(null)");
		args->configs[i].is_booster = false;
		args->configs[i].role_id =
			(rid && rid[0] != '\0') ? bstrdup(rid) : NULL;
	}

	args->num_configs = dscount;

	obs_data_release(settings);

	pthread_t thread;
	pthread_create(&thread, NULL, discord_fetch_thread, args);
	pthread_detach(thread);
}

static bool on_discord_fetch(obs_properties_t *props, obs_property_t *prop,
			     void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	start_discord_fetch(data);
	return false;
}

/* Called when the source becomes visible (scene switched to) */
static void credits_activate(void *data)
{
	struct credits_source *ctx = data;

	/* Restart scroll from beginning (respects start delay) */
	pthread_mutex_lock(&ctx->mutex);
	ctx->scroll_offset = -(float)ctx->height;
	ctx->current_speed = 0.0f;
	ctx->scrolling = true;
	ctx->started = false;
	ctx->paused = false;
	ctx->waiting_loop = false;
	ctx->delay_timer = 0.0f;
	pthread_mutex_unlock(&ctx->mutex);

	/* Always fetch Discord data on scene activation */
	start_discord_fetch(ctx);
}

static bool on_discord_toggled(void *data, obs_properties_t *props,
			       obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(prop);
	bool enabled = obs_data_get_bool(settings, "discord_enabled");

	obs_property_set_visible(obs_properties_get(props, "discord_token"),
				 enabled);
	obs_property_set_visible(
		obs_properties_get(props, "discord_guild_id"), enabled);
	/* Show/hide all discord section groups */
	int dscount =
		(int)obs_data_get_int(settings, "discord_section_count");
	for (int i = 0; i < MAX_DISCORD_SECTIONS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "dsection_%d_group", i);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, enabled && i < dscount);
	}

	obs_property_set_visible(
		obs_properties_get(props, "add_discord_section"), enabled);
	obs_property_set_visible(obs_properties_get(props, "discord_fetch"),
				 enabled);
	return true;
}

/* ---- Section expand/collapse callback ---- */

static bool on_section_expand_toggled(void *data, obs_properties_t *props,
				      obs_property_t *prop,
				      obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	/* Extract section index from property name: "section_N_expand" */
	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "section_%d_expand", &idx) != 1)
		return false;

	bool expanded = obs_data_get_bool(settings, pname);

	char key[64];
	const char *fields[] = {"heading", "heading_font", "subheading",
				"sub_font", "names", "roles", "entry_font",
				"alignment",
				"heading_color", "sub_color", "text_color",
				"outline_enabled",
				"shadow_enabled",
				"heading_spacing", "sub_spacing",
				"entry_spacing", "section_spacing"};
	for (int f = 0; f < 17; f++) {
		snprintf(key, sizeof(key), "section_%d_%s", idx, fields[f]);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, expanded);
	}

	/* For outline/shadow sub-properties, respect their toggle state */
	if (expanded) {
		snprintf(key, sizeof(key), "section_%d_outline_enabled", idx);
		bool ol_on = obs_data_get_bool(settings, key);
		snprintf(key, sizeof(key), "section_%d_outline_size", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && ol_on);
		snprintf(key, sizeof(key), "section_%d_outline_color", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && ol_on);
		snprintf(key, sizeof(key), "section_%d_outline_heading", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && ol_on);
		snprintf(key, sizeof(key), "section_%d_outline_sub", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && ol_on);
		snprintf(key, sizeof(key), "section_%d_outline_entries", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && ol_on);

		snprintf(key, sizeof(key), "section_%d_shadow_enabled", idx);
		bool sh_on = obs_data_get_bool(settings, key);
		snprintf(key, sizeof(key), "section_%d_shadow_color", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && sh_on);
		snprintf(key, sizeof(key), "section_%d_shadow_offset_x", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && sh_on);
		snprintf(key, sizeof(key), "section_%d_shadow_offset_y", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && sh_on);
		snprintf(key, sizeof(key), "section_%d_shadow_heading", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && sh_on);
		snprintf(key, sizeof(key), "section_%d_shadow_sub", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && sh_on);
		snprintf(key, sizeof(key), "section_%d_shadow_entries", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 expanded && sh_on);
	}

	return true;
}

/* ---- Per-section outline/shadow visibility callbacks ---- */

static bool on_section_outline_toggled(void *data, obs_properties_t *props,
				       obs_property_t *prop,
				       obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	/* Extract section index from property name: "section_N_outline_enabled" */
	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "section_%d_outline_enabled", &idx) != 1)
		return false;

	bool enabled = obs_data_get_bool(settings, pname);
	char key[64];

	snprintf(key, sizeof(key), "section_%d_outline_size", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_outline_color", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_outline_heading", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_outline_sub", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_outline_entries", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);

	return true;
}

static bool on_section_shadow_toggled(void *data, obs_properties_t *props,
				      obs_property_t *prop,
				      obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	/* Extract section index from property name: "section_N_shadow_enabled" */
	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "section_%d_shadow_enabled", &idx) != 1)
		return false;

	bool enabled = obs_data_get_bool(settings, pname);
	char key[64];

	snprintf(key, sizeof(key), "section_%d_shadow_color", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_shadow_offset_x", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_shadow_offset_y", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_shadow_heading", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_shadow_sub", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "section_%d_shadow_entries", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);

	return true;
}

/* ---- Discord section expand/collapse callback ---- */

static bool on_discord_section_expand_toggled(void *data,
					      obs_properties_t *props,
					      obs_property_t *prop,
					      obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "dsection_%d_expand", &idx) != 1)
		return false;

	bool expanded = obs_data_get_bool(settings, pname);

	char key[64];
	const char *fields[] = {"role_id", "heading",
				"heading_font", "entry_font", "alignment",
				"heading_color", "text_color",
				"outline_enabled",
				"shadow_enabled",
				"heading_spacing", "entry_spacing",
				"section_spacing"};
	for (int f = 0; f < 12; f++) {
		snprintf(key, sizeof(key), "dsection_%d_%s", idx, fields[f]);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, expanded);
	}

	if (expanded) {
		/* Outline sub-properties */
		snprintf(key, sizeof(key), "dsection_%d_outline_enabled", idx);
		bool ol_on = obs_data_get_bool(settings, key);
		snprintf(key, sizeof(key), "dsection_%d_outline_size", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 ol_on);
		snprintf(key, sizeof(key), "dsection_%d_outline_color", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 ol_on);
		snprintf(key, sizeof(key), "dsection_%d_outline_heading", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 ol_on);
		snprintf(key, sizeof(key), "dsection_%d_outline_sub", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 ol_on);
		snprintf(key, sizeof(key), "dsection_%d_outline_entries", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 ol_on);

		/* Shadow sub-properties */
		snprintf(key, sizeof(key), "dsection_%d_shadow_enabled", idx);
		bool sh_on = obs_data_get_bool(settings, key);
		snprintf(key, sizeof(key), "dsection_%d_shadow_color", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 sh_on);
		snprintf(key, sizeof(key), "dsection_%d_shadow_offset_x", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 sh_on);
		snprintf(key, sizeof(key), "dsection_%d_shadow_offset_y", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 sh_on);
		snprintf(key, sizeof(key), "dsection_%d_shadow_heading", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 sh_on);
		snprintf(key, sizeof(key), "dsection_%d_shadow_sub", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 sh_on);
		snprintf(key, sizeof(key), "dsection_%d_shadow_entries", idx);
		obs_property_set_visible(obs_properties_get(props, key),
					 sh_on);
	}

	return true;
}

/* ---- Discord section outline/shadow visibility callbacks ---- */

static bool on_dsection_outline_toggled(void *data, obs_properties_t *props,
					obs_property_t *prop,
					obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "dsection_%d_outline_enabled", &idx) != 1)
		return false;

	bool enabled = obs_data_get_bool(settings, pname);
	char key[64];

	snprintf(key, sizeof(key), "dsection_%d_outline_size", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_outline_color", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_outline_heading", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_outline_sub", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_outline_entries", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);

	return true;
}

static bool on_dsection_shadow_toggled(void *data, obs_properties_t *props,
				       obs_property_t *prop,
				       obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	const char *pname = obs_property_name(prop);
	int idx = 0;
	if (sscanf(pname, "dsection_%d_shadow_enabled", &idx) != 1)
		return false;

	bool enabled = obs_data_get_bool(settings, pname);
	char key[64];

	snprintf(key, sizeof(key), "dsection_%d_shadow_color", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_shadow_offset_x", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_shadow_offset_y", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_shadow_heading", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_shadow_sub", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);
	snprintf(key, sizeof(key), "dsection_%d_shadow_entries", idx);
	obs_property_set_visible(obs_properties_get(props, key), enabled);

	return true;
}

/* ---- YouTube chat streaming state callback ---- */

static void on_streaming_state_changed(enum obs_frontend_event event,
				       void *data)
{
	struct credits_source *ctx = data;

	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		/* Read yt_enabled and channel_url from source settings */
		obs_data_t *settings = obs_source_get_settings(ctx->self);
		bool enabled = obs_data_get_bool(settings, "yt_enabled");
		if (enabled) {
			const char *url =
				obs_data_get_string(settings, "yt_channel_url");
			blog(LOG_INFO,
			     "[obs-credits] Stream started - starting YouTube chat for: %s",
			     url ? url : "(empty)");
			yt_chat_start(&ctx->yt_chat, url);
		}
		obs_data_release(settings);
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		blog(LOG_INFO,
		     "[obs-credits] Stream stopped - stopping YouTube chat");
		yt_chat_stop(&ctx->yt_chat);
	}
}

/* ---- YouTube chat properties toggle callback ---- */

static bool on_yt_toggled(void *data, obs_properties_t *props,
			  obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "yt_enabled");

	/* Show/hide root-level YouTube properties */
	const char *root_props[] = {
		"yt_channel_url",
		"yt_start_collecting",
	};
	for (int i = 0; i < 2; i++) {
		obs_property_t *p = obs_properties_get(props, root_props[i]);
		if (p)
			obs_property_set_visible(p, enabled);
	}

	/* Show/hide the styling group */
	obs_property_t *grp = obs_properties_get(props, "yt_section_group");
	if (grp)
		obs_property_set_visible(grp, enabled);

	return true;
}

static bool on_yt_expand_toggled(void *data, obs_properties_t *props,
				 obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(prop);

	bool expanded = obs_data_get_bool(settings, "yt_expand");

	const char *top_props[] = {
		"yt_heading",
		"yt_heading_font",
		"yt_entry_font",
		"yt_alignment",
		"yt_heading_color",
		"yt_text_color",
		"yt_outline_enabled",
		"yt_shadow_enabled",
		"yt_heading_spacing",
		"yt_entry_spacing",
		"yt_section_spacing",
	};
	for (int i = 0; i < 11; i++) {
		obs_property_t *p = obs_properties_get(props, top_props[i]);
		if (p)
			obs_property_set_visible(p, expanded);
	}

	/* outline sub-properties */
	bool ol_on = obs_data_get_bool(settings, "yt_outline_enabled");
	const char *ol_sub[] = {"yt_outline_size", "yt_outline_color",
				"yt_outline_heading", "yt_outline_entries"};
	for (int i = 0; i < 4; i++) {
		obs_property_t *p = obs_properties_get(props, ol_sub[i]);
		if (p)
			obs_property_set_visible(p, expanded && ol_on);
	}

	/* shadow sub-properties */
	bool sh_on = obs_data_get_bool(settings, "yt_shadow_enabled");
	const char *sh_sub[] = {"yt_shadow_color", "yt_shadow_offset_x",
				"yt_shadow_offset_y", "yt_shadow_heading",
				"yt_shadow_entries"};
	for (int i = 0; i < 5; i++) {
		obs_property_t *p = obs_properties_get(props, sh_sub[i]);
		if (p)
			obs_property_set_visible(p, expanded && sh_on);
	}

	return true;
}

static bool on_yt_start_collecting(obs_properties_t *props,
				   obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	if (!obs_data_get_bool(settings, "yt_enabled")) {
		obs_data_release(settings);
		return false;
	}

	const char *url = obs_data_get_string(settings, "yt_channel_url");
	if (url && url[0] != '\0') {
		yt_chat_stop(&ctx->yt_chat);
		yt_chat_clear(&ctx->yt_chat);
		yt_chat_start(&ctx->yt_chat, url);
		blog(LOG_INFO,
		     "[obs-credits] YouTube chat: manual start for %s", url);
	}

	obs_data_release(settings);
	return false;
}

static bool on_yt_outline_toggled(void *data, obs_properties_t *props,
				  obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "yt_outline_enabled");
	const char *sub[] = {"yt_outline_size", "yt_outline_color",
			     "yt_outline_heading", "yt_outline_entries"};
	for (int i = 0; i < 4; i++) {
		obs_property_t *p = obs_properties_get(props, sub[i]);
		if (p)
			obs_property_set_visible(p, enabled);
	}
	return true;
}

static bool on_yt_shadow_toggled(void *data, obs_properties_t *props,
				 obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "yt_shadow_enabled");
	const char *sub[] = {"yt_shadow_color", "yt_shadow_offset_x",
			     "yt_shadow_offset_y", "yt_shadow_heading",
			     "yt_shadow_entries"};
	for (int i = 0; i < 5; i++) {
		obs_property_t *p = obs_properties_get(props, sub[i]);
		if (p)
			obs_property_set_visible(p, enabled);
	}
	return true;
}

/* ---- Hotkey callbacks ---- */

static void credits_hotkey_start(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct credits_source *ctx = data;
	pthread_mutex_lock(&ctx->mutex);
	ctx->scroll_offset = -(float)ctx->height;
	ctx->current_speed = 0.0f;
	ctx->delay_timer = 0.0f;
	ctx->scrolling = true;
	ctx->started = false;
	ctx->paused = false;
	ctx->waiting_loop = false;
	ctx->needs_rebuild = true;
	ctx->rebuild_warmup = -1;
	pthread_mutex_unlock(&ctx->mutex);
}

static void credits_hotkey_pause(void *data, obs_hotkey_id id,
				 obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct credits_source *ctx = data;
	pthread_mutex_lock(&ctx->mutex);
	ctx->paused = !ctx->paused;
	pthread_mutex_unlock(&ctx->mutex);
}

static void credits_hotkey_stop(void *data, obs_hotkey_id id,
				obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct credits_source *ctx = data;
	pthread_mutex_lock(&ctx->mutex);
	ctx->scrolling = false;
	ctx->started = false;
	ctx->paused = false;
	ctx->scroll_offset = -(float)ctx->height;
	ctx->current_speed = 0.0f;
	pthread_mutex_unlock(&ctx->mutex);
}

/* ---- Standard OBS source callbacks ---- */

static const char *credits_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("CreditsRoll");
}

static void *credits_create(obs_data_t *settings, obs_source_t *source)
{
	struct credits_source *ctx = bzalloc(sizeof(struct credits_source));
	ctx->self = source;
	pthread_mutex_init(&ctx->mutex, NULL);
	ctx->display_layer = 0;
	ctx->rebuild_warmup = -1;

	ctx->hotkey_start = obs_hotkey_register_source(
		source, "obs_credits.start",
		obs_module_text("StartCredits"),
		credits_hotkey_start, ctx);
	ctx->hotkey_pause = obs_hotkey_register_source(
		source, "obs_credits.pause",
		obs_module_text("PauseCredits"),
		credits_hotkey_pause, ctx);
	ctx->hotkey_stop = obs_hotkey_register_source(
		source, "obs_credits.stop",
		obs_module_text("StopCredits"),
		credits_hotkey_stop, ctx);

	/* Set initial scroll state before first update */
	ctx->scrolling = true;
	ctx->started = false;
	ctx->paused = false;

	yt_chat_init(&ctx->yt_chat);
	obs_frontend_add_event_callback(on_streaming_state_changed, ctx);

	credits_update(ctx, settings);
	return ctx;
}

static void credits_destroy(void *data)
{
	struct credits_source *ctx = data;

	pthread_mutex_lock(&ctx->mutex);
	struct credits_layout *l0 = ctx->layer[0].layout;
	struct credits_layout *l1 = ctx->layer[1].layout;
	ctx->layer[0].layout = NULL;
	ctx->layer[1].layout = NULL;
	struct credits_data *cdata = ctx->data;
	ctx->data = NULL;
	pthread_mutex_unlock(&ctx->mutex);

	credits_renderer_free(l0);
	credits_renderer_free(l1);
	credits_data_free(cdata);

	obs_enter_graphics();
	gs_texrender_destroy(ctx->layer[0].texrender);
	gs_texrender_destroy(ctx->layer[1].texrender);
	obs_leave_graphics();

	obs_hotkey_unregister(ctx->hotkey_start);
	obs_hotkey_unregister(ctx->hotkey_pause);
	obs_hotkey_unregister(ctx->hotkey_stop);

	obs_frontend_remove_event_callback(on_streaming_state_changed, ctx);
	yt_chat_destroy(&ctx->yt_chat);

	pthread_mutex_destroy(&ctx->mutex);
	bfree(ctx->default_font_face);
	for (int i = 0; i < MAX_DISCORD_SECTIONS; i++)
		bfree(ctx->discord_sections[i]);
	bfree(ctx);
}

static void credits_update(void *data, obs_data_t *settings)
{
	struct credits_source *ctx = data;

	double speed = obs_data_get_double(settings, "scroll_speed");
	bool loop_val = obs_data_get_bool(settings, "loop");
	int w = (int)obs_data_get_int(settings, "width");
	int h = (int)obs_data_get_int(settings, "height");

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	char *face = NULL;
	int size = 0;
	if (font_obj) {
		const char *face_str =
			obs_data_get_string(font_obj, "face");
		if (face_str && face_str[0] != '\0')
			face = bstrdup(face_str);
		size = (int)obs_data_get_int(font_obj, "size");
		obs_data_release(font_obj);
	}

	double start_del = obs_data_get_double(settings, "start_delay");
	double loop_del = obs_data_get_double(settings, "loop_delay");

	obs_data_array_t *arr = settings_to_sections_array(settings);

	obs_data_t *tmp = obs_data_create();
	if (arr) {
		obs_data_set_array(tmp, "sections", arr);
		obs_data_array_release(arr);
	}

	bool yt_en = obs_data_get_bool(settings, "yt_enabled");

	pthread_mutex_lock(&ctx->mutex);

	/* Free old data but keep old layout alive until video_tick
	 * builds the replacement. This prevents a 1-frame gap (flicker). */
	credits_data_free(ctx->data);
	ctx->data = NULL;
	ctx->needs_rebuild = true;

	ctx->scroll_speed = speed > 0.0 ? (float)speed : 60.0f;
	ctx->loop = loop_val;
	ctx->width = w > 0 ? (uint32_t)w : 1920;
	ctx->height = h > 0 ? (uint32_t)h : 1080;

	bfree(ctx->default_font_face);
	ctx->default_font_face = face ? face : bstrdup("Arial");
	ctx->default_font_size = size > 0 ? size : 32;

	ctx->start_delay = (float)start_del;
	ctx->loop_delay = (float)loop_del;
	ctx->delay_timer = 0.0f;
	ctx->yt_enabled = yt_en;

	/* Inject Discord sections if data is available */
	if (obs_data_get_bool(settings, "discord_enabled")) {
		int dscount = (int)obs_data_get_int(settings,
						    "discord_section_count");
		if (dscount > MAX_DISCORD_SECTIONS)
			dscount = MAX_DISCORD_SECTIONS;

		bool has_discord_data = false;
		for (int i = 0; i < dscount && !has_discord_data; i++) {
			if (ctx->discord_sections[i])
				has_discord_data = true;
		}

		if (has_discord_data) {
			obs_data_array_t *discord_arr =
				obs_data_get_array(tmp, "sections");
			if (!discord_arr)
				discord_arr = obs_data_array_create();

			char key[64];

			for (int i = 0; i < dscount; i++) {
				if (!ctx->discord_sections[i])
					continue;

				obs_data_t *sec = obs_data_create();

				/* Heading */
				snprintf(key, sizeof(key),
					 "dsection_%d_heading", i);
				const char *heading =
					obs_data_get_string(settings, key);
				if (!heading || heading[0] == '\0')
					heading = "Discord Members";
				obs_data_set_string(sec, "heading", heading);

				/* Names from fetched data */
				obs_data_set_string(sec, "names",
						    ctx->discord_sections[i]);

				/* Alignment */
				snprintf(key, sizeof(key),
					 "dsection_%d_alignment", i);
				obs_data_set_string(
					sec, "alignment",
					obs_data_get_string(settings, key));

				/* Font pickers */
				const char *ds_font_fields[] = {
					"heading_font", "entry_font"};
				for (int f = 0; f < 2; f++) {
					snprintf(key, sizeof(key),
						 "dsection_%d_%s", i,
						 ds_font_fields[f]);
					obs_data_t *fobj =
						obs_data_get_obj(settings, key);
					if (fobj) {
						obs_data_set_obj(
							sec,
							ds_font_fields[f],
							fobj);
						obs_data_release(fobj);
					}
				}

				/* Colors */
				snprintf(key, sizeof(key),
					 "dsection_%d_heading_color", i);
				obs_data_set_int(sec, "heading_color",
						 obs_data_get_int(settings,
								  key));

				snprintf(key, sizeof(key),
					 "dsection_%d_text_color", i);
				obs_data_set_int(sec, "text_color",
						 obs_data_get_int(settings,
								  key));

				/* Outline */
				snprintf(key, sizeof(key),
					 "dsection_%d_outline_enabled", i);
				obs_data_set_bool(
					sec, "outline_enabled",
					obs_data_get_bool(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_outline_size", i);
				obs_data_set_int(sec, "outline_size",
						 obs_data_get_int(settings,
								  key));

				snprintf(key, sizeof(key),
					 "dsection_%d_outline_color", i);
				obs_data_set_int(sec, "outline_color",
						 obs_data_get_int(settings,
								  key));

				snprintf(key, sizeof(key),
					 "dsection_%d_outline_heading", i);
				obs_data_set_bool(
					sec, "outline_heading",
					obs_data_get_bool(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_outline_sub", i);
				obs_data_set_bool(
					sec, "outline_sub",
					obs_data_get_bool(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_outline_entries", i);
				obs_data_set_bool(
					sec, "outline_entries",
					obs_data_get_bool(settings, key));

				/* Shadow */
				snprintf(key, sizeof(key),
					 "dsection_%d_shadow_enabled", i);
				obs_data_set_bool(
					sec, "shadow_enabled",
					obs_data_get_bool(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_shadow_color", i);
				obs_data_set_int(sec, "shadow_color",
						 obs_data_get_int(settings,
								  key));

				snprintf(key, sizeof(key),
					 "dsection_%d_shadow_offset_x", i);
				obs_data_set_double(
					sec, "shadow_offset_x",
					obs_data_get_double(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_shadow_offset_y", i);
				obs_data_set_double(
					sec, "shadow_offset_y",
					obs_data_get_double(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_shadow_heading", i);
				obs_data_set_bool(
					sec, "shadow_heading",
					obs_data_get_bool(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_shadow_sub", i);
				obs_data_set_bool(
					sec, "shadow_sub",
					obs_data_get_bool(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_shadow_entries", i);
				obs_data_set_bool(
					sec, "shadow_entries",
					obs_data_get_bool(settings, key));

				/* Spacing */
				snprintf(key, sizeof(key),
					 "dsection_%d_heading_spacing", i);
				obs_data_set_double(
					sec, "heading_spacing",
					obs_data_get_double(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_entry_spacing", i);
				obs_data_set_double(
					sec, "entry_spacing",
					obs_data_get_double(settings, key));

				snprintf(key, sizeof(key),
					 "dsection_%d_section_spacing", i);
				obs_data_set_double(
					sec, "section_spacing",
					obs_data_get_double(settings, key));

				obs_data_array_push_back(discord_arr, sec);
				obs_data_release(sec);
			}

			obs_data_set_array(tmp, "sections", discord_arr);
			obs_data_array_release(discord_arr);
		}
	}

	/* Inject YouTube chat section if enabled and names are available */
	if (yt_en) {
		char *yt_names = yt_chat_get_names(&ctx->yt_chat);
		if (yt_names) {
			obs_data_array_t *yt_arr =
				obs_data_get_array(tmp, "sections");
			if (!yt_arr)
				yt_arr = obs_data_array_create();

			obs_data_t *sec = obs_data_create();

			/* Heading */
			const char *yt_heading =
				obs_data_get_string(settings, "yt_heading");
			if (!yt_heading || yt_heading[0] == '\0')
				yt_heading = "YouTube Chat";
			obs_data_set_string(sec, "heading", yt_heading);

			/* Names from live chat */
			obs_data_set_string(sec, "names", yt_names);
			bfree(yt_names);

			/* Alignment */
			obs_data_set_string(
				sec, "alignment",
				obs_data_get_string(settings, "yt_alignment"));

			/* Font pickers */
			obs_data_t *yt_hfont =
				obs_data_get_obj(settings, "yt_heading_font");
			if (yt_hfont) {
				obs_data_set_obj(sec, "heading_font", yt_hfont);
				obs_data_release(yt_hfont);
			}
			obs_data_t *yt_efont =
				obs_data_get_obj(settings, "yt_entry_font");
			if (yt_efont) {
				obs_data_set_obj(sec, "entry_font", yt_efont);
				obs_data_release(yt_efont);
			}

			/* Colors */
			obs_data_set_int(
				sec, "heading_color",
				obs_data_get_int(settings, "yt_heading_color"));
			obs_data_set_int(
				sec, "text_color",
				obs_data_get_int(settings, "yt_text_color"));

			/* Outline */
			obs_data_set_bool(
				sec, "outline_enabled",
				obs_data_get_bool(settings,
						  "yt_outline_enabled"));
			obs_data_set_int(
				sec, "outline_size",
				obs_data_get_int(settings, "yt_outline_size"));
			obs_data_set_int(
				sec, "outline_color",
				obs_data_get_int(settings, "yt_outline_color"));
			obs_data_set_bool(
				sec, "outline_heading",
				obs_data_get_bool(settings,
						  "yt_outline_heading"));
			obs_data_set_bool(
				sec, "outline_entries",
				obs_data_get_bool(settings,
						  "yt_outline_entries"));

			/* Shadow */
			obs_data_set_bool(
				sec, "shadow_enabled",
				obs_data_get_bool(settings,
						  "yt_shadow_enabled"));
			obs_data_set_int(
				sec, "shadow_color",
				obs_data_get_int(settings, "yt_shadow_color"));
			obs_data_set_double(
				sec, "shadow_offset_x",
				obs_data_get_double(settings,
						    "yt_shadow_offset_x"));
			obs_data_set_double(
				sec, "shadow_offset_y",
				obs_data_get_double(settings,
						    "yt_shadow_offset_y"));
			obs_data_set_bool(
				sec, "shadow_heading",
				obs_data_get_bool(settings,
						  "yt_shadow_heading"));
			obs_data_set_bool(
				sec, "shadow_entries",
				obs_data_get_bool(settings,
						  "yt_shadow_entries"));

			/* Spacing */
			obs_data_set_double(
				sec, "heading_spacing",
				obs_data_get_double(settings,
						    "yt_heading_spacing"));
			obs_data_set_double(
				sec, "entry_spacing",
				obs_data_get_double(settings,
						    "yt_entry_spacing"));
			obs_data_set_double(
				sec, "section_spacing",
				obs_data_get_double(settings,
						    "yt_section_spacing"));

			obs_data_array_push_back(yt_arr, sec);
			obs_data_release(sec);

			obs_data_set_array(tmp, "sections", yt_arr);
			obs_data_array_release(yt_arr);
		}
	}

	ctx->data = credits_build_from_settings(tmp);

	/* Preserve scroll position on settings changes so edits
	 * are visible live. Layout rebuilds on next video_tick.
	 * Scroll only resets via hotkey or first creation. */

	pthread_mutex_unlock(&ctx->mutex);

	obs_data_release(tmp);
}

static void credits_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "scroll_speed", 60.0);
	obs_data_set_default_bool(settings, "loop", false);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);

	obs_data_set_default_int(settings, "section_count", 1);
	obs_data_set_default_string(settings, "section_0_alignment", "center");

	/* Per-section defaults for section 0 */
	obs_data_set_default_int(settings, "section_0_heading_color",
				 (long long)0x00FFD700);
	obs_data_set_default_int(settings, "section_0_sub_color",
				 (long long)0x00CCCCCC);
	obs_data_set_default_int(settings, "section_0_text_color",
				 (long long)0x00FFFFFF);

	obs_data_set_default_bool(settings, "section_0_outline_enabled", false);
	obs_data_set_default_int(settings, "section_0_outline_size", 2);
	obs_data_set_default_int(settings, "section_0_outline_color",
				 (long long)0x00000000);
	obs_data_set_default_bool(settings, "section_0_outline_heading", true);
	obs_data_set_default_bool(settings, "section_0_outline_sub", true);
	obs_data_set_default_bool(settings, "section_0_outline_entries", true);

	obs_data_set_default_bool(settings, "section_0_shadow_enabled", false);
	obs_data_set_default_int(settings, "section_0_shadow_color",
				 (long long)0x00333333);
	obs_data_set_default_double(settings, "section_0_shadow_offset_x", 2.0);
	obs_data_set_default_double(settings, "section_0_shadow_offset_y", 2.0);
	obs_data_set_default_bool(settings, "section_0_shadow_heading", true);
	obs_data_set_default_bool(settings, "section_0_shadow_sub", true);
	obs_data_set_default_bool(settings, "section_0_shadow_entries", true);

	obs_data_set_default_double(settings, "section_0_heading_spacing", 0.0);
	obs_data_set_default_double(settings, "section_0_sub_spacing", 0.0);
	obs_data_set_default_double(settings, "section_0_entry_spacing", 0.0);
	obs_data_set_default_double(settings, "section_0_section_spacing", 0.0);

	obs_data_set_default_double(settings, "start_delay", 0.0);
	obs_data_set_default_double(settings, "loop_delay", 0.0);

	obs_data_set_default_bool(settings, "discord_enabled", false);
	obs_data_set_default_int(settings, "discord_section_count", 0);

	/* YouTube chat defaults */
	obs_data_set_default_bool(settings, "yt_enabled", false);
	obs_data_set_default_bool(settings, "yt_expand", true);
	obs_data_set_default_string(settings, "yt_channel_url", "");
	obs_data_set_default_string(settings, "yt_heading", "YouTube Chat");
	obs_data_set_default_string(settings, "yt_alignment", "center");
	obs_data_set_default_int(settings, "yt_heading_color",
				 (long long)0x00FF0000);
	obs_data_set_default_int(settings, "yt_text_color",
				 (long long)0x00FFFFFF);
	obs_data_set_default_bool(settings, "yt_outline_enabled", false);
	obs_data_set_default_int(settings, "yt_outline_size", 2);
	obs_data_set_default_int(settings, "yt_outline_color",
				 (long long)0x00000000);
	obs_data_set_default_bool(settings, "yt_outline_heading", true);
	obs_data_set_default_bool(settings, "yt_outline_entries", true);
	obs_data_set_default_bool(settings, "yt_shadow_enabled", false);
	obs_data_set_default_int(settings, "yt_shadow_color",
				 (long long)0x00333333);
	obs_data_set_default_double(settings, "yt_shadow_offset_x", 2.0);
	obs_data_set_default_double(settings, "yt_shadow_offset_y", 2.0);
	obs_data_set_default_bool(settings, "yt_shadow_heading", true);
	obs_data_set_default_bool(settings, "yt_shadow_entries", true);
	obs_data_set_default_double(settings, "yt_heading_spacing", 0.0);
	obs_data_set_default_double(settings, "yt_entry_spacing", 0.0);
	obs_data_set_default_double(settings, "yt_section_spacing", 0.0);

	/* Default all sections to expanded */
	for (int i = 0; i < MAX_SECTIONS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "section_%d_expand", i);
		obs_data_set_default_bool(settings, key, true);
	}

	/* Default all discord sections to expanded */
	for (int i = 0; i < MAX_DISCORD_SECTIONS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "dsection_%d_expand", i);
		obs_data_set_default_bool(settings, key, true);
	}

	/* Set default font objects for first section so the font picker
	 * dialog starts at a reasonable size instead of tiny system default */
	const char *def_font_keys[] = {"section_0_heading_font",
				       "section_0_sub_font",
				       "section_0_entry_font"};
	int def_sizes[] = {72, 36, 36};
	for (int i = 0; i < 3; i++) {
		obs_data_t *fobj = obs_data_create();
		obs_data_set_string(fobj, "face", "Arial");
		obs_data_set_int(fobj, "size", def_sizes[i]);
		obs_data_set_int(fobj, "flags", 0);
		obs_data_set_string(fobj, "style", "Regular");
		obs_data_set_default_obj(settings, def_font_keys[i], fobj);
		obs_data_release(fobj);
	}
}

static void credits_video_tick(void *data, float seconds)
{
	struct credits_source *ctx = data;

	if (seconds <= 0.0f || seconds > 1.0f)
		seconds = 0.016f;

	pthread_mutex_lock(&ctx->mutex);

	if (ctx->data && (ctx->needs_rebuild || !ctx->layer[ctx->display_layer].layout)) {
		bool first_build = !ctx->layer[0].layout && !ctx->layer[1].layout;

		if (first_build) {
			/* First build: put directly into displayed layer
			 * so content shows immediately */
			ctx->layer[ctx->display_layer].layout =
				credits_renderer_build(
					ctx->data, ctx->width,
					ctx->default_font_face,
					ctx->default_font_size);
			ctx->rebuild_warmup = -1;
		} else {
			/* Subsequent rebuilds: build into non-displayed layer,
			 * warm up for 3 frames, then swap */
			int target = 1 - ctx->display_layer;
			credits_renderer_free(ctx->layer[target].layout);
			ctx->layer[target].layout =
				credits_renderer_build(
					ctx->data, ctx->width,
					ctx->default_font_face,
					ctx->default_font_size);
			ctx->rebuild_warmup = 0;
		}
		ctx->needs_rebuild = false;
	}

	/* Count warmup frames and swap when ready */
	if (ctx->rebuild_warmup >= 0 && ctx->rebuild_warmup < 3) {
		ctx->rebuild_warmup++;
		if (ctx->rebuild_warmup >= 3) {
			int target = 1 - ctx->display_layer;
			if (ctx->layer[target].layout) {
				ctx->display_layer = target;
				/* Free the old displayed layer now that we swapped */
				int old = 1 - ctx->display_layer;
				credits_renderer_free(ctx->layer[old].layout);
				ctx->layer[old].layout = NULL;
			}
			ctx->rebuild_warmup = -1;
		}
	}

	/* Update total_height from the current layout */
	if (ctx->layer[ctx->display_layer].layout)
		ctx->total_height = credits_renderer_total_height(
			ctx->layer[ctx->display_layer].layout);

	if (ctx->scrolling && ctx->layer[ctx->display_layer].layout) {
		if (!ctx->started) {
			ctx->scroll_offset = -(float)ctx->height;
			ctx->delay_timer = 0.0f;
			ctx->started = true;
		}

		if (!ctx->paused) {
			bool do_scroll = true;

			/* Start delay */
			if (ctx->start_delay > 0.0f &&
			    ctx->delay_timer < ctx->start_delay) {
				ctx->delay_timer += seconds;
				do_scroll = false;
			}

			/* Loop delay */
			if (do_scroll && ctx->waiting_loop) {
				ctx->delay_timer += seconds;
				if (ctx->delay_timer >= ctx->loop_delay) {
					ctx->waiting_loop = false;
					ctx->scroll_offset =
						-(float)ctx->height;
					ctx->current_speed = 0.0f;
					ctx->delay_timer = ctx->start_delay;
				}
				do_scroll = false;
			}

			if (do_scroll) {
				ctx->current_speed +=
					(ctx->scroll_speed -
					 ctx->current_speed) *
					5.0f * seconds;
				ctx->scroll_offset +=
					ctx->current_speed * seconds;

				if (ctx->scroll_offset > ctx->total_height) {
					if (ctx->loop) {
						if (ctx->loop_delay > 0.0f) {
							ctx->waiting_loop =
								true;
							ctx->delay_timer =
								0.0f;
						} else {
							ctx->scroll_offset =
								-(float)ctx
									 ->height;
							ctx->current_speed =
								0.0f;
						}
					} else {
						ctx->scrolling = false;
					}
				}
			}
		}
	}

	pthread_mutex_unlock(&ctx->mutex);

	/* Poll YouTube chat count - MUST be after mutex unlock and
	 * MUST NOT be blocked by early returns from scroll logic. */
	ctx->yt_poll_timer += seconds;
	if (ctx->yt_poll_timer >= 3.0f) {
		ctx->yt_poll_timer = 0.0f;

		pthread_mutex_lock(&ctx->mutex);
		bool yt_en = ctx->yt_enabled;
		pthread_mutex_unlock(&ctx->mutex);

		if (yt_en) {
			size_t cur_count = yt_chat_get_count(&ctx->yt_chat);
			bool changed = false;

			pthread_mutex_lock(&ctx->mutex);
			if (cur_count != ctx->yt_last_count) {
				ctx->yt_last_count = cur_count;
				changed = true;
			}
			pthread_mutex_unlock(&ctx->mutex);

			if (changed) {
				obs_data_t *s =
					obs_source_get_settings(ctx->self);
				obs_source_update(ctx->self, s);
				obs_data_release(s);
			}
		}
	}
}

static void credits_video_render(void *data, gs_effect_t *effect)
{
	struct credits_source *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	if (!ctx->layer[ctx->display_layer].layout || ctx->width == 0 || ctx->height == 0) {
		pthread_mutex_unlock(&ctx->mutex);
		UNUSED_PARAMETER(effect);
		return;
	}

	/* Render BOTH layers so text sources in both stay initialized */
	for (int i = 0; i < 2; i++) {
		if (!ctx->layer[i].layout)
			continue;
		if (!ctx->layer[i].texrender)
			ctx->layer[i].texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

		gs_texrender_reset(ctx->layer[i].texrender);
		if (gs_texrender_begin(ctx->layer[i].texrender, ctx->width, ctx->height)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, (float)ctx->width, ctx->scroll_offset,
				 ctx->scroll_offset + (float)ctx->height, -1.0f, 1.0f);
			credits_renderer_draw(ctx->layer[i].layout, ctx->width,
					      ctx->scroll_offset, (float)ctx->height);
			gs_texrender_end(ctx->layer[i].texrender);
		}
	}

	/* Display ONLY the active layer */
	gs_texture_t *tex = gs_texrender_get_texture(ctx->layer[ctx->display_layer].texrender);
	if (tex) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *param = gs_effect_get_param_by_name(def, "image");
		gs_effect_set_texture(param, tex);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(tex, 0, ctx->width, ctx->height);
	}

	pthread_mutex_unlock(&ctx->mutex);

	UNUSED_PARAMETER(effect);
}

static uint32_t credits_get_width(void *data)
{
	struct credits_source *ctx = data;
	return ctx->width;
}

static uint32_t credits_get_height(void *data)
{
	struct credits_source *ctx = data;
	return ctx->height;
}

static obs_properties_t *credits_get_properties(void *data)
{
	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	obs_properties_t *props = obs_properties_create();

	int count = (int)obs_data_get_int(settings, "section_count");
	if (count < 1)
		count = 1;

	/* Pre-create all section groups up to MAX_SECTIONS.
	 * Only the first `count` are visible. Add/remove toggles visibility
	 * without rebuilding the panel (which would crash Qt). */
	for (int i = 0; i < MAX_SECTIONS; i++) {
		char group_name[64], expand_name[64];
		char heading_name[64], sub_name[64];
		char names_name[64], roles_name[64], align_name[64];
		char remove_name[64], label[128];
		char hfont_name[64], sfont_name[64], efont_name[64];
		char hcolor_name[64], scolor_name[64], tcolor_name[64];
		char ol_en_name[64], ol_sz_name[64], ol_col_name[64];
		char ol_h_name[64], ol_s_name[64], ol_e_name[64];
		char sh_en_name[64], sh_col_name[64];
		char sh_ox_name[64], sh_oy_name[64];
		char sh_h_name[64], sh_s_name[64], sh_e_name[64];
		char hsp_name[64], ssp_name[64], esp_name[64], secsp_name[64];

		snprintf(group_name, sizeof(group_name), "section_%d_group",
			 i);
		snprintf(expand_name, sizeof(expand_name),
			 "section_%d_expand", i);
		snprintf(heading_name, sizeof(heading_name),
			 "section_%d_heading", i);
		snprintf(hfont_name, sizeof(hfont_name),
			 "section_%d_heading_font", i);
		snprintf(sub_name, sizeof(sub_name), "section_%d_subheading",
			 i);
		snprintf(sfont_name, sizeof(sfont_name),
			 "section_%d_sub_font", i);
		snprintf(names_name, sizeof(names_name), "section_%d_names",
			 i);
		snprintf(roles_name, sizeof(roles_name), "section_%d_roles",
			 i);
		snprintf(efont_name, sizeof(efont_name),
			 "section_%d_entry_font", i);
		snprintf(align_name, sizeof(align_name),
			 "section_%d_alignment", i);
		snprintf(remove_name, sizeof(remove_name),
			 "section_%d_remove", i);

		snprintf(hcolor_name, sizeof(hcolor_name),
			 "section_%d_heading_color", i);
		snprintf(scolor_name, sizeof(scolor_name),
			 "section_%d_sub_color", i);
		snprintf(tcolor_name, sizeof(tcolor_name),
			 "section_%d_text_color", i);

		snprintf(ol_en_name, sizeof(ol_en_name),
			 "section_%d_outline_enabled", i);
		snprintf(ol_sz_name, sizeof(ol_sz_name),
			 "section_%d_outline_size", i);
		snprintf(ol_col_name, sizeof(ol_col_name),
			 "section_%d_outline_color", i);
		snprintf(ol_h_name, sizeof(ol_h_name),
			 "section_%d_outline_heading", i);
		snprintf(ol_s_name, sizeof(ol_s_name),
			 "section_%d_outline_sub", i);
		snprintf(ol_e_name, sizeof(ol_e_name),
			 "section_%d_outline_entries", i);

		snprintf(sh_en_name, sizeof(sh_en_name),
			 "section_%d_shadow_enabled", i);
		snprintf(sh_col_name, sizeof(sh_col_name),
			 "section_%d_shadow_color", i);
		snprintf(sh_ox_name, sizeof(sh_ox_name),
			 "section_%d_shadow_offset_x", i);
		snprintf(sh_oy_name, sizeof(sh_oy_name),
			 "section_%d_shadow_offset_y", i);
		snprintf(sh_h_name, sizeof(sh_h_name),
			 "section_%d_shadow_heading", i);
		snprintf(sh_s_name, sizeof(sh_s_name),
			 "section_%d_shadow_sub", i);
		snprintf(sh_e_name, sizeof(sh_e_name),
			 "section_%d_shadow_entries", i);

		snprintf(hsp_name, sizeof(hsp_name),
			 "section_%d_heading_spacing", i);
		snprintf(ssp_name, sizeof(ssp_name),
			 "section_%d_sub_spacing", i);
		snprintf(esp_name, sizeof(esp_name),
			 "section_%d_entry_spacing", i);
		snprintf(secsp_name, sizeof(secsp_name),
			 "section_%d_section_spacing", i);

		/* Use heading text as label if available */
		const char *h = obs_data_get_string(settings, heading_name);
		if (h && h[0] != '\0')
			snprintf(label, sizeof(label), "%s", h);
		else
			snprintf(label, sizeof(label), "%s %d",
				 obs_module_text("Section"), i + 1);

		obs_properties_t *group = obs_properties_create();

		/* Expand toggle - controls visibility of all fields below */
		obs_property_t *exp = obs_properties_add_bool(
			group, expand_name,
			obs_module_text("ExpandSection"));
		obs_property_set_modified_callback2(
			exp, on_section_expand_toggled, ctx);

		obs_properties_add_text(group, heading_name,
					obs_module_text("Heading"),
					OBS_TEXT_DEFAULT);
		obs_properties_add_font(group, hfont_name,
					obs_module_text("HeadingFont"));

		obs_properties_add_text(group, sub_name,
					obs_module_text("Subheading"),
					OBS_TEXT_DEFAULT);
		obs_properties_add_font(group, sfont_name,
					obs_module_text("SubFont"));

		obs_properties_add_text(group, names_name,
					obs_module_text("Names"),
					OBS_TEXT_MULTILINE);
		obs_properties_add_text(group, roles_name,
					obs_module_text("Roles"),
					OBS_TEXT_MULTILINE);
		obs_properties_add_font(group, efont_name,
					obs_module_text("EntryFont"));

		/* Alignment */
		obs_property_t *align_prop = obs_properties_add_list(
			group, align_name, obs_module_text("Alignment"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignLeft"), "left");
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignCenter"), "center");
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignRight"), "right");

		/* Per-section colors */
		obs_properties_add_color(group, hcolor_name,
					 obs_module_text("SectionHeadingColor"));
		obs_properties_add_color(group, scolor_name,
					 obs_module_text("SectionSubColor"));
		obs_properties_add_color(group, tcolor_name,
					 obs_module_text("SectionTextColor"));

		/* Per-section outline */
		obs_property_t *ol_cb = obs_properties_add_bool(
			group, ol_en_name,
			obs_module_text("SectionOutlineEnabled"));
		obs_property_set_modified_callback2(
			ol_cb, on_section_outline_toggled, ctx);
		obs_properties_add_int(group, ol_sz_name,
				       obs_module_text("SectionOutlineSize"),
				       1, 20, 1);
		obs_properties_add_color(group, ol_col_name,
					 obs_module_text("SectionOutlineColor"));
		obs_properties_add_bool(group, ol_h_name,
					obs_module_text("OutlineApplyHeading"));
		obs_properties_add_bool(group, ol_s_name,
					obs_module_text("OutlineApplySub"));
		obs_properties_add_bool(group, ol_e_name,
					obs_module_text("OutlineApplyEntries"));

		/* Per-section shadow */
		obs_property_t *sh_cb = obs_properties_add_bool(
			group, sh_en_name,
			obs_module_text("SectionShadowEnabled"));
		obs_property_set_modified_callback2(
			sh_cb, on_section_shadow_toggled, ctx);
		obs_properties_add_color(group, sh_col_name,
					 obs_module_text("SectionShadowColor"));
		obs_properties_add_float(group, sh_ox_name,
					 obs_module_text("SectionShadowOffsetX"),
					 -20.0, 20.0, 0.5);
		obs_properties_add_float(group, sh_oy_name,
					 obs_module_text("SectionShadowOffsetY"),
					 -20.0, 20.0, 0.5);
		obs_properties_add_bool(group, sh_h_name,
					obs_module_text("ShadowApplyHeading"));
		obs_properties_add_bool(group, sh_s_name,
					obs_module_text("ShadowApplySub"));
		obs_properties_add_bool(group, sh_e_name,
					obs_module_text("ShadowApplyEntries"));

		/* Per-section spacing */
		obs_properties_add_float(group, hsp_name,
					 obs_module_text("SectionHeadingSpacing"),
					 -9999.0, 9999.0, 1.0);
		obs_properties_add_float(group, ssp_name,
					 obs_module_text("SectionSubSpacing"),
					 -9999.0, 9999.0, 1.0);
		obs_properties_add_float(group, esp_name,
					 obs_module_text("SectionEntrySpacing"),
					 -9999.0, 9999.0, 1.0);
		obs_properties_add_float(group, secsp_name,
					 obs_module_text("SectionSectionSpacing"),
					 -9999.0, 9999.0, 1.0);

		/* Remove button */
		obs_property_t *rm = obs_properties_add_button2(
			group, remove_name,
			obs_module_text("RemoveSection"),
			on_remove_section, ctx);
		obs_property_set_visible(rm, i < count && count > 1);

		/* Set initial visibility based on expand state */
		bool expanded = obs_data_get_bool(settings, expand_name);
		obs_property_set_visible(
			obs_properties_get(group, heading_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, hfont_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, sub_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, sfont_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, names_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, roles_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, efont_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, align_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, hcolor_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, scolor_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, tcolor_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, ol_en_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, sh_en_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, hsp_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, ssp_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, esp_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, secsp_name), expanded);

		/* Outline sub-properties: visible only if expanded AND outline enabled */
		bool ol_on = obs_data_get_bool(settings, ol_en_name);
		obs_property_set_visible(
			obs_properties_get(group, ol_sz_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_col_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_h_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_s_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_e_name),
			expanded && ol_on);

		/* Shadow sub-properties: visible only if expanded AND shadow enabled */
		bool sh_on = obs_data_get_bool(settings, sh_en_name);
		obs_property_set_visible(
			obs_properties_get(group, sh_col_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_ox_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_oy_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_h_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_s_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_e_name),
			expanded && sh_on);

		obs_property_t *gp = obs_properties_add_group(
			props, group_name, label, OBS_GROUP_NORMAL, group);
		obs_property_set_visible(gp, i < count);
	}

	obs_properties_add_button2(props, "add_section",
				   obs_module_text("AddSection"),
				   on_add_section, ctx);

	/* Discord integration */
	obs_property_t *discord_cb = obs_properties_add_bool(
		props, "discord_enabled",
		obs_module_text("DiscordEnabled"));
	obs_property_set_modified_callback2(discord_cb, on_discord_toggled,
					    ctx);

	bool d_on = obs_data_get_bool(settings, "discord_enabled");

	obs_property_t *d_token = obs_properties_add_text(
		props, "discord_token", obs_module_text("DiscordToken"),
		OBS_TEXT_PASSWORD);
	obs_property_set_visible(d_token, d_on);

	obs_property_t *d_guild = obs_properties_add_text(
		props, "discord_guild_id",
		obs_module_text("DiscordGuildID"), OBS_TEXT_DEFAULT);
	obs_property_set_visible(d_guild, d_on);

	/* Discord sections - pre-create all up to MAX_DISCORD_SECTIONS */
	int dscount =
		(int)obs_data_get_int(settings, "discord_section_count");

	for (int i = 0; i < MAX_DISCORD_SECTIONS; i++) {
		char group_name[64], expand_name[64];
		char role_id_name[64];
		char heading_name[64], align_name[64];
		char hfont_name[64], efont_name[64];
		char hcolor_name[64], tcolor_name[64];
		char ol_en_name[64], ol_sz_name[64], ol_col_name[64];
		char ol_h_name[64], ol_s_name[64], ol_e_name[64];
		char sh_en_name[64], sh_col_name[64];
		char sh_ox_name[64], sh_oy_name[64];
		char sh_h_name[64], sh_s_name[64], sh_e_name[64];
		char hsp_name[64], esp_name[64], secsp_name[64];
		char remove_name[64], label[128];

		snprintf(group_name, sizeof(group_name), "dsection_%d_group",
			 i);
		snprintf(expand_name, sizeof(expand_name),
			 "dsection_%d_expand", i);
		snprintf(role_id_name, sizeof(role_id_name),
			 "dsection_%d_role_id", i);
		snprintf(heading_name, sizeof(heading_name),
			 "dsection_%d_heading", i);
		snprintf(hfont_name, sizeof(hfont_name),
			 "dsection_%d_heading_font", i);
		snprintf(efont_name, sizeof(efont_name),
			 "dsection_%d_entry_font", i);
		snprintf(align_name, sizeof(align_name),
			 "dsection_%d_alignment", i);
		snprintf(hcolor_name, sizeof(hcolor_name),
			 "dsection_%d_heading_color", i);
		snprintf(tcolor_name, sizeof(tcolor_name),
			 "dsection_%d_text_color", i);
		snprintf(ol_en_name, sizeof(ol_en_name),
			 "dsection_%d_outline_enabled", i);
		snprintf(ol_sz_name, sizeof(ol_sz_name),
			 "dsection_%d_outline_size", i);
		snprintf(ol_col_name, sizeof(ol_col_name),
			 "dsection_%d_outline_color", i);
		snprintf(ol_h_name, sizeof(ol_h_name),
			 "dsection_%d_outline_heading", i);
		snprintf(ol_s_name, sizeof(ol_s_name),
			 "dsection_%d_outline_sub", i);
		snprintf(ol_e_name, sizeof(ol_e_name),
			 "dsection_%d_outline_entries", i);
		snprintf(sh_en_name, sizeof(sh_en_name),
			 "dsection_%d_shadow_enabled", i);
		snprintf(sh_col_name, sizeof(sh_col_name),
			 "dsection_%d_shadow_color", i);
		snprintf(sh_ox_name, sizeof(sh_ox_name),
			 "dsection_%d_shadow_offset_x", i);
		snprintf(sh_oy_name, sizeof(sh_oy_name),
			 "dsection_%d_shadow_offset_y", i);
		snprintf(sh_h_name, sizeof(sh_h_name),
			 "dsection_%d_shadow_heading", i);
		snprintf(sh_s_name, sizeof(sh_s_name),
			 "dsection_%d_shadow_sub", i);
		snprintf(sh_e_name, sizeof(sh_e_name),
			 "dsection_%d_shadow_entries", i);
		snprintf(hsp_name, sizeof(hsp_name),
			 "dsection_%d_heading_spacing", i);
		snprintf(esp_name, sizeof(esp_name),
			 "dsection_%d_entry_spacing", i);
		snprintf(secsp_name, sizeof(secsp_name),
			 "dsection_%d_section_spacing", i);
		snprintf(remove_name, sizeof(remove_name),
			 "dsection_%d_remove", i);

		/* Use heading text as label if available */
		const char *dh = obs_data_get_string(settings, heading_name);
		if (dh && dh[0] != '\0')
			snprintf(label, sizeof(label), "%s", dh);
		else
			snprintf(label, sizeof(label), "%s %d",
				 obs_module_text("DiscordSection"), i + 1);

		obs_properties_t *group = obs_properties_create();

		/* Expand toggle */
		obs_property_t *exp = obs_properties_add_bool(
			group, expand_name,
			obs_module_text("ExpandSection"));
		obs_property_set_modified_callback2(
			exp, on_discord_section_expand_toggled, ctx);

		/* Role ID */
		obs_properties_add_text(group, role_id_name,
					obs_module_text("DiscordRoleID"),
					OBS_TEXT_DEFAULT);

		/* Heading */
		obs_properties_add_text(group, heading_name,
					obs_module_text("Heading"),
					OBS_TEXT_DEFAULT);

		/* Heading font */
		obs_properties_add_font(group, hfont_name,
					obs_module_text("HeadingFont"));

		/* Entry font (for fetched names) */
		obs_properties_add_font(group, efont_name,
					obs_module_text("EntryFont"));

		/* Alignment */
		obs_property_t *align_prop = obs_properties_add_list(
			group, align_name, obs_module_text("Alignment"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignLeft"), "left");
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignCenter"), "center");
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignRight"), "right");

		/* Colors */
		obs_properties_add_color(group, hcolor_name,
					 obs_module_text("SectionHeadingColor"));
		obs_properties_add_color(group, tcolor_name,
					 obs_module_text("SectionTextColor"));

		/* Outline */
		obs_property_t *ol_cb = obs_properties_add_bool(
			group, ol_en_name,
			obs_module_text("SectionOutlineEnabled"));
		obs_property_set_modified_callback2(
			ol_cb, on_dsection_outline_toggled, ctx);
		obs_properties_add_int(group, ol_sz_name,
				       obs_module_text("SectionOutlineSize"),
				       1, 20, 1);
		obs_properties_add_color(group, ol_col_name,
					 obs_module_text("SectionOutlineColor"));
		obs_properties_add_bool(group, ol_h_name,
					obs_module_text("OutlineApplyHeading"));
		obs_properties_add_bool(group, ol_s_name,
					obs_module_text("OutlineApplySub"));
		obs_properties_add_bool(group, ol_e_name,
					obs_module_text("OutlineApplyEntries"));

		/* Shadow */
		obs_property_t *sh_cb = obs_properties_add_bool(
			group, sh_en_name,
			obs_module_text("SectionShadowEnabled"));
		obs_property_set_modified_callback2(
			sh_cb, on_dsection_shadow_toggled, ctx);
		obs_properties_add_color(group, sh_col_name,
					 obs_module_text("SectionShadowColor"));
		obs_properties_add_float(group, sh_ox_name,
					 obs_module_text("SectionShadowOffsetX"),
					 -20.0, 20.0, 0.5);
		obs_properties_add_float(group, sh_oy_name,
					 obs_module_text("SectionShadowOffsetY"),
					 -20.0, 20.0, 0.5);
		obs_properties_add_bool(group, sh_h_name,
					obs_module_text("ShadowApplyHeading"));
		obs_properties_add_bool(group, sh_s_name,
					obs_module_text("ShadowApplySub"));
		obs_properties_add_bool(group, sh_e_name,
					obs_module_text("ShadowApplyEntries"));

		/* Spacing */
		obs_properties_add_float(group, hsp_name,
					 obs_module_text("SectionHeadingSpacing"),
					 -9999.0, 9999.0, 1.0);
		obs_properties_add_float(group, esp_name,
					 obs_module_text("SectionEntrySpacing"),
					 -9999.0, 9999.0, 1.0);
		obs_properties_add_float(group, secsp_name,
					 obs_module_text("SectionSectionSpacing"),
					 -9999.0, 9999.0, 1.0);

		/* Remove button */
		obs_property_t *rm = obs_properties_add_button2(
			group, remove_name,
			obs_module_text("RemoveDiscordSection"),
			on_remove_discord_section, ctx);
		obs_property_set_visible(rm, i < dscount);

		/* Set initial visibility based on expand state */
		bool expanded = obs_data_get_bool(settings, expand_name);

		obs_property_set_visible(
			obs_properties_get(group, role_id_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, heading_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, hfont_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, efont_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, align_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, hcolor_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, tcolor_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, ol_en_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, sh_en_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, hsp_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, esp_name), expanded);
		obs_property_set_visible(
			obs_properties_get(group, secsp_name), expanded);

		/* Outline sub-properties */
		bool ol_on = obs_data_get_bool(settings, ol_en_name);
		obs_property_set_visible(
			obs_properties_get(group, ol_sz_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_col_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_h_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_s_name),
			expanded && ol_on);
		obs_property_set_visible(
			obs_properties_get(group, ol_e_name),
			expanded && ol_on);

		/* Shadow sub-properties */
		bool sh_on = obs_data_get_bool(settings, sh_en_name);
		obs_property_set_visible(
			obs_properties_get(group, sh_col_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_ox_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_oy_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_h_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_s_name),
			expanded && sh_on);
		obs_property_set_visible(
			obs_properties_get(group, sh_e_name),
			expanded && sh_on);

		obs_property_t *gp = obs_properties_add_group(
			props, group_name, label, OBS_GROUP_NORMAL, group);
		obs_property_set_visible(gp, d_on && i < dscount);
	}

	obs_property_t *add_ds = obs_properties_add_button2(
		props, "add_discord_section",
		obs_module_text("AddDiscordSection"),
		on_add_discord_section, ctx);
	obs_property_set_visible(add_ds, d_on);

	obs_property_t *d_fetch = obs_properties_add_button2(
		props, "discord_fetch", obs_module_text("DiscordFetch"),
		on_discord_fetch, ctx);
	obs_property_set_visible(d_fetch, d_on);

	/* YouTube Chat integration */
	bool yt_on = obs_data_get_bool(settings, "yt_enabled");

	obs_property_t *yt_cb = obs_properties_add_bool(
		props, "yt_enabled", obs_module_text("YouTubeEnabled"));
	obs_property_set_modified_callback2(yt_cb, on_yt_toggled, ctx);

	obs_property_t *yt_url = obs_properties_add_text(
		props, "yt_channel_url", obs_module_text("YouTubeChannelURL"),
		OBS_TEXT_DEFAULT);
	obs_property_set_visible(yt_url, yt_on);

	obs_property_t *yt_start = obs_properties_add_button2(
		props, "yt_start_collecting",
		obs_module_text("YouTubeStartCollecting"),
		on_yt_start_collecting, ctx);
	obs_property_set_visible(yt_start, yt_on);

	/* YouTube styling group */
	bool yt_expand = obs_data_get_bool(settings, "yt_expand");
	const char *yt_heading_val =
		obs_data_get_string(settings, "yt_heading");
	char yt_group_label[128];
	if (yt_heading_val && yt_heading_val[0] != '\0')
		snprintf(yt_group_label, sizeof(yt_group_label), "%s",
			 yt_heading_val);
	else
		snprintf(yt_group_label, sizeof(yt_group_label), "%s",
			 obs_module_text("YouTubeChatSection"));

	obs_properties_t *yt_group = obs_properties_create();

	/* Expand toggle */
	obs_property_t *yt_exp = obs_properties_add_bool(
		yt_group, "yt_expand", obs_module_text("ExpandSection"));
	obs_property_set_modified_callback2(yt_exp, on_yt_expand_toggled, ctx);

	/* Heading */
	obs_properties_add_text(yt_group, "yt_heading",
				obs_module_text("YouTubeHeading"),
				OBS_TEXT_DEFAULT);

	/* Fonts */
	obs_properties_add_font(yt_group, "yt_heading_font",
				obs_module_text("HeadingFont"));
	obs_properties_add_font(yt_group, "yt_entry_font",
				obs_module_text("EntryFont"));

	/* Alignment */
	obs_property_t *yt_align = obs_properties_add_list(
		yt_group, "yt_alignment", obs_module_text("Alignment"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(yt_align, obs_module_text("AlignLeft"),
				     "left");
	obs_property_list_add_string(yt_align, obs_module_text("AlignCenter"),
				     "center");
	obs_property_list_add_string(yt_align, obs_module_text("AlignRight"),
				     "right");

	/* Colors */
	obs_properties_add_color(yt_group, "yt_heading_color",
				 obs_module_text("SectionHeadingColor"));
	obs_properties_add_color(yt_group, "yt_text_color",
				 obs_module_text("SectionTextColor"));

	/* Outline */
	bool yt_ol_on = obs_data_get_bool(settings, "yt_outline_enabled");
	obs_property_t *yt_ol_cb = obs_properties_add_bool(
		yt_group, "yt_outline_enabled",
		obs_module_text("SectionOutlineEnabled"));
	obs_property_set_modified_callback2(yt_ol_cb, on_yt_outline_toggled,
					    ctx);
	obs_properties_add_int(yt_group, "yt_outline_size",
			       obs_module_text("SectionOutlineSize"), 1, 20, 1);
	obs_properties_add_color(yt_group, "yt_outline_color",
				 obs_module_text("SectionOutlineColor"));
	obs_properties_add_bool(yt_group, "yt_outline_heading",
				obs_module_text("OutlineApplyHeading"));
	obs_properties_add_bool(yt_group, "yt_outline_entries",
				obs_module_text("OutlineApplyEntries"));

	/* Shadow */
	bool yt_sh_on = obs_data_get_bool(settings, "yt_shadow_enabled");
	obs_property_t *yt_sh_cb = obs_properties_add_bool(
		yt_group, "yt_shadow_enabled",
		obs_module_text("SectionShadowEnabled"));
	obs_property_set_modified_callback2(yt_sh_cb, on_yt_shadow_toggled,
					    ctx);
	obs_properties_add_color(yt_group, "yt_shadow_color",
				 obs_module_text("SectionShadowColor"));
	obs_properties_add_float(yt_group, "yt_shadow_offset_x",
				 obs_module_text("SectionShadowOffsetX"),
				 -20.0, 20.0, 0.5);
	obs_properties_add_float(yt_group, "yt_shadow_offset_y",
				 obs_module_text("SectionShadowOffsetY"),
				 -20.0, 20.0, 0.5);
	obs_properties_add_bool(yt_group, "yt_shadow_heading",
				obs_module_text("ShadowApplyHeading"));
	obs_properties_add_bool(yt_group, "yt_shadow_entries",
				obs_module_text("ShadowApplyEntries"));

	/* Spacing */
	obs_properties_add_float(yt_group, "yt_heading_spacing",
				 obs_module_text("SectionHeadingSpacing"),
				 -9999.0, 9999.0, 1.0);
	obs_properties_add_float(yt_group, "yt_entry_spacing",
				 obs_module_text("SectionEntrySpacing"),
				 -9999.0, 9999.0, 1.0);
	obs_properties_add_float(yt_group, "yt_section_spacing",
				 obs_module_text("SectionSectionSpacing"),
				 -9999.0, 9999.0, 1.0);

	/* Set initial visibility of group contents based on expand state */
	const char *yt_expand_props[] = {
		"yt_heading",      "yt_heading_font",  "yt_entry_font",
		"yt_alignment",    "yt_heading_color", "yt_text_color",
		"yt_outline_enabled", "yt_shadow_enabled",
		"yt_heading_spacing", "yt_entry_spacing", "yt_section_spacing",
	};
	for (int i = 0; i < 11; i++) {
		obs_property_t *p =
			obs_properties_get(yt_group, yt_expand_props[i]);
		if (p)
			obs_property_set_visible(p, yt_expand);
	}
	/* Outline sub-properties */
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_outline_size"),
		yt_expand && yt_ol_on);
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_outline_color"),
		yt_expand && yt_ol_on);
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_outline_heading"),
		yt_expand && yt_ol_on);
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_outline_entries"),
		yt_expand && yt_ol_on);
	/* Shadow sub-properties */
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_shadow_color"),
		yt_expand && yt_sh_on);
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_shadow_offset_x"),
		yt_expand && yt_sh_on);
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_shadow_offset_y"),
		yt_expand && yt_sh_on);
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_shadow_heading"),
		yt_expand && yt_sh_on);
	obs_property_set_visible(
		obs_properties_get(yt_group, "yt_shadow_entries"),
		yt_expand && yt_sh_on);

	obs_property_t *yt_grp = obs_properties_add_group(
		props, "yt_section_group", yt_group_label, OBS_GROUP_NORMAL,
		yt_group);
	obs_property_set_visible(yt_grp, yt_on);

	/* General settings */
	obs_properties_add_float(props, "scroll_speed",
				 obs_module_text("ScrollSpeed"), 10.0, 500.0,
				 5.0);

	obs_properties_add_bool(props, "loop", obs_module_text("Loop"));

	obs_properties_add_int(props, "width", obs_module_text("Width"), 640,
			       3840, 1);

	obs_properties_add_int(props, "height", obs_module_text("Height"), 480,
			       2160, 1);

	obs_properties_add_font(props, "font",
				obs_module_text("DefaultFont"));

	/* Delay settings */
	obs_properties_add_float(props, "start_delay",
				 obs_module_text("StartDelay"), 0.0, 30.0,
				 0.5);
	obs_properties_add_float(props, "loop_delay",
				 obs_module_text("LoopDelay"), 0.0, 30.0,
				 0.5);

	obs_data_release(settings);

	return props;
}

struct obs_source_info credits_source_info = {
	.id = "obs_credits_roll",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = credits_get_name,
	.create = credits_create,
	.destroy = credits_destroy,
	.activate = credits_activate,
	.update = credits_update,
	.video_tick = credits_video_tick,
	.video_render = credits_video_render,
	.get_width = credits_get_width,
	.get_height = credits_get_height,
	.get_properties = credits_get_properties,
	.get_defaults = credits_get_defaults,
};
