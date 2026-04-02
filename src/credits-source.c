#include "credits-source.h"
#include "credits-parser.h"
#include "credits-renderer.h"

#include <obs-module.h>
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
	uint32_t heading_color;
	uint32_t text_color;

	/* Outline settings */
	bool outline_enabled;
	int outline_size;
	uint32_t outline_color;

	/* Shadow settings */
	bool shadow_enabled;
	uint32_t shadow_color;
	float shadow_offset_x;
	float shadow_offset_y;

	/* Delay settings */
	float start_delay;
	float loop_delay;
	float delay_timer;

	/* State */
	struct credits_data *data;
	struct credits_layout *layout;
	gs_texrender_t *texrender;
	float scroll_offset;
	float current_speed;
	float total_height;
	bool scrolling;
	bool started;
	bool waiting_loop;

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

		snprintf(key, sizeof(key), "section_%d_bold", i);
		obs_data_set_bool(sec, "bold",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_italic", i);
		obs_data_set_bool(sec, "italic",
				  obs_data_get_bool(settings, key));

		snprintf(key, sizeof(key), "section_%d_underline", i);
		obs_data_set_bool(sec, "underline",
				  obs_data_get_bool(settings, key));

		obs_data_array_push_back(arr, sec);
		obs_data_release(sec);
	}

	return arr;
}

/* Helper: shift all per-section keys from src index to dst index */
static void shift_section_keys(obs_data_t *settings, int dst, int src)
{
	char sk[64], dk[64];
	const char *fields[] = {"heading", "subheading", "names", "roles",
				"alignment"};
	for (int f = 0; f < 5; f++) {
		snprintf(sk, sizeof(sk), "section_%d_%s", src, fields[f]);
		snprintf(dk, sizeof(dk), "section_%d_%s", dst, fields[f]);
		obs_data_set_string(settings, dk,
				    obs_data_get_string(settings, sk));
	}
	const char *bools[] = {"bold", "italic", "underline"};
	for (int f = 0; f < 3; f++) {
		snprintf(sk, sizeof(sk), "section_%d_%s", src, bools[f]);
		snprintf(dk, sizeof(dk), "section_%d_%s", dst, bools[f]);
		obs_data_set_bool(settings, dk,
				  obs_data_get_bool(settings, sk));
	}
}

/* ---- Deferred properties rebuild ---- */

/* obs_source_update_properties cannot be called from inside a button
 * callback because it destroys the properties panel while Qt is still
 * processing the click event, causing a use-after-free crash.
 * Instead, queue the rebuild on the UI thread so it runs after the
 * callback returns. */
static void deferred_update_properties(void *param)
{
	obs_source_t *source = param;
	obs_source_update_properties(source);
}

/* ---- Section add/remove button callbacks ---- */

static bool on_add_section(obs_properties_t *props, obs_property_t *prop,
			   void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	int count = (int)obs_data_get_int(settings, "section_count");
	obs_data_set_int(settings, "section_count", count + 1);

	/* Clear all fields for the new section so it doesn't inherit
	 * stale data from a previously removed section at this index */
	char key[64];
	snprintf(key, sizeof(key), "section_%d_heading", count);
	obs_data_set_string(settings, key, "");
	snprintf(key, sizeof(key), "section_%d_subheading", count);
	obs_data_set_string(settings, key, "");
	snprintf(key, sizeof(key), "section_%d_names", count);
	obs_data_set_string(settings, key, "");
	snprintf(key, sizeof(key), "section_%d_roles", count);
	obs_data_set_string(settings, key, "");
	snprintf(key, sizeof(key), "section_%d_alignment", count);
	obs_data_set_string(settings, key, "center");
	snprintf(key, sizeof(key), "section_%d_bold", count);
	obs_data_set_bool(settings, key, false);
	snprintf(key, sizeof(key), "section_%d_italic", count);
	obs_data_set_bool(settings, key, false);
	snprintf(key, sizeof(key), "section_%d_underline", count);
	obs_data_set_bool(settings, key, false);

	obs_data_release(settings);

	/* Defer properties rebuild to after this callback returns */
	obs_queue_task(OBS_TASK_UI, deferred_update_properties, ctx->self,
		       false);
	return true;
}

