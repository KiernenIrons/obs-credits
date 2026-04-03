#include "youtube-chat.h"

#include <obs-module.h>
#include <util/platform.h>
#include <curl/curl.h>
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

/* ---- HTTP helpers ---- */

struct http_buf {
	char *data;
	size_t size;
};

static size_t yt_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
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

static char *yt_http_get(const char *url)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return NULL;

	struct http_buf buf = {NULL, 0};

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, yt_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT,
			 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
			 "AppleWebKit/537.36 (KHTML, like Gecko) "
			 "Chrome/120.0.0.0 Safari/537.36");
#ifdef _WIN32
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
			 CURLSSLOPT_REVOKE_BEST_EFFORT);
#endif

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		bfree(buf.data);
		return NULL;
	}

	return buf.data;
}

static char *yt_http_post_json(const char *url, const char *json_body)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return NULL;

	struct http_buf buf = {NULL, 0};
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, yt_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT,
			 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
			 "AppleWebKit/537.36 (KHTML, like Gecko) "
			 "Chrome/120.0.0.0 Safari/537.36");
#ifdef _WIN32
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
			 CURLSSLOPT_REVOKE_BEST_EFFORT);
#endif

	CURLcode res = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		bfree(buf.data);
		return NULL;
	}

	return buf.data;
}

/* ---- Name tracking ---- */

static bool has_name(struct yt_chat_ctx *ctx, const char *name)
{
	for (size_t i = 0; i < ctx->num_names; i++) {
		if (strcmp(ctx->names[i], name) == 0)
			return true;
	}
	return false;
}

static void add_name(struct yt_chat_ctx *ctx, const char *name)
{
	if (!name || name[0] == '\0')
		return;
	if (ctx->num_names >= YT_CHAT_MAX_NAMES)
		return;
	if (has_name(ctx, name))
		return;

	if (ctx->num_names >= ctx->names_cap) {
		ctx->names_cap = ctx->names_cap ? ctx->names_cap * 2 : 64;
		ctx->names = brealloc(ctx->names,
				      sizeof(char *) * ctx->names_cap);
	}
	ctx->names[ctx->num_names++] = bstrdup(name);
}

/* ---- Find live video ID from channel page ---- */

static char *find_live_video_id(const char *channel_url)
{
	/* Try the /live endpoint first - redirects to current livestream */
	size_t url_len = strlen(channel_url);
	char *live_url = bmalloc(url_len + 16);
	snprintf(live_url, url_len + 16, "%s/live", channel_url);

	char *page = yt_http_get(live_url);
	bfree(live_url);

	if (!page)
		return NULL;

	/* Look for "isLiveContent":true and extract video ID from canonical URL
	 * Pattern: <link rel="canonical" href="https://www.youtube.com/watch?v=VIDEO_ID"> */
	if (!strstr(page, "isLiveContent")) {
		bfree(page);
		return NULL;
	}

	const char *canonical = strstr(page, "watch?v=");
	if (!canonical) {
		bfree(page);
		return NULL;
	}

	canonical += 8; /* skip "watch?v=" */
	char video_id[16] = {0};
	int i = 0;
	while (i < 11 && canonical[i] && canonical[i] != '"' &&
	       canonical[i] != '&' && canonical[i] != '\\') {
		video_id[i] = canonical[i];
		i++;
	}
	video_id[i] = '\0';

	bfree(page);

	if (i < 8)
		return NULL; /* too short to be a valid ID */

	return bstrdup(video_id);
}

/* ---- Get initial chat continuation token ---- */

static char *get_chat_continuation(const char *video_id)
{
	char url[128];
	snprintf(url, sizeof(url),
		 "https://www.youtube.com/live_chat?v=%s&is_popout=1",
		 video_id);

	char *page = yt_http_get(url);
	if (!page)
		return NULL;

	/* Find continuation token in the page source.
	 * Pattern: "continuation":"TOKEN" */
	const char *needle = "\"continuation\":\"";
	const char *found = strstr(page, needle);
	if (!found) {
		bfree(page);
		return NULL;
	}

	found += strlen(needle);
	const char *end = strchr(found, '"');
	if (!end) {
		bfree(page);
		return NULL;
	}

	size_t len = (size_t)(end - found);
	char *token = bmalloc(len + 1);
	memcpy(token, found, len);
	token[len] = '\0';

	bfree(page);
	return token;
}

/* ---- Poll chat using innertube API ---- */

