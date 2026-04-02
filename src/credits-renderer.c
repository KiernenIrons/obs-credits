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
	int align; /* 0=center, 1=left, 2=right */
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
	float viewport_width;
};

/*
 * make_text_source - Create a private text source for rendering.
 *
 * font_flags bitmask: Bold=1, Italic=2, Underline=4
 * color: ABGR with alpha already included
 * align: "left", "center", or "right"
 * viewport_width: used for extents so text aligns within full width
 */
static obs_source_t *make_text_source(const char *name, const char *text,
				      const char *font_face, int font_size,
				      uint32_t font_flags, uint32_t color,
				      bool outline_enabled, int outline_size,
				      uint32_t outline_color)
{
	if (!font_face)
		font_face = "Arial";

	obs_data_t *font_data = obs_data_create();
	obs_data_set_string(font_data, "face", font_face);
	obs_data_set_int(font_data, "size", font_size);

	char style_str[64] = "";
	int flags_int = (int)font_flags;

	if (font_flags & 1)
		strncat(style_str, "Bold ",
			sizeof(style_str) - strlen(style_str) - 1);
	if (font_flags & 2)
		strncat(style_str, "Italic ",
			sizeof(style_str) - strlen(style_str) - 1);

	size_t slen = strlen(style_str);
	if (slen > 0 && style_str[slen - 1] == ' ')
		style_str[slen - 1] = '\0';

	obs_data_set_int(font_data, "flags", flags_int);
	obs_data_set_string(font_data, "style",
			    style_str[0] ? style_str : "Regular");

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", text ? text : "");
	obs_data_set_obj(settings, "font", font_data);

	/* text_gdiplus_v3 uses "color" (RGB, 0xRRGGBB format).
	 * obs_properties_add_color returns RGB. Pass through directly. */
	obs_data_set_int(settings, "color", (long long)(color & 0xFFFFFF));
	obs_data_set_int(settings, "opacity", 100);
	obs_data_set_bool(settings, "gradient", false);

	/* No extents - text renders at natural size, no clipping.
	 * Alignment is handled by x positioning in the layout. */

	/* Outline */
	obs_data_set_bool(settings, "outline", outline_enabled);
	if (outline_enabled) {
		obs_data_set_int(settings, "outline_size", outline_size);
		obs_data_set_int(settings, "outline_color",
				 (long long)(outline_color & 0xFFFFFF));
		obs_data_set_int(settings, "outline_opacity", 100);
	}

#ifdef _WIN32
	const char *text_source_id = "text_gdiplus_v3";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	obs_source_t *source =
		obs_source_create_private(text_source_id, name, settings);

	/* Force the source to process its settings and calculate dimensions.
	 * Without this, obs_source_get_width/height return 0 because
	 * the source hasn't processed its text yet. */
	if (source)
		obs_source_update(source, settings);

	obs_data_release(settings);
	obs_data_release(font_data);

	return source;
}

static float text_source_height(obs_source_t *source, int font_size)
{
	uint32_t h = obs_source_get_height(source);
	return h > 0 ? (float)h : (float)font_size * 1.5f;
}

static float text_source_width(obs_source_t *source, int font_size,
			       const char *text)
{
	uint32_t w = obs_source_get_width(source);
	if (w > 0)
		return (float)w;
	/* Estimate: ~0.6 * font_size per character */
	size_t len = text ? strlen(text) : 5;
	if (len == 0)
		len = 1;
	return (float)font_size * 0.6f * (float)len;
}

static int align_from_string(const char *align)
{
	if (!align || strcmp(align, "center") == 0)
		return 0;
	if (strcmp(align, "left") == 0)
		return 1;
	if (strcmp(align, "right") == 0)
		return 2;
	return 0;
}

