#include "credits-renderer.h"
#include "credits-parser.h"

#include <obs-module.h>
#include <graphics/image-file.h>

#include <stdio.h>
#include <string.h>

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
	obs_source_t *shadow_source;
	float shadow_off_x;
	float shadow_off_y;
	gs_image_file3_t *image;
	uint32_t image_width;
	uint32_t image_height;
};

struct credits_layout {
	struct layout_elem *elems;
	size_t num_elems;
	float total_height;
};

/*
 * make_text_source - Create a private text source for rendering.
 *
 * font_flags bitmask: Bold=1, Italic=2, Underline=4
 * color: ABGR format with alpha (e.g. 0xFF00D7FF for gold)
 * align: "left", "center", or "right"
 */
static obs_source_t *make_text_source(const char *name, const char *text,
				      const char *font_face, int font_size,
				      uint32_t font_flags, uint32_t color,
				      const char *align, bool outline_enabled,
				      int outline_size, uint32_t outline_color)
{
	if (!font_face)
		font_face = "Arial";
	if (!align)
		align = "center";

	/* Build font object */
	obs_data_t *font_data = obs_data_create();
	obs_data_set_string(font_data, "face", font_face);
	obs_data_set_int(font_data, "size", font_size);

	/* Build style string for text_gdiplus_v2 */
	char style_str[64] = "";
	int flags_int = 0;

	if (font_flags & 1) {  /* Bold */
		flags_int |= 1;
		strncat(style_str, "Bold ", sizeof(style_str) - strlen(style_str) - 1);
	}
	if (font_flags & 2) {  /* Italic */
		flags_int |= 2;
		strncat(style_str, "Italic ", sizeof(style_str) - strlen(style_str) - 1);
	}
	if (font_flags & 4) {  /* Underline */
		flags_int |= 4;
	}

	/* Trim trailing space */
	size_t slen = strlen(style_str);
	if (slen > 0 && style_str[slen - 1] == ' ')
		style_str[slen - 1] = '\0';

	obs_data_set_int(font_data, "flags", flags_int);
	obs_data_set_string(font_data, "style",
			    style_str[0] ? style_str : "Regular");

	/* Build source settings */
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text ? text : "");
	obs_data_set_obj(settings, "font", font_data);

	/* Ensure alpha is set on color (ABGR with 0xFF alpha) */
	uint32_t color_abgr = color | 0xFF000000;

	obs_data_set_int(settings, "color1", color_abgr);
	obs_data_set_int(settings, "color2", color_abgr);
	obs_data_set_bool(settings, "gradient", false);

	/* Alignment: "left" -> "left", "center" -> "center", "right" -> "right" */
	obs_data_set_string(settings, "align", align);

	/* Outline */
	obs_data_set_bool(settings, "outline", outline_enabled);
	if (outline_enabled) {
		obs_data_set_int(settings, "outline_size", outline_size);
		obs_data_set_int(settings, "outline_color",
				 outline_color | 0xFF000000);
		obs_data_set_int(settings, "outline_opacity", 100);
	}

#ifdef _WIN32
	const char *text_source_id = "text_gdiplus_v2";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	obs_source_t *source =
		obs_source_create_private(text_source_id, name, settings);

	obs_data_release(settings);
	obs_data_release(font_data);

	return source;
}

static float text_source_height(obs_source_t *source, int font_size)
{
	uint32_t h = obs_source_get_height(source);
	return h > 0 ? (float)h : (float)font_size * 1.5f;
}

static float text_source_width(obs_source_t *source, int font_size)
{
	uint32_t w = obs_source_get_width(source);
	return w > 0 ? (float)w : (float)font_size * 4.0f;
}

static float calc_entry_gap(int font_size)
{
	float gap = (float)font_size * 0.3f;
	return gap > 8.0f ? gap : 8.0f;
}

