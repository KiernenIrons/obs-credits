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