static bool on_remove_section(obs_properties_t *props, obs_property_t *prop,
			      void *data)
{
	UNUSED_PARAMETER(props);

	struct credits_source *ctx = data;
	obs_data_t *settings = obs_source_get_settings(ctx->self);

	const char *name = obs_property_name(prop);
	int idx = 0;
	if (sscanf(name, "section_%d_remove", &idx) != 1) {
		obs_data_release(settings);
		return false;
	}

	int count = (int)obs_data_get_int(settings, "section_count");
	if (idx >= count || count <= 1) {
		obs_data_release(settings);
		return false;
	}

	for (int i = idx; i < count - 1; i++)
		shift_section_keys(settings, i, i + 1);

	obs_data_set_int(settings, "section_count", count - 1);

	obs_data_release(settings);

	/* Defer properties rebuild to after this callback returns */
	obs_queue_task(OBS_TASK_UI, deferred_update_properties, ctx->self,
		       false);
	return true;
}

/* ---- Outline/shadow visibility callbacks ---- */

static bool on_outline_toggled(void *data, obs_properties_t *props,
			       obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(prop);
	bool enabled = obs_data_get_bool(settings, "outline_enabled");
	obs_property_set_visible(obs_properties_get(props, "outline_size"),
				 enabled);
	obs_property_set_visible(obs_properties_get(props, "outline_color"),
				 enabled);
	return true;
}

static bool on_shadow_toggled(void *data, obs_properties_t *props,
			      obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(prop);
	bool enabled = obs_data_get_bool(settings, "shadow_enabled");
	obs_property_set_visible(obs_properties_get(props, "shadow_color"),
				 enabled);
	obs_property_set_visible(obs_properties_get(props, "shadow_offset_x"),
				 enabled);
	obs_property_set_visible(obs_properties_get(props, "shadow_offset_y"),
				 enabled);
	return true;
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
	credits_update(ctx, settings);
	return ctx;
}

static void credits_destroy(void *data)
{
	struct credits_source *ctx = data;

	pthread_mutex_lock(&ctx->mutex);
	struct credits_layout *layout = ctx->layout;
	struct credits_data *cdata = ctx->data;
	ctx->layout = NULL;
	ctx->data = NULL;
	pthread_mutex_unlock(&ctx->mutex);

	credits_renderer_free(layout);
	credits_data_free(cdata);

	obs_enter_graphics();
	gs_texrender_destroy(ctx->texrender);
	obs_leave_graphics();

	pthread_mutex_destroy(&ctx->mutex);
	bfree(ctx->default_font_face);
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

	/* Colors from obs_properties_add_color: 0x00BBGGRR format (no alpha).
	 * We add alpha when passing to the renderer/text source. */
	uint32_t hcolor =
		(uint32_t)obs_data_get_int(settings, "heading_color");
	uint32_t tcolor =
		(uint32_t)obs_data_get_int(settings, "text_color");

	bool outline_on = obs_data_get_bool(settings, "outline_enabled");
	int outline_sz = (int)obs_data_get_int(settings, "outline_size");
	uint32_t outline_col =
		(uint32_t)obs_data_get_int(settings, "outline_color");

	bool shadow_on = obs_data_get_bool(settings, "shadow_enabled");
	uint32_t shadow_col =
		(uint32_t)obs_data_get_int(settings, "shadow_color");
	double shadow_ox =
		obs_data_get_double(settings, "shadow_offset_x");
	double shadow_oy =
		obs_data_get_double(settings, "shadow_offset_y");

	double start_del = obs_data_get_double(settings, "start_delay");
	double loop_del = obs_data_get_double(settings, "loop_delay");

	obs_data_array_t *arr = settings_to_sections_array(settings);

	obs_data_t *tmp = obs_data_create();
	if (arr) {
		obs_data_set_array(tmp, "sections", arr);
		obs_data_array_release(arr);
	}

	pthread_mutex_lock(&ctx->mutex);

	struct credits_layout *old_layout = ctx->layout;
	struct credits_data *old_data = ctx->data;
	ctx->layout = NULL;
	ctx->data = NULL;

	ctx->scroll_speed = speed > 0.0 ? (float)speed : 60.0f;
	ctx->loop = loop_val;
	ctx->width = w > 0 ? (uint32_t)w : 1920;
	ctx->height = h > 0 ? (uint32_t)h : 1080;

	bfree(ctx->default_font_face);
	ctx->default_font_face = face ? face : bstrdup("Arial");
	ctx->default_font_size = size > 0 ? size : 32;

	ctx->heading_color = hcolor;
	ctx->text_color = tcolor;

	ctx->outline_enabled = outline_on;
	ctx->outline_size = outline_sz;
	ctx->outline_color = outline_col;

	ctx->shadow_enabled = shadow_on;
	ctx->shadow_color = shadow_col;
	ctx->shadow_offset_x = (float)shadow_ox;
	ctx->shadow_offset_y = (float)shadow_oy;

	ctx->start_delay = (float)start_del;
	ctx->loop_delay = (float)loop_del;
	ctx->delay_timer = 0.0f;

	ctx->data = credits_build_from_settings(tmp);

	ctx->scroll_offset = 0.0f;
	ctx->current_speed = 0.0f;
	ctx->scrolling = true;
	ctx->started = false;
	ctx->waiting_loop = false;

	pthread_mutex_unlock(&ctx->mutex);

	obs_data_release(tmp);

	credits_renderer_free(old_layout);
	credits_data_free(old_data);
}