static float calc_section_gap(int font_size)
{
	float gap = (float)font_size * 2.0f;
	return gap > 60.0f ? gap : 60.0f;
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

/*
 * Compute x position based on alignment, viewport width, and element width.
 */
static float compute_x(const char *align, float viewport_w, float elem_w)
{
	if (!align || strcmp(align, "center") == 0)
		return (viewport_w - elem_w) / 2.0f;
	if (strcmp(align, "right") == 0)
		return viewport_w - elem_w;
	/* "left" */
	return 0.0f;
}

struct credits_layout *credits_renderer_build(
	const struct credits_data *data, uint32_t viewport_width,
	const struct credits_style *style)
{
	if (!data || data->num_sections == 0 || !style)
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

	const char *default_font = style->default_font ? style->default_font
						       : "Arial";
	int default_font_size = style->default_font_size > 0
					? style->default_font_size
					: 32;

	for (size_t s = 0; s < data->num_sections; s++) {
		const struct credits_section *section = &data->sections[s];

		/* Determine section alignment */
		const char *section_align = section->alignment
						    ? section->alignment
						    : "center";

		/* Section font flags */
		uint32_t section_flags = section->font_flags;

		/* Entry gap / section gap scaled to font size */
		float entry_gap = calc_entry_gap(default_font_size);
		float section_gap = calc_section_gap(default_font_size);

		/* --- Heading --- */
		const char *h_font = section->heading_font
					     ? section->heading_font
					     : default_font;
		int h_size = section->heading_font_size > 0
				     ? section->heading_font_size
				     : default_font_size;
		uint32_t h_color = section->heading_color != 0
					   ? section->heading_color
					   : style->heading_color;

		const char *heading_text =
			section->heading ? section->heading : "";

		snprintf(name_buf, sizeof(name_buf), "credits_heading_%zu", s);

		struct layout_elem *he = &layout->elems[elem_idx++];
		he->type = ELEM_TEXT;
		he->text_source = make_text_source(
			name_buf, heading_text, h_font, h_size, section_flags,
			h_color, section_align, style->outline_enabled,
			style->outline_size, style->outline_color);
		he->height = text_source_height(he->text_source, h_size);

		float h_w = text_source_width(he->text_source, h_size);
		he->x = compute_x(section_align, vw, h_w);
		he->y = y_cursor;

		/* Shadow for heading */
		if (style->shadow_enabled) {
			char shadow_name[128];
			snprintf(shadow_name, sizeof(shadow_name),
				 "credits_heading_%zu_shadow", s);
			he->shadow_source = make_text_source(
				shadow_name, heading_text, h_font, h_size,
				section_flags, style->shadow_color,
				section_align, false, 0, 0);
			he->shadow_off_x = style->shadow_offset_x;
			he->shadow_off_y = style->shadow_offset_y;
		}

		y_cursor += he->height + entry_gap * 2.0f;

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
					default_font_size, section_flags,
					style->text_color, section_align,
					style->outline_enabled,
					style->outline_size,
					style->outline_color);
				le->height = text_source_height(
					le->text_source, default_font_size);
				float ew = text_source_width(le->text_source,
							     default_font_size);
				le->x = compute_x(section_align, vw, ew);
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
					section_flags, style->text_color,
					section_align,
					style->outline_enabled,
					style->outline_size,
					style->outline_color);
				le->height = text_source_height(
					le->text_source, default_font_size);
				float ew = text_source_width(le->text_source,
							     default_font_size);
				le->x = compute_x(section_align, vw, ew);
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
					section_flags, style->text_color,
					section_align,
					style->outline_enabled,
					style->outline_size,
					style->outline_color);
				le->height = text_source_height(
					le->text_source, default_font_size);
				float ew = text_source_width(le->text_source,
							     default_font_size);
				le->x = compute_x(section_align, vw, ew);
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
						: le->image->image2.image.cx;
				le->image_height =
					entry->image_height > 0
						? (uint32_t)entry->image_height
						: le->image->image2.image.cy;
				le->height = (float)le->image_height;
				le->x = compute_x(section_align, vw,
						   (float)le->image_width);
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

			/* Shadow for text entries */
			if (le->type == ELEM_TEXT && style->shadow_enabled &&
			    le->text_source) {
				char shadow_name[128];
				snprintf(shadow_name, sizeof(shadow_name),
					 "credits_shadow_%zu_%zu", s, e);
				const char *shadow_text = NULL;
				switch (entry->type) {
				case CREDITS_ENTRY_NAME_ROLE: {
					char combined[512];
					snprintf(combined, sizeof(combined),
						 "%s  -  %s",
						 entry->name ? entry->name
							     : "",
						 entry->role ? entry->role
							     : "");
					shadow_text = combined;
					le->shadow_source = make_text_source(
						shadow_name, shadow_text,
						default_font,
						default_font_size,
						section_flags,
						style->shadow_color,
						section_align, false, 0, 0);
					break;
				}
				case CREDITS_ENTRY_NAME_ONLY:
					le->shadow_source = make_text_source(
						shadow_name,
						entry->name ? entry->name : "",
						default_font,
						default_font_size,
						section_flags,
						style->shadow_color,
						section_align, false, 0, 0);
					break;
				case CREDITS_ENTRY_TEXT:
					le->shadow_source = make_text_source(
						shadow_name,
						entry->text ? entry->text : "",
						default_font,
						default_font_size,
						section_flags,
						style->shadow_color,
						section_align, false, 0, 0);
					break;
				default:
					break;
				}
				le->shadow_off_x = style->shadow_offset_x;
				le->shadow_off_y = style->shadow_offset_y;
			}

			y_cursor += le->height + entry_gap;
		}

		/* --- Section gap spacer --- */
		struct layout_elem *gap = &layout->elems[elem_idx++];
		gap->type = ELEM_SPACER;
		gap->y = y_cursor;
		gap->height = section_gap;
		gap->x = 0.0f;
		y_cursor += section_gap;
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

			/* Render shadow first if present */
			if (e->shadow_source) {
				gs_matrix_push();
				gs_matrix_translate3f(
					e->x + e->shadow_off_x,
					e->y + e->shadow_off_y, 0.0f);
				obs_source_video_render(e->shadow_source);
				gs_matrix_pop();
			}

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
		if (e->shadow_source) {
			obs_source_release(e->shadow_source);
			e->shadow_source = NULL;
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
