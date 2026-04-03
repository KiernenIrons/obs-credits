#pragma once

#include <stdint.h>
#include <stdbool.h>

struct credits_data;
struct credits_layout;

struct credits_layout *credits_renderer_build(
	const struct credits_data *data, uint32_t viewport_width,
	const char *fallback_font, int fallback_font_size);

float credits_renderer_total_height(const struct credits_layout *layout);

void credits_renderer_draw(const struct credits_layout *layout,
			   uint32_t viewport_width, float scroll_y,
			   float viewport_h);

void credits_renderer_free(struct credits_layout *layout);
