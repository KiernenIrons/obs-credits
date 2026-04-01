#include "credits-renderer.h"
#include "credits-parser.h"

#include <obs-module.h>
#include <graphics/image-file.h>

#include <stdio.h>
#include <string.h>

#define ENTRY_GAP 8.0f
#define SECTION_GAP 60.0f

enum layout_elem_type {
	ELEM_TEXT,
	ELEM_IMAGE,
	ELEM_SPACER,
};

struct layout_elem {
	enum layout_elem_type type;
	float y;
	float height;
	float x;
	obs_source_t *text_source;
	gs_image_file3_t *image;
	uint32_t image_width;
	uint32_t image_height;
};

struct credits_layout {
	struct layout_elem *elems;
	size_t num_elems;
	float total_height;
};

static obs_source_t *make_text_source(const char *name, const char *text,
				      const char *font_face, int font_size,
				      uint32_t color)
{
	obs_data_t *font_data = obs_data_create();
	obs_data_set_string(font_data, "face", font_face);
	obs_data_set_int(font_data, "size", font_size);
	obs_data_set_int(font_data, "flags", 0);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text);
	obs_data_set_obj(settings, "font", font_data);
	obs_data_set_int(settings, "color1", color);
	obs_data_set_int(settings, "color2", color);

#ifdef _WIN32
	const char *text_source_id = "text_gdiplus";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	obs_source_t *source =
		obs_source_create_private(text_source_id, name, settings);

	obs_data_release(settings);
	obs_data_release(font_data);

	return source;
}

static float text_source_height(obs_source_t *source)
{
	uint32_t h = obs_source_get_height(source);
	return h > 0 ? (float)h : 40.0f;
}

static float text_source_width(obs_source_t *source)
{
	uint32_t w = obs_source_get_width(source);
	return w > 0 ? (float)w : 100.0f;
}

static size_t count_total_elems(const struct credits_data *data)
{
	size_t count = 0;
	for (size_t i = 0; i < data->num_sections; i++) {
		count += 1; /* heading */
		count += data->sections[i].num_entries;
		count += 1; /* section gap spacer */
	}
	return count;
}

struct credits_layout *credits_renderer_build(
	const struct credits_data *data, uint32_t viewport_width,
	const char *default_font, int default_font_size,
	uint32_t heading_color, uint32_t text_color)
{
	if (!data || data->num_sections == 0)
		return NULL;

	struct credits_layout *layout =
		bzalloc(sizeof(struct credits_layout));
	layout->num_elems = count_total_elems(data);
	layout->elems =
		bzalloc(sizeof(struct layout_elem) * layout->num_elems);

	float y_cursor = 0.0f;
	size_t elem_idx = 0;
	char name_buf[128];
	float vw = (float)viewport_width;

	for (size_t s = 0; s < data->num_sections; s++) {
		const struct credits_section *section = &data->sections[s];

		/* --- Heading --- */
		const char *h_font = section->heading_font
					     ? section->heading_font
					     : default_font;
		int h_size = section->heading_font_size > 0
				     ? section->heading_font_size
				     : default_font_size;
		uint32_t h_color = section->heading_color != 0
					   ? section->heading_color
					   : heading_color;

		const char *heading_text =
			section->heading ? section->heading : "";

		snprintf(name_buf, sizeof(name_buf), "credits_heading_%zu", s);

		struct layout_elem *he = &layout->elems[elem_idx++];
		he->type = ELEM_TEXT;
		he->text_source = make_text_source(name_buf, heading_text,
						   h_font, h_size, h_color);
		he->height = text_source_height(he->text_source);
		he->x = (vw - text_source_width(he->text_source)) / 2.0f;
		he->y = y_cursor;
		y_cursor += he->height + ENTRY_GAP * 2.0f;

		/* --- Entries --- */
		for (size_t e = 0; e < section->num_entries; e++) {
			const struct credits_entry *entry =
				&section->entries[e];
			struct layout_elem *le = &layout->elems[elem_idx++];

			switch (entry->type) {
			case CREDITS_ENTRY_NAME_ROLE: {
				snprintf(name_buf, sizeof(name_buf),
					 "credits_entry_%zu_%zu", s, e);
				char combined[512];
				snprintf(combined, sizeof(combined),
					 "%s  -  %s",
					 entry->name ? entry->name : "",
					 entry->role ? entry->role : "");
				le->type = ELEM_TEXT;
				le->text_source = make_text_source(
					name_buf, combined, default_font,
					default_font_size, text_color);
				le->height =
					text_source_height(le->text_source);
				le->x = (vw - text_source_width(
						      le->text_source)) /
					2.0f;
				break;
			}
			case CREDITS_ENTRY_NAME_ONLY: {
				snprintf(name_buf, sizeof(name_buf),
					 "credits_name_%zu_%zu", s, e);
				le->type = ELEM_TEXT;
				le->text_source = make_text_source(
					name_buf,
					entry->name ? entry->name : "",
					default_font, default_font_size,
					text_color);
				le->height =
					text_source_height(le->text_source);
				le->x = (vw - text_source_width(
						      le->text_source)) /
					2.0f;
				break;
			}
			case CREDITS_ENTRY_TEXT: {
				snprintf(name_buf, sizeof(name_buf),
					 "credits_text_%zu_%zu", s, e);
				le->type = ELEM_TEXT;
				le->text_source = make_text_source(
					name_buf,
					entry->text ? entry->text : "",
					default_font, default_font_size,
					text_color);
				le->height =
					text_source_height(le->text_source);
				le->x = (vw - text_source_width(
						      le->text_source)) /
					2.0f;
				break;
			}
			case CREDITS_ENTRY_IMAGE: {
				le->type = ELEM_IMAGE;
				le->image = bzalloc(sizeof(gs_image_file3_t));
				if (entry->image_path) {
					gs_image_file3_init(le->image,
							    entry->image_path,
							    GS_IMAGE_ALPHA_STRAIGHT);
					gs_image_file3_init_texture(le->image);
				}
				le->image_width =
					entry->image_width > 0
						? (uint32_t)entry->image_width
						: le->image->image2.image
							  .cx;
				le->image_height =
					entry->image_height > 0
						? (uint32_t)entry->image_height
						: le->image->image2.image
							  .cy;
				le->height = (float)le->image_height;
				le->x = (vw - (float)le->image_width) / 2.0f;
				break;
			}
			case CREDITS_ENTRY_SPACER: {
				le->type = ELEM_SPACER;
				le->height = (float)entry->spacer_height;
				le->x = 0.0f;
				break;
			}
			}

			le->y = y_cursor;
			y_cursor += le->height + ENTRY_GAP;
		}

		/* --- Section gap spacer --- */
		struct layout_elem *gap = &layout->elems[elem_idx++];
		gap->type = ELEM_SPACER;
		gap->y = y_cursor;
		gap->height = SECTION_GAP;
		gap->x = 0.0f;
		y_cursor += SECTION_GAP;
	}

	layout->total_height = y_cursor;

	blog(LOG_INFO,
	     "[obs-credits] Layout built: %zu elements, total height %.0f px",
	     layout->num_elems, layout->total_height);

	return layout;
}

