#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/*
 * YouTube Live Chat Scraper
 *
 * Scrapes YouTube live chat from a channel URL to collect unique
 * chatter display names. No API key or OAuth required - works by
 * parsing the public live chat page.
 *
 * Usage:
 *   yt_chat_start(&ctx, "https://youtube.com/@channel");
 *   // ... polling thread runs, collecting chatters ...
 *   char *names = yt_chat_get_names(&ctx);  // "Alice\nBob\nCharlie"
 *   bfree(names);
 *   yt_chat_stop(&ctx);
 */

#define YT_CHAT_MAX_NAMES 10000

struct yt_chat_ctx {
	/* Config */
	char *channel_url;

	/* State */
	pthread_t thread;
	pthread_mutex_t mutex;
	bool running;
	bool thread_active;

	/* Collected unique chatter names (protected by mutex) */
	char **names;
	size_t num_names;
	size_t names_cap;

	/* Internal: continuation token for polling */
	char *continuation;
	char *video_id;
};

/* Initialize the context (call once) */
void yt_chat_init(struct yt_chat_ctx *ctx);

/* Start collecting chatters for the given channel URL.
 * Clears any previously collected names.
 * Spawns a background thread that polls live chat. */
void yt_chat_start(struct yt_chat_ctx *ctx, const char *channel_url);

/* Stop collecting and clean up the background thread. */
void yt_chat_stop(struct yt_chat_ctx *ctx);

/* Clear all collected names (call at stream start). */
void yt_chat_clear(struct yt_chat_ctx *ctx);

/* Get newline-separated string of all unique chatter names.
 * Caller must bfree() the returned string. Returns NULL if no names. */
char *yt_chat_get_names(struct yt_chat_ctx *ctx);

/* Get current chatter count. */
size_t yt_chat_get_count(struct yt_chat_ctx *ctx);

/* Free all resources. */
void yt_chat_destroy(struct yt_chat_ctx *ctx);