static void poll_chat(struct yt_chat_ctx *ctx)
{
	if (!ctx->continuation)
		return;

	/* Use YouTube's innertube API to get live chat messages */
	char *body = bmalloc(strlen(ctx->continuation) + 512);
	snprintf(body, strlen(ctx->continuation) + 512,
		 "{\"context\":{\"client\":{\"clientName\":\"WEB\","
		 "\"clientVersion\":\"2.20240101.00.00\"}},"
		 "\"continuation\":\"%s\"}",
		 ctx->continuation);

	char *response = yt_http_post_json(
		"https://www.youtube.com/youtubei/v1/live_chat/get_live_chat"
		"?prettyPrint=false",
		body);
	bfree(body);

	if (!response) {
		blog(LOG_WARNING, "[obs-credits] YouTube chat: no response from poll");
		return;
	}

	cJSON *root = cJSON_Parse(response);
	if (!root) {
		blog(LOG_WARNING, "[obs-credits] YouTube chat: failed to parse poll response (len=%zu)",
		     strlen(response));
		bfree(response);
		return;
	}
	bfree(response);

	/* Extract continuation token for next poll */
	const cJSON *cont_actions =
		cJSON_GetObjectItem(root, "continuationContents");
	if (!cJSON_IsObject(cont_actions)) {
		blog(LOG_WARNING, "[obs-credits] YouTube chat: no continuationContents in response");
		/* Try to log top-level keys for debugging */
		cJSON *child = root->child;
		while (child) {
			blog(LOG_INFO, "[obs-credits] YouTube chat: response key: %s",
			     child->string ? child->string : "(null)");
			child = child->next;
		}
		cJSON_Delete(root);
		return;
	}
	if (cJSON_IsObject(cont_actions)) {
		const cJSON *live_chat = cJSON_GetObjectItem(
			cont_actions, "liveChatContinuation");
		if (cJSON_IsObject(live_chat)) {
			/* Get next continuation */
			const cJSON *continuations = cJSON_GetObjectItem(
				live_chat, "continuations");
			if (cJSON_IsArray(continuations)) {
				const cJSON *first =
					cJSON_GetArrayItem(continuations, 0);
				if (cJSON_IsObject(first)) {
					/* Try invalidation then timed */
					const cJSON *ic = cJSON_GetObjectItem(
						first,
						"invalidationContinuationData");
					const cJSON *tc = cJSON_GetObjectItem(
						first,
						"timedContinuationData");
					const cJSON *cont_obj = ic ? ic : tc;
					if (cJSON_IsObject(cont_obj)) {
						const cJSON *c =
							cJSON_GetObjectItem(
								cont_obj,
								"continuation");
						if (cJSON_IsString(c)) {
							bfree(ctx->continuation);
							ctx->continuation =
								bstrdup(c->valuestring);
						}
					}
				}
			}

			/* Extract chat messages */
			const cJSON *actions = cJSON_GetObjectItem(
				live_chat, "actions");
			blog(LOG_INFO, "[obs-credits] YouTube chat: actions array %s, size=%d",
			     cJSON_IsArray(actions) ? "found" : "NOT FOUND",
			     cJSON_IsArray(actions) ? cJSON_GetArraySize(actions) : 0);
			if (cJSON_IsArray(actions)) {
				const cJSON *action = NULL;
				cJSON_ArrayForEach(action, actions)
				{
					/* Navigate to the message renderer */
					const cJSON *replay =
						cJSON_GetObjectItem(
							action,
							"replayChatItemAction");
					const cJSON *add_action;
					if (cJSON_IsObject(replay)) {
						const cJSON *inner_actions =
							cJSON_GetObjectItem(
								replay,
								"actions");
						if (!cJSON_IsArray(
							    inner_actions))
							continue;
						add_action =
							cJSON_GetArrayItem(
								inner_actions,
								0);
					} else {
						add_action = action;
					}

					const cJSON *add_item =
						cJSON_GetObjectItem(
							add_action,
							"addChatItemAction");
					if (!cJSON_IsObject(add_item))
						continue;

					const cJSON *item =
						cJSON_GetObjectItem(
							add_item, "item");
					if (!cJSON_IsObject(item))
						continue;

					/* Try different message types */
					const cJSON *renderer = NULL;
					renderer = cJSON_GetObjectItem(
						item,
						"liveChatTextMessageRenderer");
					if (!cJSON_IsObject(renderer))
						renderer = cJSON_GetObjectItem(
							item,
							"liveChatPaidMessageRenderer");
					if (!cJSON_IsObject(renderer))
						continue;

					/* Get author name */
					const cJSON *author_name =
						cJSON_GetObjectItem(
							renderer,
							"authorName");
					if (!cJSON_IsObject(author_name))
						continue;

					const cJSON *simple =
						cJSON_GetObjectItem(
							author_name,
							"simpleText");
					if (!cJSON_IsString(simple))
						continue;

					pthread_mutex_lock(&ctx->mutex);
					add_name(ctx,
						 simple->valuestring);
					pthread_mutex_unlock(&ctx->mutex);
				}
			}
		}
	}

	cJSON_Delete(root);
}

