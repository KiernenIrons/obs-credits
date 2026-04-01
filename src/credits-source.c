#include "credits-source.h"
#include "credits-parser.h"
#include "credits-renderer.h"

#include <obs-module.h>
#include <graphics/graphics.h>

#include <pthread.h>
#include <string.h>

static void credits_update(void *data, obs_data_t *settings);

struct credits_source {
	obs_source_t *self;

	/* Settings */
	char *credits_file;
	float scroll_speed;
	bool loop;
	uint32_t width;
	uint32_t height;
	char *default_font_face;
	int default_font_size;
	uint32_t heading_color;
	uint32_t text_color;

	/* State */
	struct credits_data *data;
	struct credits_layout *layout;
	gs_texrender_t *texrender;
	float scroll_offset;
	float current_speed;
	float total_height;
	bool scrolling;
	bool started;

	pthread_mutex_t mutex;
};

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

	/* Take ownership of resources under lock */
	pthread_mutex_lock(&ctx->mutex);
	struct credits_layout *layout = ctx->layout;
	struct credits_data *cdata = ctx->data;
	ctx->layout = NULL;
	ctx->data = NULL;
	pthread_mutex_unlock(&ctx->mutex);

	/* Free outside mutex (renderer calls obs_enter_graphics) */
	credits_renderer_free(layout);
	credits_data_free(cdata);

	obs_enter_graphics();
	gs_texrender_destroy(ctx->texrender);
	obs_leave_graphics();

	pthread_mutex_destroy(&ctx->mutex);
	bfree(ctx->credits_file);
	bfree(ctx->default_font_face);
	bfree(ctx);
}

static void credits_update(void *data, obs_data_t *settings)
{
	struct credits_source *ctx = data;

	const char *file = obs_data_get_string(settings, "credits_file");
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

	uint32_t hcolor =
		(uint32_t)obs_data_get_int(settings, "heading_color");
	uint32_t tcolor =
		(uint32_t)obs_data_get_int(settings, "text_color");

	/* Swap out old layout/data under lock, free outside lock to
	 * avoid holding mutex while obs_enter_graphics runs (deadlock) */
	pthread_mutex_lock(&ctx->mutex);

	struct credits_layout *old_layout = ctx->layout;
	struct credits_data *old_data = ctx->data;
	ctx->layout = NULL;
	ctx->data = NULL;

	bfree(ctx->credits_file);
	ctx->credits_file = (file && file[0] != '\0') ? bstrdup(file) : NULL;

	ctx->scroll_speed = speed > 0.0 ? (float)speed : 60.0f;
	ctx->loop = loop_val;
	ctx->width = w > 0 ? (uint32_t)w : 1920;
	ctx->height = h > 0 ? (uint32_t)h : 1080;

	bfree(ctx->default_font_face);
	ctx->default_font_face = face ? face : bstrdup("Arial");
	ctx->default_font_size = size > 0 ? size : 32;

	ctx->heading_color = hcolor != 0 ? hcolor : 0xFFFFD700;
	ctx->text_color = tcolor != 0 ? tcolor : 0xFFFFFFFF;

	/* Parse new credits file */
	if (ctx->credits_file)
		ctx->data = credits_parse_file(ctx->credits_file);

	/* Reset scroll state - layout rebuilt on next video_tick */
	ctx->scroll_offset = 0.0f;
	ctx->current_speed = 0.0f;
	ctx->scrolling = true;
	ctx->started = false;

	pthread_mutex_unlock(&ctx->mutex);

	/* Free old resources outside mutex (renderer calls obs_enter_graphics) */
	credits_renderer_free(old_layout);
	credits_data_free(old_data);
}

static void credits_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "scroll_speed", 60.0);
	obs_data_set_default_bool(settings, "loop", false);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_int(settings, "heading_color", 0xFFFFD700);
	obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF);
}

static void credits_video_tick(void *data, float seconds)
{
	struct credits_source *ctx = data;

	if (seconds <= 0.0f || seconds > 1.0f)
		seconds = 0.016f;

	pthread_mutex_lock(&ctx->mutex);

	/* Build layout on graphics thread if we have data but no layout */
	if (ctx->data && !ctx->layout) {
		ctx->layout = credits_renderer_build(
			ctx->data, ctx->width, ctx->default_font_face,
			ctx->default_font_size, ctx->heading_color,
			ctx->text_color);
		if (ctx->layout)
			ctx->total_height =
				credits_renderer_total_height(ctx->layout);
	}

	/* Advance scroll */
	if (ctx->scrolling && ctx->layout) {
		if (!ctx->started) {
			ctx->scroll_offset = -(float)ctx->height;
			ctx->started = true;
		}

		/* Smooth acceleration */
		ctx->current_speed +=
			(ctx->scroll_speed - ctx->current_speed) * 5.0f *
			seconds;
		ctx->scroll_offset += ctx->current_speed * seconds;

		/* End / loop check */
		if (ctx->scroll_offset > ctx->total_height) {
			if (ctx->loop) {
				ctx->scroll_offset = -(float)ctx->height;
				ctx->current_speed = 0.0f;
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

	/* Create texrender on first call (must be on graphics thread) */
	if (!ctx->texrender)
		ctx->texrender =
			gs_texrender_create(GS_RGBA, GS_ZS_NONE);

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
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, "credits_file",
				obs_module_text("CreditsFile"), OBS_PATH_FILE,
				"JSON files (*.json)", NULL);

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
