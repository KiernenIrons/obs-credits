#include "credits-parser.h"

#include <obs-module.h>
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (len <= 0) {
		fclose(f);
		return NULL;
	}

	char *buf = bmalloc((size_t)len + 1);
	size_t read = fread(buf, 1, (size_t)len, f);
	fclose(f);

	buf[read] = '\0';
	return buf;
}

static enum credits_entry_type parse_entry_type(const char *str)
{
	if (!str)
		return CREDITS_ENTRY_NAME_ONLY;
	if (strcmp(str, "name_role") == 0)
		return CREDITS_ENTRY_NAME_ROLE;
	if (strcmp(str, "name_only") == 0)
		return CREDITS_ENTRY_NAME_ONLY;
	if (strcmp(str, "image") == 0)
		return CREDITS_ENTRY_IMAGE;
	if (strcmp(str, "spacer") == 0)
		return CREDITS_ENTRY_SPACER;
	if (strcmp(str, "text") == 0)
		return CREDITS_ENTRY_TEXT;
	return CREDITS_ENTRY_NAME_ONLY;
}

static void parse_entry(const cJSON *json, struct credits_entry *entry)
{
	const cJSON *type_item = cJSON_GetObjectItem(json, "type");
	entry->type = parse_entry_type(
		cJSON_IsString(type_item) ? type_item->valuestring : NULL);

	const cJSON *name_item = cJSON_GetObjectItem(json, "name");
	if (cJSON_IsString(name_item))
		entry->name = bstrdup(name_item->valuestring);

	const cJSON *role_item = cJSON_GetObjectItem(json, "role");
	if (cJSON_IsString(role_item))
		entry->role = bstrdup(role_item->valuestring);

	const cJSON *text_item = cJSON_GetObjectItem(json, "text");
	if (cJSON_IsString(text_item))
		entry->text = bstrdup(text_item->valuestring);

	const cJSON *image_item = cJSON_GetObjectItem(json, "image_path");
	if (cJSON_IsString(image_item))
		entry->image_path = bstrdup(image_item->valuestring);

	const cJSON *iw_item = cJSON_GetObjectItem(json, "image_width");
	if (cJSON_IsNumber(iw_item))
		entry->image_width = iw_item->valueint;

	const cJSON *ih_item = cJSON_GetObjectItem(json, "image_height");
	if (cJSON_IsNumber(ih_item))
		entry->image_height = ih_item->valueint;

	const cJSON *sh_item = cJSON_GetObjectItem(json, "height");
	if (cJSON_IsNumber(sh_item))
		entry->spacer_height = sh_item->valueint;
	else
		entry->spacer_height = 40;
}

static uint32_t parse_font_flags_json(const cJSON *json,
				      const char *bold_key,
				      const char *italic_key,
				      const char *underline_key)
{
	uint32_t flags = 0;
	const cJSON *bold_item = cJSON_GetObjectItem(json, bold_key);
	const cJSON *italic_item = cJSON_GetObjectItem(json, italic_key);
	const cJSON *underline_item = cJSON_GetObjectItem(json, underline_key);

	if (cJSON_IsBool(bold_item) && cJSON_IsTrue(bold_item))
		flags |= 1;
	if (cJSON_IsBool(italic_item) && cJSON_IsTrue(italic_item))
		flags |= 2;
	if (cJSON_IsBool(underline_item) && cJSON_IsTrue(underline_item))
		flags |= 4;
	return flags;
}

