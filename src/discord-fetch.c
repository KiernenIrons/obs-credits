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

/* ---- Per-role dynamic array helpers ---- */

struct role_buf {
	struct discord_member *members;
	size_t count;
	size_t cap;
};

static void role_buf_push(struct role_buf *rb, const cJSON *member,
			  bool is_booster)
{
	if (rb->count >= rb->cap) {
		rb->cap = rb->cap ? rb->cap * 2 : 32;
		rb->members = brealloc(rb->members,
				       sizeof(struct discord_member) * rb->cap);
	}
	rb->members[rb->count].display_name = get_display_name(member);
	rb->members[rb->count].avatar_url = get_avatar_url(member);
	rb->members[rb->count].is_booster = is_booster;
	rb->count++;
}

/* ---- Main fetch function ---- */

struct discord_result *discord_fetch(const char *bot_token,
				     const char *guild_id,
				     const char *role_ids[DISCORD_MAX_ROLES])
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

	/* Determine which role slots are active */
	bool role_active[DISCORD_MAX_ROLES] = {false};
	int active_role_count = 0;
	for (int i = 0; i < DISCORD_MAX_ROLES; i++) {
		if (role_ids && role_ids[i] && role_ids[i][0] != '\0') {
			role_active[i] = true;
			active_role_count++;
		}
	}

	/* Dynamic arrays for boosters and each role bucket */
	struct role_buf boosters_buf = {NULL, 0, 0};
	struct role_buf role_bufs[DISCORD_MAX_ROLES] = {{NULL, 0, 0}};

	size_t members_count = 0;
	char after[64] = "0";

	/* Paginate guild members (up to 10 pages of 1000) */
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
				role_buf_push(&boosters_buf, member, true);
			}

			/* Check each active role slot */
			for (int i = 0; i < DISCORD_MAX_ROLES; i++) {
				if (!role_active[i])
					continue;
				if (member_has_role(member, role_ids[i]))
					role_buf_push(&role_bufs[i], member,
						      false);
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

	result->boosters = boosters_buf.members;
	result->num_boosters = boosters_buf.count;

	for (int i = 0; i < DISCORD_MAX_ROLES; i++) {
		result->roles[i].members = role_bufs[i].members;
		result->roles[i].num_members = role_bufs[i].count;
	}

	if (!result->error) {
		blog(LOG_INFO,
		     "[obs-credits] Discord fetch: %zu members scanned, "
		     "%zu boosters, %d active role slots",
		     members_count, boosters_buf.count, active_role_count);
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

	for (int i = 0; i < DISCORD_MAX_ROLES; i++) {
		for (size_t j = 0; j < result->roles[i].num_members; j++) {
			bfree(result->roles[i].members[j].display_name);
			bfree(result->roles[i].members[j].avatar_url);
		}
		bfree(result->roles[i].members);
	}

	bfree(result->error);
	bfree(result);
}
