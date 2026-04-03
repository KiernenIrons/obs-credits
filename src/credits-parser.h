#pragma once

#include <obs-data.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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
	char *alignment;

	/* Per-field fonts (from font pickers) */
	char *heading_face;
	int heading_size;
	uint32_t heading_flags;
	char *sub_face;
	int sub_size;
	uint32_t sub_flags;
	char *entry_face;
	int entry_size;
	uint32_t entry_flags;

	/* Per-section colors (RGB 0xRRGGBB) */
	uint32_t heading_color;
	uint32_t sub_color;
	uint32_t text_color;

	/* Per-section outline */
	bool outline_enabled;
	int outline_size;
	uint32_t outline_color;
	bool outline_heading;
	bool outline_sub;
	bool outline_entries;

	/* Per-section shadow */
	bool shadow_enabled;
	uint32_t shadow_color;
	float shadow_offset_x;
	float shadow_offset_y;
	bool shadow_heading;
	bool shadow_sub;
	bool shadow_entries;

	/* Per-section spacing */
	float heading_spacing;  /* heading -> sub, 0=auto */
	float sub_spacing;      /* sub -> entries, 0=auto */
	float entry_spacing;    /* between entries, 0=auto */
	float section_spacing;  /* gap after this section, 0=auto */

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
