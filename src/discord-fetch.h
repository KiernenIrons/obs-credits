#pragma once

#include <stddef.h>
#include <stdbool.h>

/* A single Discord member entry */
struct discord_member {
	char *display_name; /* nick if set, else global_name, else username */
	char *avatar_url;   /* CDN URL or NULL */
	bool is_booster;
};

/* Result of a Discord fetch */
struct discord_result {
	struct discord_member *boosters;
	size_t num_boosters;
	struct discord_member *role_members; /* members matching configured role */
	size_t num_role_members;
	char *error; /* NULL on success, error message on failure */
};

/* Fetch Discord guild data. Runs HTTP requests (call from background thread).
 * bot_token: Discord bot token
 * guild_id: Discord server/guild ID
 * role_id: Role ID to filter (mods/VIPs etc), or NULL to skip
 * Returns a discord_result (caller must free with discord_result_free). */
struct discord_result *discord_fetch(const char *bot_token,
				     const char *guild_id,
				     const char *role_id);

void discord_result_free(struct discord_result *result);