static float compute_x_live(int align, float vw, obs_source_t *source)
{
	float w = (float)obs_source_get_width(source);
	if (w <= 0.0f)
		return 0.0f; /* not yet rendered, left-align as fallback */
	switch (align) {
	case 1:
		return 0.0f; /* left */
	case 2:
		return vw - w; /* right */
	default:
		return (vw - w) / 2.0f; /* center */
	}
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

	/* Spacing: 0 = auto-calculate, any non-zero value used directly */
	float auto_entry = calc_entry_gap(default_font_size);
	float auto_section = calc_section_gap(default_font_size);

	float heading_gap = style->heading_spacing != 0.0f
				    ? style->heading_spacing
				    : auto_entry * 2.0f;
	float sub_gap = style->sub_spacing != 0.0f
				? style->sub_spacing
				: auto_entry;
	float entry_gap = style->entry_spacing != 0.0f
				  ? style->entry_spacing
				  : auto_entry;
	float section_gap = style->section_spacing != 0.0f
				    ? style->section_spacing
				    : auto_section;

	for (size_t s = 0; s < data->num_sections; s++) {
		const struct credits_section *section = &data->sections[s];

		const char *section_align = section->alignment
						    ? section->alignment
						    : "center";

		/* --- Heading --- */
		const char *h_font = section->heading_face
					     ? section->heading_face
					     : (section->heading_font
							? section->heading_font
							: default_font);
		int h_size = section->heading_size > 0
				     ? section->heading_size
				     : (section->heading_font_size > 0
						? section->heading_font_size
						: default_font_size);
		uint32_t h_color = section->heading_color != 0
					   ? section->heading_color
					   : style->heading_color;
		uint32_t h_flags = section->heading_flags;

		const char *heading_text =
			section->heading ? section->heading : "";

		snprintf(name_buf, sizeof(name_buf), "credits_heading_%zu", s);

		int section_align_id = align_from_string(section_align);

		struct layout_elem *he = &layout->elems[elem_idx++];
		he->type = ELEM_TEXT;
		he->align = section_align_id;
		he->text_source = make_text_source(
			name_buf, heading_text, h_font, h_size, h_flags,
			h_color, style->outline_enabled, style->outline_size,
			style->outline_color);
		he->height = text_source_height(he->text_source, h_size);
		he->x = 0.0f;
		he->y = y_cursor;

		if (style->shadow_enabled) {
			char sname[128];
			snprintf(sname, sizeof(sname),
				 "credits_heading_%zu_sh", s);
			he->shadow_source = make_text_source(
				sname, heading_text, h_font, h_size,
				h_flags, style->shadow_color, false, 0, 0);
			he->shadow_off_x = style->shadow_offset_x;
			he->shadow_off_y = style->shadow_offset_y;
		}

		y_cursor += he->height + heading_gap;

		/* --- Entries --- */
		bool first_entry = true;
		for (size_t e = 0; e < section->num_entries; e++) {
			const struct credits_entry *entry =
				&section->entries[e];
			struct layout_elem *le = &layout->elems[elem_idx++];
			const char *entry_text = NULL;
			char combined[512];

			/* Determine font face, size, and flags based on entry type */
			int e_size;
			uint32_t e_flags;
			const char *e_font;

			switch (entry->type) {
			case CREDITS_ENTRY_NAME_ROLE:
				snprintf(combined, sizeof(combined),
					 "%s  -  %s",
					 entry->name ? entry->name : "",
					 entry->role ? entry->role : "");
				entry_text = combined;
				e_font = section->entry_face
						 ? section->entry_face
						 : default_font;
				e_size = section->entry_size > 0
						 ? section->entry_size
						 : default_font_size;
				e_flags = section->entry_flags;
				break;
			case CREDITS_ENTRY_NAME_ONLY:
				entry_text = entry->name ? entry->name : "";
				e_font = section->entry_face
						 ? section->entry_face
						 : default_font;
				e_size = section->entry_size > 0
						 ? section->entry_size
						 : default_font_size;
				e_flags = section->entry_flags;
				break;
			case CREDITS_ENTRY_TEXT:
				entry_text = entry->text ? entry->text : "";
				/* Subheading is the first TEXT entry */
				if (first_entry) {
					e_font = section->sub_face
							 ? section->sub_face
							 : default_font;
					e_size = section->sub_size > 0
							 ? section->sub_size
							 : default_font_size;
					e_flags = section->sub_flags;
				} else {
					e_font = section->entry_face
							 ? section->entry_face
							 : default_font;
					e_size = section->entry_size > 0
							 ? section->entry_size
							 : default_font_size;
					e_flags = section->entry_flags;
				}
				break;
			case CREDITS_ENTRY_IMAGE:
				le->type = ELEM_IMAGE;
				le->image = bzalloc(sizeof(gs_image_file3_t));
				if (entry->image_path) {
					gs_image_file3_init(
						le->image, entry->image_path,
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
				le->x = 0.0f;
				le->y = y_cursor;
				y_cursor += le->height + entry_gap;
				first_entry = false;
				continue;
			case CREDITS_ENTRY_SPACER:
				le->type = ELEM_SPACER;
				le->height = (float)entry->spacer_height;
				le->x = 0.0f;
				le->y = y_cursor;
				y_cursor += le->height + entry_gap;
				first_entry = false;
				continue;
			default:
				e_size = default_font_size;
				e_flags = 0;
				break;
			}

			/* Text entry (NAME_ROLE, NAME_ONLY, TEXT) */
			snprintf(name_buf, sizeof(name_buf),
				 "credits_entry_%zu_%zu", s, e);
			le->type = ELEM_TEXT;
			le->align = section_align_id;
			le->text_source = make_text_source(
				name_buf, entry_text, e_font,
				e_size, e_flags,
				style->text_color, style->outline_enabled,
				style->outline_size, style->outline_color);
			le->height = text_source_height(le->text_source,
							e_size);
			le->x = 0.0f;
			le->y = y_cursor;

			if (style->shadow_enabled) {
				char sname[128];
				snprintf(sname, sizeof(sname),
					 "credits_sh_%zu_%zu", s, e);
				le->shadow_source = make_text_source(
					sname, entry_text, e_font,
					e_size, e_flags,
					style->shadow_color, false, 0, 0);
				le->shadow_off_x = style->shadow_offset_x;
				le->shadow_off_y = style->shadow_offset_y;
			}

			/* Use sub_gap after subheading, entry_gap for everything else */
			y_cursor += le->height +
				    (first_entry ? sub_gap : entry_gap);
			first_entry = false;
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
	layout->viewport_width = vw;

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

		if (e->y + e->height < scroll_y)
			continue;
		if (e->y > scroll_y + viewport_h)
			continue;

		switch (e->type) {
		case ELEM_TEXT: {
			if (!e->text_source)
				break;

			/* Compute x from real source width every frame.
			 * At build time width is 0; by draw time the
			 * source has rendered and reports its true width. */
			float x = compute_x_live(e->align,
						 layout->viewport_width,
						 e->text_source);

			if (e->shadow_source) {
				gs_matrix_push();
				gs_matrix_translate3f(
					x + e->shadow_off_x,
					e->y + e->shadow_off_y, 0.0f);
				obs_source_video_render(e->shadow_source);
				gs_matrix_pop();
			}

			gs_matrix_push();
			gs_matrix_translate3f(x, e->y, 0.0f);
			obs_source_video_render(e->text_source);
			gs_matrix_pop();
			break;
		}
		case ELEM_IMAGE: {
			if (!e->image || !e->image->image2.image.texture)
				break;
			gs_effect_t *effect =
				obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *param = gs_effect_get_param_by_name(
				effect, "image");
			gs_effect_set_texture(
				param, e->image->image2.image.texture);
			gs_matrix_push();
			gs_matrix_translate3f(e->x, e->y, 0.0f);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(e->image->image2.image.texture,
					       0, e->image_width,
					       e->image_height);
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