float credits_renderer_total_height(const struct credits_layout *layout)
{
	if (!layout)
		return 0.0f;
	return layout->total_height;
}

void credits_renderer_draw(const struct credits_layout *layout,
			   uint32_t viewport_width, float scroll_y,
			   float viewport_h)
{
	UNUSED_PARAMETER(viewport_width);

	if (!layout)
		return;

	for (size_t i = 0; i < layout->num_elems; i++) {
		const struct layout_elem *e = &layout->elems[i];

		/* Viewport culling */
		if (e->y + e->height < scroll_y)
			continue;
		if (e->y > scroll_y + viewport_h)
			continue;

		switch (e->type) {
		case ELEM_TEXT: {
			if (!e->text_source)
				break;
			gs_matrix_push();
			gs_matrix_translate3f(e->x, e->y, 0.0f);
			obs_source_video_render(e->text_source);
			gs_matrix_pop();
			break;
		}
		case ELEM_IMAGE: {
			if (!e->image ||
			    !e->image->image2.image.texture)
				break;
			gs_effect_t *effect = obs_get_base_effect(
				OBS_EFFECT_DEFAULT);
			gs_eparam_t *param = gs_effect_get_param_by_name(
				effect, "image");
			gs_effect_set_texture(
				param, e->image->image2.image.texture);
			gs_matrix_push();
			gs_matrix_translate3f(e->x, e->y, 0.0f);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(
					e->image->image2.image.texture,
					0, e->image_width, e->image_height);
			gs_matrix_pop();
			break;
		}
		case ELEM_SPACER:
			break;
		}
	}
}

void credits_renderer_free(struct credits_layout *layout)
{
	if (!layout)
		return;

	obs_enter_graphics();
	for (size_t i = 0; i < layout->num_elems; i++) {
		struct layout_elem *e = &layout->elems[i];
		if (e->text_source) {
			obs_source_release(e->text_source);
			e->text_source = NULL;
		}
		if (e->image) {
			gs_image_file3_free(e->image);
			bfree(e->image);
			e->image = NULL;
		}
	}
	obs_leave_graphics();

	bfree(layout->elems);
	bfree(layout);
}