static void parse_section(const cJSON *json, struct credits_section *section)
{
	const cJSON *heading_item = cJSON_GetObjectItem(json, "heading");
	if (cJSON_IsString(heading_item))
		section->heading = bstrdup(heading_item->valuestring);

	const cJSON *align_item = cJSON_GetObjectItem(json, "alignment");
	if (cJSON_IsString(align_item))
		section->alignment = bstrdup(align_item->valuestring);

	const cJSON *style = cJSON_GetObjectItem(json, "heading_style");
	if (cJSON_IsObject(style)) {
		const cJSON *font_item = cJSON_GetObjectItem(style, "font");
		if (cJSON_IsString(font_item))
			section->heading_font =
				bstrdup(font_item->valuestring);

		const cJSON *size_item = cJSON_GetObjectItem(style, "size");
		if (cJSON_IsNumber(size_item))
			section->heading_font_size = size_item->valueint;

		const cJSON *color_item =
			cJSON_GetObjectItem(style, "color");
		if (cJSON_IsString(color_item) &&
		    color_item->valuestring[0] == '#') {
			section->heading_color = (uint32_t)strtoul(
				color_item->valuestring + 1, NULL, 16);
		}
	}

	/* Parse per-field font flags from JSON */
	section->heading_flags = parse_font_flags_json(
		json, "heading_bold", "heading_italic", "heading_underline");
	section->sub_flags = parse_font_flags_json(
		json, "sub_bold", "sub_italic", "sub_underline");
	section->entry_flags = parse_font_flags_json(
		json, "entry_bold", "entry_italic", "entry_underline");

	/* Parse per-field font sizes */
	const cJSON *hs = cJSON_GetObjectItem(json, "heading_size");
	if (cJSON_IsNumber(hs))
		section->heading_size = hs->valueint;

	const cJSON *ss = cJSON_GetObjectItem(json, "sub_size");
	if (cJSON_IsNumber(ss))
		section->sub_size = ss->valueint;

	const cJSON *es = cJSON_GetObjectItem(json, "entry_size");
	if (cJSON_IsNumber(es))
		section->entry_size = es->valueint;

	const cJSON *entries_arr = cJSON_GetObjectItem(json, "entries");
	if (cJSON_IsArray(entries_arr)) {
		int count = cJSON_GetArraySize(entries_arr);
		if (count > 0) {
			section->num_entries = (size_t)count;
			section->entries = bzalloc(sizeof(struct credits_entry) *
						   section->num_entries);

			size_t i = 0;
			const cJSON *entry_json = NULL;
			cJSON_ArrayForEach(entry_json, entries_arr)
			{
				parse_entry(entry_json,
					    &section->entries[i]);
				i++;
			}
		}
	}
}

struct credits_data *credits_parse_file(const char *path)
{
	if (!path)
		return NULL;

	char *content = read_file(path);
	if (!content) {
		blog(LOG_WARNING,
		     "[obs-credits] Failed to read credits file: %s", path);
		return NULL;
	}

	cJSON *root = cJSON_Parse(content);
	bfree(content);

	if (!root) {
		blog(LOG_WARNING,
		     "[obs-credits] Failed to parse JSON from: %s", path);
		return NULL;
	}

	const cJSON *sections_arr = cJSON_GetObjectItem(root, "sections");
	if (!cJSON_IsArray(sections_arr)) {
		blog(LOG_WARNING,
		     "[obs-credits] No 'sections' array found in: %s", path);
		cJSON_Delete(root);
		return NULL;
	}

	int count = cJSON_GetArraySize(sections_arr);
	if (count <= 0) {
		blog(LOG_WARNING,
		     "[obs-credits] Empty sections array in: %s", path);
		cJSON_Delete(root);
		return NULL;
	}

	struct credits_data *data = bzalloc(sizeof(struct credits_data));
	data->num_sections = (size_t)count;
	data->sections =
		bzalloc(sizeof(struct credits_section) * data->num_sections);

	size_t i = 0;
	const cJSON *section_json = NULL;
	cJSON_ArrayForEach(section_json, sections_arr)
	{
		parse_section(section_json, &data->sections[i]);
		i++;
	}

	cJSON_Delete(root);

	blog(LOG_INFO,
	     "[obs-credits] Parsed %zu section(s) from: %s",
	     data->num_sections, path);

	return data;
}

static size_t count_lines(const char *text)
{
	if (!text || text[0] == '\0')
		return 0;
	size_t count = 1;
	for (const char *p = text; *p; p++) {
		if (*p == '\n')
			count++;
	}
	return count;
}

static char *get_line(const char *text, size_t line_idx)
{
	if (!text)
		return NULL;

	const char *start = text;
	for (size_t i = 0; i < line_idx && *start; i++) {
		while (*start && *start != '\n')
			start++;
		if (*start == '\n')
			start++;
	}

	if (*start == '\0')
		return NULL;

	const char *end = start;
	while (*end && *end != '\n')
		end++;

	/* Trim trailing whitespace */
	while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\r'))
		end--;

	if (end == start)
		return NULL;

	size_t len = (size_t)(end - start);
	char *line = bmalloc(len + 1);
	memcpy(line, start, len);
	line[len] = '\0';
	return line;
}

