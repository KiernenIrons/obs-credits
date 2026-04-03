#pragma once

#include <stddef.h>
#include <stdbool.h>

/* A single Discord member entry */
struct discord_member {
	char *display_name; /* nick if set, else global_name, else username */
	char *avatar_url;   /* CDN URL or NULL */
	bool is_booster;
};

#define MAX_DISCORD_SECTIONS 10

/* Configuration for a single Discord section fetch */
struct discord_fetch_config {
	bool is_booster; /* true = fetch boosters, false = fetch by role_id */
	char *role_id;   /* only used if is_booster=false */
};

/* Members belonging to one section */
struct discord_section_result {
	struct discord_member *members;
	size_t num_members;
};

/* Result of a Discord fetch */
struct discord_result {
	struct discord_section_result sections[MAX_DISCORD_SECTIONS];
	int num_sections;
	char *error; /* NULL on success, error message on failure */
};

/* Fetch Discord guild data. Runs HTTP requests (call from background thread).
 * bot_token   : Discord bot token
 * guild_id    : Discord server/guild ID
 * configs     : Array of fetch configs (one per Discord section)
 * num_configs : Number of configs (up to MAX_DISCORD_SECTIONS)
 * Returns a discord_result (caller must free with discord_result_free). */
struct discord_result *discord_fetch(const char *bot_token,
				     const char *guild_id,
				     const struct discord_fetch_config *configs,
				     int num_configs);

void discord_result_free(struct discord_result *result);