/* ---- Background polling thread ---- */

static void *yt_chat_thread(void *arg)
{
	struct yt_chat_ctx *ctx = arg;

	blog(LOG_INFO,
	     "[obs-credits] YouTube chat: looking for live stream on %s",
	     ctx->channel_url);

	/* Find the live video ID */
	ctx->video_id = find_live_video_id(ctx->channel_url);
	if (!ctx->video_id) {
		blog(LOG_WARNING,
		     "[obs-credits] YouTube chat: no live stream found for %s",
		     ctx->channel_url);
		ctx->thread_active = false;
		return NULL;
	}

	blog(LOG_INFO,
	     "[obs-credits] YouTube chat: found live stream %s, getting chat...",
	     ctx->video_id);

	/* Get initial continuation token */
	ctx->continuation = get_chat_continuation(ctx->video_id);
	if (!ctx->continuation) {
		blog(LOG_WARNING,
		     "[obs-credits] YouTube chat: failed to get chat token for %s",
		     ctx->video_id);
		ctx->thread_active = false;
		return NULL;
	}

	blog(LOG_INFO,
	     "[obs-credits] YouTube chat: polling started for %s",
	     ctx->video_id);

	/* Poll loop */
	while (ctx->running) {
		poll_chat(ctx);

		/* Sleep ~6 seconds between polls (respect YouTube rate limits).
		 * Sleep in 100ms chunks so we can stop quickly. */
		for (int i = 0; i < 60 && ctx->running; i++)
			os_sleep_ms(100);
	}

	blog(LOG_INFO,
	     "[obs-credits] YouTube chat: polling stopped (%zu unique chatters)",
	     ctx->num_names);

	ctx->thread_active = false;
	return NULL;
}

/* ---- Public API ---- */

void yt_chat_init(struct yt_chat_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	pthread_mutex_init(&ctx->mutex, NULL);
}

void yt_chat_start(struct yt_chat_ctx *ctx, const char *channel_url)
{
	yt_chat_stop(ctx);
	yt_chat_clear(ctx);

	if (!channel_url || channel_url[0] == '\0')
		return;

	bfree(ctx->channel_url);
	ctx->channel_url = bstrdup(channel_url);
	ctx->running = true;
	ctx->thread_active = true;

	pthread_create(&ctx->thread, NULL, yt_chat_thread, ctx);
}

void yt_chat_stop(struct yt_chat_ctx *ctx)
{
	if (!ctx->running && !ctx->thread_active)
		return;

	ctx->running = false;

	if (ctx->thread_active) {
		pthread_join(ctx->thread, NULL);
		ctx->thread_active = false;
	}

	bfree(ctx->continuation);
	ctx->continuation = NULL;
	bfree(ctx->video_id);
	ctx->video_id = NULL;
}

void yt_chat_clear(struct yt_chat_ctx *ctx)
{
	pthread_mutex_lock(&ctx->mutex);
	for (size_t i = 0; i < ctx->num_names; i++)
		bfree(ctx->names[i]);
	bfree(ctx->names);
	ctx->names = NULL;
	ctx->num_names = 0;
	ctx->names_cap = 0;
	pthread_mutex_unlock(&ctx->mutex);
}

char *yt_chat_get_names(struct yt_chat_ctx *ctx)
{
	pthread_mutex_lock(&ctx->mutex);

	if (ctx->num_names == 0) {
		pthread_mutex_unlock(&ctx->mutex);
		return NULL;
	}

	size_t total = 0;
	for (size_t i = 0; i < ctx->num_names; i++)
		total += strlen(ctx->names[i]) + 1;

	char *result = bmalloc(total + 1);
	result[0] = '\0';
	for (size_t i = 0; i < ctx->num_names; i++) {
		if (i > 0)
			strcat(result, "\n");
		strcat(result, ctx->names[i]);
	}

	pthread_mutex_unlock(&ctx->mutex);
	return result;
}

size_t yt_chat_get_count(struct yt_chat_ctx *ctx)
{
	pthread_mutex_lock(&ctx->mutex);
	size_t count = ctx->num_names;
	pthread_mutex_unlock(&ctx->mutex);
	return count;
}

void yt_chat_destroy(struct yt_chat_ctx *ctx)
{
	yt_chat_stop(ctx);
	yt_chat_clear(ctx);
	bfree(ctx->channel_url);
	ctx->channel_url = NULL;
	pthread_mutex_destroy(&ctx->mutex);
}
