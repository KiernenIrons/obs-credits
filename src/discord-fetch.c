#include "discord-fetch.h"

#include <obs-module.h>
#include <curl/curl.h>
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#define DISCORD_API_BASE "https://discord.com/api/v10"

/* ---- HTTP response buffer ---- */

struct http_buf {
	char *data;
	size_t size;
};

static size_t http_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
	struct http_buf *buf = ud;
	size_t total = size * nmemb;
	char *new_data = brealloc(buf->data, buf->size + total + 1);
	if (!new_data)
		return 0;
	buf->data = new_data;
	memcpy(buf->data + buf->size, ptr, total);
	buf->size += total;
	buf->data[buf->size] = '\0';
	return total;
}

/* ---- HTTP GET with Discord bot auth ---- */

static cJSON *discord_get(CURL *curl, const char *url,
			  struct curl_slist *headers, char **out_error)
{
	struct http_buf buf = {NULL, 0};

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

#ifdef _WIN32
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
			 CURLSSLOPT_REVOKE_BEST_EFFORT);
#endif

	CURLcode res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		if (out_error)
			*out_error = bstrdup(curl_easy_strerror(res));
		bfree(buf.data);
		return NULL;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 200) {
		if (out_error) {
			char err[256];
			snprintf(err, sizeof(err),
				 "Discord API returned HTTP %ld", http_code);
			*out_error = bstrdup(err);
		}
		bfree(buf.data);
		return NULL;
	}

	cJSON *json = cJSON_Parse(buf.data);
	bfree(buf.data);

	if (!json && out_error)
		*out_error = bstrdup("Failed to parse Discord API response");

	return json;
}

/* ---- Get display name from member JSON ---- */

static char *get_display_name(const cJSON *member)
{
	/* Priority: nick > user.global_name > user.username */
	const cJSON *nick = cJSON_GetObjectItem(member, "nick");
	if (cJSON_IsString(nick) && nick->valuestring[0] != '\0')
		return bstrdup(nick->valuestring);

	const cJSON *user = cJSON_GetObjectItem(member, "user");
	if (!cJSON_IsObject(user))
		return bstrdup("Unknown");

	const cJSON *global = cJSON_GetObjectItem(user, "global_name");
	if (cJSON_IsString(global) && global->valuestring[0] != '\0')
		return bstrdup(global->valuestring);

	const cJSON *uname = cJSON_GetObjectItem(user, "username");
	if (cJSON_IsString(uname))
		return bstrdup(uname->valuestring);

	return bstrdup("Unknown");
}

/* ---- Get avatar URL from member JSON ---- */

static char *get_avatar_url(const cJSON *member)
{
	const cJSON *user = cJSON_GetObjectItem(member, "user");
	if (!cJSON_IsObject(user))
		return NULL;

	const cJSON *id = cJSON_GetObjectItem(user, "id");
	const cJSON *avatar = cJSON_GetObjectItem(user, "avatar");

	if (!cJSON_IsString(id) || !cJSON_IsString(avatar) ||
	    avatar->valuestring[0] == '\0')
		return NULL;

	char url[256];
	snprintf(url, sizeof(url),
		 "https://cdn.discordapp.com/avatars/%s/%s.png?size=64",
		 id->valuestring, avatar->valuestring);
	return bstrdup(url);
}

/* ---- Check if member has a specific role ---- */

static bool member_has_role(const cJSON *member, const char *role_id)
{
	const cJSON *roles = cJSON_GetObjectItem(member, "roles");
	if (!cJSON_IsArray(roles))
		return false;

	const cJSON *r = NULL;
	cJSON_ArrayForEach(r, roles)
	{
		if (cJSON_IsString(r) &&
		    strcmp(r->valuestring, role_id) == 0)
			return true;
	}
	return false;
}

/* ---- Main fetch function ---- */