static void credits_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "scroll_speed", 60.0);
	obs_data_set_default_bool(settings, "loop", false);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);

	/* obs_properties_add_color uses 0x00BBGGRR (no alpha byte).
	 * Gold (#FFD700): R=FF G=D7 B=00 -> 0x0000D7FF
	 * White (#FFFFFF): -> 0x00FFFFFF */
	obs_data_set_default_int(settings, "heading_color",
				 (long long)0x0000D7FF);
	obs_data_set_default_int(settings, "text_color",
				 (long long)0x00FFFFFF);

	obs_data_set_default_int(settings, "section_count", 1);
	obs_data_set_default_string(settings, "section_0_alignment", "center");

	obs_data_set_default_bool(settings, "outline_enabled", false);
	obs_data_set_default_int(settings, "outline_size", 2);
	obs_data_set_default_int(settings, "outline_color",
				 (long long)0x00000000);

	obs_data_set_default_bool(settings, "shadow_enabled", false);
	obs_data_set_default_int(settings, "shadow_color",
				 (long long)0x00000000);
	obs_data_set_default_double(settings, "shadow_offset_x", 2.0);
	obs_data_set_default_double(settings, "shadow_offset_y", 2.0);

	obs_data_set_default_double(settings, "start_delay", 0.0);
	obs_data_set_default_double(settings, "loop_delay", 0.0);
}

