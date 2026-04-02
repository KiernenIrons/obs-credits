#pragma once

#include <stdint.h>
#include <stdbool.h>

struct credits_data;
struct credits_layout;

struct credits_style {
	const char *default_font;
	int default_font_size;
	uint32_t heading_color;
	uint32_t text_color;
	bool outline_enabled;
	int outline_size;
	uint32_t outline_color;
	bool shadow_enabled;
	uint32_t shadow_color;
	float shadow_offset_x;
	float shadow_offset_y;
};

struct credits_layout *credits_renderer_build(
	const struct credits_data *data, uint32_t viewport_width,
	const struct credits_style *style);

float credits_renderer_total_height(const struct credits_layout *layout);

void credits_renderer_draw(const struct credits_layout *layout,
			   uint32_t viewport_width, float scroll_y,
			   float viewport_h);

void credits_renderer_free(struct credits_layout *layout);
