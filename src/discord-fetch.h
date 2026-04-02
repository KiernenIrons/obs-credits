#pragma once

#include <stddef.h>
#include <stdbool.h>

/* A single Discord member entry */
struct discord_member {
	char *display_name; /* nick if set, else global_name, else username */
	char *avatar_url;   /* CDN URL or NULL */
	bool is_booster;
};

#define DISCORD_MAX_ROLES 5

/* Members belonging to one role */
struct discord_role_result {
	struct discord_member *members;
	size_t num_members;
};

/* Result of a Discord fetch */
struct discord_result {
	struct discord_member *boosters;
	size_t num_boosters;
	struct discord_role_result roles[DISCORD_MAX_ROLES];
	char *error; /* NULL on success, error message on failure */
};

/* Fetch Discord guild data. Runs HTTP requests (call from background thread).
 * bot_token : Discord bot token
 * guild_id  : Discord server/guild ID
 * role_ids  : Array of up to DISCORD_MAX_ROLES role ID strings (may be NULL
 *             or empty string for unused slots)
 * Returns a discord_result (caller must free with discord_result_free). */
struct discord_result *discord_fetch(const char *bot_token,
				     const char *guild_id,
				     const char *role_ids[DISCORD_MAX_ROLES]);

void discord_result_free(struct discord_result *result);