static void credits_video_tick(void *data, float seconds)
{
	struct credits_source *ctx = data;

	if (seconds <= 0.0f || seconds > 1.0f)
		seconds = 0.016f;

	pthread_mutex_lock(&ctx->mutex);

	if (ctx->data && !ctx->layout) {
		struct credits_style style;
		style.default_font = ctx->default_font_face;
		style.default_font_size = ctx->default_font_size;
		style.heading_color = ctx->heading_color;
		style.text_color = ctx->text_color;
		style.outline_enabled = ctx->outline_enabled;
		style.outline_size = ctx->outline_size;
		style.outline_color = ctx->outline_color;
		style.shadow_enabled = ctx->shadow_enabled;
		style.shadow_color = ctx->shadow_color;
		style.shadow_offset_x = ctx->shadow_offset_x;
		style.shadow_offset_y = ctx->shadow_offset_y;

		ctx->layout = credits_renderer_build(ctx->data, ctx->width,
						     &style);
		if (ctx->layout)
			ctx->total_height =
				credits_renderer_total_height(ctx->layout);
	}

	if (ctx->scrolling && ctx->layout) {
		if (!ctx->started) {
			ctx->scroll_offset = -(float)ctx->height;
			ctx->delay_timer = 0.0f;
			ctx->started = true;
		}

		/* Start delay */
		if (ctx->start_delay > 0.0f &&
		    ctx->delay_timer < ctx->start_delay) {
			ctx->delay_timer += seconds;
			pthread_mutex_unlock(&ctx->mutex);
			return;
		}

		/* Loop delay */
		if (ctx->waiting_loop) {
			ctx->delay_timer += seconds;
			if (ctx->delay_timer >= ctx->loop_delay) {
				ctx->waiting_loop = false;
				ctx->scroll_offset = -(float)ctx->height;
				ctx->current_speed = 0.0f;
				ctx->delay_timer = ctx->start_delay;
			}
			pthread_mutex_unlock(&ctx->mutex);
			return;
		}

		ctx->current_speed +=
			(ctx->scroll_speed - ctx->current_speed) * 5.0f *
			seconds;
		ctx->scroll_offset += ctx->current_speed * seconds;

		if (ctx->scroll_offset > ctx->total_height) {
			if (ctx->loop) {
				if (ctx->loop_delay > 0.0f) {
					ctx->waiting_loop = true;
					ctx->delay_timer = 0.0f;
				} else {
					ctx->scroll_offset =
						-(float)ctx->height;
					ctx->current_speed = 0.0f;
				}
			} else {
				ctx->scrolling = false;
			}
		}
	}

	pthread_mutex_unlock(&ctx->mutex);
}