struct discord_result *discord_fetch(const char *bot_token,
				     const char *guild_id,
				     const char *role_id)
{
	struct discord_result *result =
		bzalloc(sizeof(struct discord_result));

	if (!bot_token || bot_token[0] == '\0' || !guild_id ||
	    guild_id[0] == '\0') {
		result->error = bstrdup("Bot token and Guild ID are required");
		return result;
	}

	CURL *curl = curl_easy_init();
	if (!curl) {
		result->error = bstrdup("Failed to initialize curl");
		return result;
	}

	/* Build auth header */
	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s",
		 bot_token);
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, auth_header);
	headers = curl_slist_append(headers, "User-Agent: OBS-Credits/0.1");

	/* Paginate guild members (up to 10 pages of 1000) */
	size_t members_cap = 256;
	size_t members_count = 0;
	struct discord_member *boosters = NULL;
	size_t boosters_count = 0;
	size_t boosters_cap = 64;
	struct discord_member *role_members = NULL;
	size_t role_count = 0;
	size_t role_cap = 64;

	boosters = bzalloc(sizeof(struct discord_member) * boosters_cap);
	if (role_id && role_id[0] != '\0')
		role_members =
			bzalloc(sizeof(struct discord_member) * role_cap);

	char after[64] = "0";
	bool has_role_filter = role_id && role_id[0] != '\0';

	for (int page = 0; page < 10; page++) {
		char url[512];
		snprintf(url, sizeof(url),
			 DISCORD_API_BASE
			 "/guilds/%s/members?limit=1000&after=%s",
			 guild_id, after);

		char *err = NULL;
		cJSON *members_json = discord_get(curl, url, headers, &err);
		if (!members_json) {
			result->error = err;
			break;
		}

		int count = cJSON_GetArraySize(members_json);
		if (count == 0) {
			cJSON_Delete(members_json);
			break;
		}

		const cJSON *member = NULL;
		cJSON_ArrayForEach(member, members_json)
		{
			/* Track last member ID for pagination */
			const cJSON *user =
				cJSON_GetObjectItem(member, "user");
			if (cJSON_IsObject(user)) {
				const cJSON *id =
					cJSON_GetObjectItem(user, "id");
				if (cJSON_IsString(id))
					snprintf(after, sizeof(after), "%s",
						 id->valuestring);
			}

			/* Check booster */
			const cJSON *premium =
				cJSON_GetObjectItem(member, "premium_since");
			if (cJSON_IsString(premium) &&
			    premium->valuestring[0] != '\0') {
				if (boosters_count >= boosters_cap) {
					boosters_cap *= 2;
					boosters = brealloc(
						boosters,
						sizeof(struct discord_member) *
							boosters_cap);
				}
				boosters[boosters_count].display_name =
					get_display_name(member);
				boosters[boosters_count].avatar_url =
					get_avatar_url(member);
				boosters[boosters_count].is_booster = true;
				boosters_count++;
			}

			/* Check role */
			if (has_role_filter &&
			    member_has_role(member, role_id)) {
				if (role_count >= role_cap) {
					role_cap *= 2;
					role_members = brealloc(
						role_members,
						sizeof(struct discord_member) *
							role_cap);
				}
				role_members[role_count].display_name =
					get_display_name(member);
				role_members[role_count].avatar_url =
					get_avatar_url(member);
				role_members[role_count].is_booster = false;
				role_count++;
			}

			members_count++;
		}

		cJSON_Delete(members_json);

		/* If we got fewer than 1000, we've reached the end */
		if (count < 1000)
			break;
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	result->boosters = boosters;
	result->num_boosters = boosters_count;
	result->role_members = role_members;
	result->num_role_members = role_count;

	if (!result->error) {
		blog(LOG_INFO,
		     "[obs-credits] Discord fetch: %zu members scanned, "
		     "%zu boosters, %zu role members",
		     members_count, boosters_count, role_count);
	}

	return result;
}

void discord_result_free(struct discord_result *result)
{
	if (!result)
		return;

	for (size_t i = 0; i < result->num_boosters; i++) {
		bfree(result->boosters[i].display_name);
		bfree(result->boosters[i].avatar_url);
	}
	bfree(result->boosters);

	for (size_t i = 0; i < result->num_role_members; i++) {
		bfree(result->role_members[i].display_name);
		bfree(result->role_members[i].avatar_url);
	}
	bfree(result->role_members);

	bfree(result->error);
	bfree(result);
}
