#pragma once

#include <obs-data.h>
#include <stdint.h>
#include <stddef.h>

enum credits_entry_type {
	CREDITS_ENTRY_NAME_ROLE,
	CREDITS_ENTRY_NAME_ONLY,
	CREDITS_ENTRY_IMAGE,
	CREDITS_ENTRY_SPACER,
	CREDITS_ENTRY_TEXT,
};

struct credits_entry {
	enum credits_entry_type type;
	char *name;
	char *role;
	char *text;
	char *image_path;
	int image_width;
	int image_height;
	int spacer_height;
};

struct credits_section {
	char *heading;
	char *heading_font;
	int heading_font_size;
	uint32_t heading_color;
	char *alignment;

	/* Per-field font size overrides (0 = use default) */
	int heading_size;
	uint32_t heading_flags; /* Bold=1, Italic=2, Underline=4 */
	int sub_size;
	uint32_t sub_flags;
	int entry_size;
	uint32_t entry_flags;

	struct credits_entry *entries;
	size_t num_entries;
};

struct credits_data {
	struct credits_section *sections;
	size_t num_sections;
};

struct credits_data *credits_parse_file(const char *path);
struct credits_data *credits_build_from_settings(obs_data_t *settings);
void credits_data_free(struct credits_data *data);