static void credits_video_render(void *data, gs_effect_t *effect)
{
	struct credits_source *ctx = data;

	pthread_mutex_lock(&ctx->mutex);

	if (!ctx->layout || ctx->width == 0 || ctx->height == 0) {
		pthread_mutex_unlock(&ctx->mutex);
		UNUSED_PARAMETER(effect);
		return;
	}

	if (!ctx->texrender)
		ctx->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	gs_texrender_reset(ctx->texrender);

	if (gs_texrender_begin(ctx->texrender, ctx->width, ctx->height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

		gs_ortho(0.0f, (float)ctx->width, ctx->scroll_offset,
			 ctx->scroll_offset + (float)ctx->height, -1.0f,
			 1.0f);

		credits_renderer_draw(ctx->layout, ctx->width,
				      ctx->scroll_offset,
				      (float)ctx->height);

		gs_texrender_end(ctx->texrender);
	}

	gs_texture_t *tex = gs_texrender_get_texture(ctx->texrender);
	if (tex) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *param =
			gs_effect_get_param_by_name(def, "image");
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
	if (count < 0)
		count = 0;

	for (int i = 0; i < count; i++) {
		char group_name[64], heading_name[64], sub_name[64];
		char names_name[64], roles_name[64], align_name[64];
		char bold_name[64], italic_name[64], underline_name[64];
		char remove_name[64], label[64];

		snprintf(group_name, sizeof(group_name), "section_%d_group",
			 i);
		snprintf(heading_name, sizeof(heading_name),
			 "section_%d_heading", i);
		snprintf(sub_name, sizeof(sub_name), "section_%d_subheading",
			 i);
		snprintf(names_name, sizeof(names_name), "section_%d_names",
			 i);
		snprintf(roles_name, sizeof(roles_name), "section_%d_roles",
			 i);
		snprintf(align_name, sizeof(align_name),
			 "section_%d_alignment", i);
		snprintf(bold_name, sizeof(bold_name), "section_%d_bold", i);
		snprintf(italic_name, sizeof(italic_name),
			 "section_%d_italic", i);
		snprintf(underline_name, sizeof(underline_name),
			 "section_%d_underline", i);
		snprintf(remove_name, sizeof(remove_name),
			 "section_%d_remove", i);
		snprintf(label, sizeof(label), "%s %d",
			 obs_module_text("Section"), i + 1);

		obs_properties_t *group = obs_properties_create();

		obs_properties_add_text(group, heading_name,
					obs_module_text("Heading"),
					OBS_TEXT_DEFAULT);

		obs_properties_add_text(group, sub_name,
					obs_module_text("Subheading"),
					OBS_TEXT_DEFAULT);

		obs_properties_add_text(group, names_name,
					obs_module_text("Names"),
					OBS_TEXT_MULTILINE);

		obs_properties_add_text(group, roles_name,
					obs_module_text("Roles"),
					OBS_TEXT_MULTILINE);

		obs_property_t *align_prop = obs_properties_add_list(
			group, align_name, obs_module_text("Alignment"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignLeft"), "left");
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignCenter"), "center");
		obs_property_list_add_string(
			align_prop, obs_module_text("AlignRight"), "right");

		obs_properties_add_bool(group, bold_name,
					obs_module_text("Bold"));
		obs_properties_add_bool(group, italic_name,
					obs_module_text("Italic"));
		obs_properties_add_bool(group, underline_name,
					obs_module_text("Underline"));

		/* Only show remove button if there are multiple sections */
		if (count > 1) {
			obs_properties_add_button2(
				group, remove_name,
				obs_module_text("RemoveSection"),
				on_remove_section, ctx);
		}

		obs_properties_add_group(props, group_name, label,
					 OBS_GROUP_NORMAL, group);
	}

	obs_properties_add_button2(props, "add_section",
				   obs_module_text("AddSection"),
				   on_add_section, ctx);

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

	obs_properties_add_color(props, "heading_color",
				 obs_module_text("HeadingColor"));

	obs_properties_add_color(props, "text_color",
				 obs_module_text("TextColor"));

	/* Outline - checkbox with show/hide for sub-options */
	obs_property_t *outline_cb = obs_properties_add_bool(
		props, "outline_enabled",
		obs_module_text("OutlineEnabled"));
	obs_property_set_modified_callback2(outline_cb, on_outline_toggled,
					    ctx);

	obs_property_t *ol_size = obs_properties_add_int(
		props, "outline_size", obs_module_text("OutlineSize"), 1, 20,
		1);
	obs_property_t *ol_color = obs_properties_add_color(
		props, "outline_color", obs_module_text("OutlineColor"));

	bool ol_on = obs_data_get_bool(settings, "outline_enabled");
	obs_property_set_visible(ol_size, ol_on);
	obs_property_set_visible(ol_color, ol_on);

	/* Shadow - checkbox with show/hide for sub-options */
	obs_property_t *shadow_cb = obs_properties_add_bool(
		props, "shadow_enabled",
		obs_module_text("ShadowEnabled"));
	obs_property_set_modified_callback2(shadow_cb, on_shadow_toggled, ctx);

	obs_property_t *sh_color = obs_properties_add_color(
		props, "shadow_color", obs_module_text("ShadowColor"));
	obs_property_t *sh_ox = obs_properties_add_float(
		props, "shadow_offset_x", obs_module_text("ShadowOffsetX"),
		-20.0, 20.0, 0.5);
	obs_property_t *sh_oy = obs_properties_add_float(
		props, "shadow_offset_y", obs_module_text("ShadowOffsetY"),
		-20.0, 20.0, 0.5);

	bool sh_on = obs_data_get_bool(settings, "shadow_enabled");
	obs_property_set_visible(sh_color, sh_on);
	obs_property_set_visible(sh_ox, sh_on);
	obs_property_set_visible(sh_oy, sh_on);

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
	.update = credits_update,
	.video_tick = credits_video_tick,
	.video_render = credits_video_render,
	.get_width = credits_get_width,
	.get_height = credits_get_height,
	.get_properties = credits_get_properties,
	.get_defaults = credits_get_defaults,
};