static uint32_t build_flags_from_bools(obs_data_t *sec,
					const char *bold_key,
					const char *italic_key,
					const char *underline_key)
{
	uint32_t flags = 0;
	if (obs_data_get_bool(sec, bold_key))
		flags |= 1;
	if (obs_data_get_bool(sec, italic_key))
		flags |= 2;
	if (obs_data_get_bool(sec, underline_key))
		flags |= 4;
	return flags;
}

struct credits_data *credits_build_from_settings(obs_data_t *settings)
{
	if (!settings)
		return NULL;

	obs_data_array_t *arr = obs_data_get_array(settings, "sections");
	if (!arr)
		return NULL;

	size_t num_sections = obs_data_array_count(arr);
	if (num_sections == 0) {
		obs_data_array_release(arr);
		return NULL;
	}

	struct credits_data *data = bzalloc(sizeof(struct credits_data));
	data->num_sections = num_sections;
	data->sections =
		bzalloc(sizeof(struct credits_section) * num_sections);

	for (size_t i = 0; i < num_sections; i++) {
		obs_data_t *sec = obs_data_array_item(arr, i);
		struct credits_section *section = &data->sections[i];

		const char *heading = obs_data_get_string(sec, "heading");
		const char *subheading =
			obs_data_get_string(sec, "subheading");
		const char *names = obs_data_get_string(sec, "names");
		const char *roles = obs_data_get_string(sec, "roles");

		if (heading && heading[0] != '\0')
			section->heading = bstrdup(heading);

		/* Read alignment */
		const char *align = obs_data_get_string(sec, "alignment");
		if (align && align[0] != '\0')
			section->alignment = bstrdup(align);

		/* Read per-field font sizes */
		section->heading_size =
			(int)obs_data_get_int(sec, "heading_size");
		section->sub_size =
			(int)obs_data_get_int(sec, "sub_size");
		section->entry_size =
			(int)obs_data_get_int(sec, "entry_size");

		/* Read per-field font flags */
		section->heading_flags = build_flags_from_bools(
			sec, "heading_bold", "heading_italic",
			"heading_underline");
		section->sub_flags = build_flags_from_bools(
			sec, "sub_bold", "sub_italic", "sub_underline");
		section->entry_flags = build_flags_from_bools(
			sec, "entry_bold", "entry_italic",
			"entry_underline");

		/* Count entries: subheading (if present) + name/role lines */
		size_t name_lines = count_lines(names);
		size_t role_lines = count_lines(roles);
		size_t max_lines =
			name_lines > role_lines ? name_lines : role_lines;
		bool has_sub = subheading && subheading[0] != '\0';

		size_t num_entries = max_lines + (has_sub ? 1 : 0);
		if (num_entries > 0) {
			section->num_entries = num_entries;
			section->entries = bzalloc(
				sizeof(struct credits_entry) * num_entries);

			size_t idx = 0;

			if (has_sub) {
				section->entries[idx].type =
					CREDITS_ENTRY_TEXT;
				section->entries[idx].text =
					bstrdup(subheading);
				idx++;
			}

			for (size_t j = 0; j < max_lines; j++) {
				char *name = get_line(names, j);
				char *role = get_line(roles, j);

				if (name && role) {
					section->entries[idx].type =
						CREDITS_ENTRY_NAME_ROLE;
					section->entries[idx].name = name;
					section->entries[idx].role = role;
				} else if (name) {
					section->entries[idx].type =
						CREDITS_ENTRY_NAME_ONLY;
					section->entries[idx].name = name;
					bfree(role);
				} else {
					section->entries[idx].type =
						CREDITS_ENTRY_NAME_ONLY;
					section->entries[idx].name =
						role ? role : bstrdup("");
				}
				idx++;
			}
		}

		obs_data_release(sec);
	}

	obs_data_array_release(arr);

	blog(LOG_INFO, "[obs-credits] Built %zu section(s) from settings",
	     data->num_sections);

	return data;
}

void credits_data_free(struct credits_data *data)
{
	if (!data)
		return;

	for (size_t i = 0; i < data->num_sections; i++) {
		struct credits_section *section = &data->sections[i];

		for (size_t j = 0; j < section->num_entries; j++) {
			struct credits_entry *entry = &section->entries[j];
			bfree(entry->name);
			bfree(entry->role);
			bfree(entry->text);
			bfree(entry->image_path);
		}

		bfree(section->entries);
		bfree(section->heading);
		bfree(section->heading_font);
		bfree(section->alignment);
	}

	bfree(data->sections);
	bfree(data);
}
