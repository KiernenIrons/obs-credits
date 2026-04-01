# OBS Credits Plugin — Technical Research

**Author:** Kiernen Irons
**Date:** 2026-04-01
**Status:** Pre-implementation research

---

## Table of Contents

1. [Plugin Vision](#1-plugin-vision)
2. [OBS Source Plugin API](#2-obs-source-plugin-api)
3. [Text Rendering](#3-text-rendering)
4. [Image & Emoji Support](#4-image--emoji-support)
5. [Video Clip Playback](#5-video-clip-playback)
6. [Scrolling & Animation](#6-scrolling--animation)
7. [Data Model & Serialization](#7-data-model--serialization)
8. [Properties UI](#8-properties-ui)
9. [Discord Bot Integration](#9-discord-bot-integration)
10. [Live Chat — YouTube](#10-live-chat--youtube)
11. [Live Chat — Twitch](#11-live-chat--twitch)
12. [Live Chat — Kick & Others](#12-live-chat--kick--others)
13. [Donation Tracking](#13-donation-tracking)
14. [Architecture & Integration Strategy](#14-architecture--integration-strategy)
15. [Unified Data Structures](#15-unified-data-structures)
16. [Implementation Roadmap](#16-implementation-roadmap)
17. [Dependencies & Libraries](#17-dependencies--libraries)
18. [Open Questions & Risks](#18-open-questions--risks)

---

## 1. Plugin Vision

A cinematic, scrolling credits source for OBS Studio — like the end credits of a movie — that supports:

- **Headings** — section titles (e.g., "Director", "Special Thanks", "Moderators")
- **Names & Roles** — paired text entries (name on left, role on right or vice versa)
- **Images** — logos, avatars, emojis inline or alongside names
- **Video Clips** — small clips that scroll with the credits on the side
- **Live Discord data** — server boosters, moderators, honourable mentions pulled from a Discord bot
- **Live chat participants** — auto-populated from YouTube, Twitch, and other platforms
- **Donation roll** — people who donated during the stream, with amounts
- **Multi-platform** — works on Windows, macOS, Linux

---

## 2. OBS Source Plugin API

### Registration

The plugin registers as an `OBS_SOURCE_TYPE_INPUT` source with custom rendering:

```c
struct obs_source_info credits_source_info = {
    .id             = "obs_credits_roll",
    .type           = OBS_SOURCE_TYPE_INPUT,
    .output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
    .get_name       = credits_get_name,
    .create         = credits_create,
    .destroy        = credits_destroy,
    .update         = credits_update,
    .video_tick     = credits_video_tick,    // animation/delta time
    .video_render   = credits_video_render,  // GPU drawing
    .get_width      = credits_get_width,
    .get_height     = credits_get_height,
    .get_properties = credits_get_properties,
    .get_defaults   = credits_get_defaults,
};
```

### Key Callbacks

| Callback | Thread | Purpose |
|---|---|---|
| `create` | UI thread | Allocate state, parse initial settings |
| `destroy` | UI thread | Free all resources |
| `update` | Any thread | Settings changed — reload config (mutex-protect shared state) |
| `video_tick(data, seconds)` | Graphics thread | Advance animation, `seconds` = delta time (~0.016 at 60fps) |
| `video_render(data, effect)` | Graphics thread | All GPU draw calls happen here |
| `get_properties` | UI thread | Build the settings UI |
| `get_defaults` | Any | Set default values for settings |

### Rendering Pipeline

The plugin uses an offscreen render target (`gs_texrender_t`) to composite all credits content, then draws the result as a single texture:

```c
// In video_render:
gs_texrender_reset(ctx->texrender);
if (gs_texrender_begin(ctx->texrender, ctx->width, ctx->height)) {
    // Set scrolling viewport via orthographic projection
    gs_ortho(0.0f, (float)ctx->width,
             ctx->scroll_offset,                        // top edge
             ctx->scroll_offset + (float)ctx->height,   // bottom edge
             -1.0f, 1.0f);

    // Clear background
    struct vec4 clear_color;
    vec4_zero(&clear_color);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0, 0);

    // Draw all elements at their absolute Y positions
    render_all_sections(ctx);

    gs_texrender_end(ctx->texrender);
}

// Draw the composited texture to screen
gs_texture_t *tex = gs_texrender_get_texture(ctx->texrender);
// ... draw with default effect ...
```

### Key Graphics Functions

| Function | Purpose |
|---|---|
| `gs_texrender_create/begin/end/get_texture` | Offscreen render target for compositing |
| `gs_ortho(l, r, t, b, zn, zf)` | Set orthographic projection (scrolling window) |
| `gs_matrix_push/pop/translate3f/scale3f` | Transform stack for positioning elements |
| `gs_draw_sprite(tex, flip, w, h)` | Draw a textured quad (images, text textures) |
| `gs_texture_create(w, h, fmt, levels, data, flags)` | Create GPU texture from pixel data |
| `gs_image_file3_init/init_texture/free` | Load images (PNG, JPEG, GIF) |
| `gs_effect_set_texture` | Bind texture to shader |
| `gs_blend_function(src, dst)` | Alpha blending for transparency |
| `obs_source_video_render(child)` | Render a child source (text, media) |

---

## 3. Text Rendering

### Approach: Private Child Text Sources (Recommended)

Rather than reimplementing text layout with FreeType, the plugin creates internal OBS text sources and renders them as children:

```c
// Create a text source for a heading
obs_data_t *font_data = obs_data_create();
obs_data_set_string(font_data, "face", "Georgia");
obs_data_set_int(font_data, "size", 48);

obs_data_t *settings = obs_data_create();
obs_data_set_string(settings, "text", "Special Thanks");
obs_data_set_obj(settings, "font", font_data);
obs_data_set_int(settings, "color1", 0xFFFFD700);  // gold

obs_source_t *text_src = obs_source_create_private(
    "text_ft2_source",   // cross-platform FreeType
    "heading_special_thanks",
    settings);

obs_data_release(font_data);
obs_data_release(settings);
```

Then in `video_render`, position and render each text source:

```c
gs_matrix_push();
gs_matrix_translate3f(x, y, 0.0f);
obs_source_video_render(text_src);
gs_matrix_pop();
```

**Benefits:**
- Cross-platform (FreeType handles all font rendering)
- No need to reimplement word wrap, kerning, or layout
- Supports all system fonts
- OBS handles the heavy lifting

**Alternative — "text_gdiplus" on Windows:** Higher quality on Windows but not cross-platform. Can detect platform at runtime and choose.

### Text Types Needed

| Type | Font Style | Example |
|---|---|---|
| Section Heading | Large, bold, possibly gold/colored | "CAST", "SPECIAL THANKS" |
| Name | Medium, white | "Kiernen Irons" |
| Role | Medium, gray or italic | "Director" |
| Subheading | Smaller bold | "Server Boosters" |
| Info text | Small, dim | "23 boosts" |

---

## 4. Image & Emoji Support

### Static Images (Logos, Avatars)

Use `gs_image_file3_t` for PNG/JPEG/BMP:

```c
struct gs_image_file3 image;

// Load (any thread)
gs_image_file3_init(&image, path, GS_IMAGE_ALPHA_PREMULTIPLY_SRGB);

// Upload to GPU (graphics thread)
obs_enter_graphics();
gs_image_file3_init_texture(&image);
obs_leave_graphics();

// Render at position
gs_draw_sprite(image.image2.image.texture, 0, width, height);
```

### Animated GIFs

Same API, but call `gs_image_file3_update_texture(&image)` in `video_tick` to advance frames. The `is_animated_gif` field indicates whether animation is needed.

### Emoji Support

Two approaches:

**Option A — Font-based emoji:** If the system font supports emoji (e.g., Segoe UI Emoji on Windows, Apple Color Emoji on macOS), the `text_ft2_source` child will render them natively. This is the simplest path.

**Option B — Image-based emoji:** For consistent cross-platform emoji, ship a sprite sheet (e.g., Twemoji/Noto Emoji) and render emoji as inline images. Parse text for emoji codepoints, replace with image draws.

**Recommendation:** Start with Option A (font-native emoji). Fall back to Option B only if cross-platform consistency is critical.

### Discord Avatars

Discord CDN avatar URLs follow the pattern:
```
https://cdn.discordapp.com/avatars/{user_id}/{avatar_hash}.png?size=64
```

The companion service (or direct libcurl call) can download these to a local cache directory. The plugin then loads them as images via `gs_image_file3`.

---

## 5. Video Clip Playback

### Approach: Private Child Media Sources

Create internal `ffmpeg_source` instances for each clip:

```c
obs_data_t *settings = obs_data_create();
obs_data_set_string(settings, "local_file", "/path/to/clip.mp4");
obs_data_set_bool(settings, "looping", true);
obs_data_set_bool(settings, "restart_on_activate", true);

obs_source_t *media_src = obs_source_create_private(
    "ffmpeg_source", "clip_highlight_01", settings);
obs_data_release(settings);
```

### Layout: Clips Alongside Credits

The credits viewport is split:

```
┌───────────────────────────────┐
│  ┌─────────┐                  │
│  │  VIDEO   │  HEADING        │
│  │  CLIP    │  Name - Role    │
│  │  (side)  │  Name - Role    │
│  └─────────┘  Name - Role    │
│                               │
│         NEXT HEADING          │
│         Name - Role           │
│  ┌─────────┐  Name - Role    │
│  │  CLIP 2 │  Name - Role    │
│  └─────────┘                  │
└───────────────────────────────┘
        ↑ scrolling up ↑
```

Each clip has a Y range it's associated with. When the scroll position is within that range, the clip is rendered at its designated position and playing. Clips outside the viewport can be paused to save resources.

### Media Control

```c
obs_source_media_restart(media_src);       // restart playback
obs_source_media_play_pause(media_src, true);  // pause
obs_source_media_play_pause(media_src, false); // resume
obs_source_media_stop(media_src);          // stop
```

---

## 6. Scrolling & Animation

### Core Scroll Logic

In `video_tick`:

```c
static void credits_video_tick(void *data, float seconds)
{
    struct credits_source *ctx = data;

    if (!ctx->scrolling)
        return;

    // Smooth acceleration/deceleration
    float target_speed = ctx->target_scroll_speed;
    ctx->current_speed += (target_speed - ctx->current_speed) * 5.0f * seconds;
    ctx->scroll_offset += ctx->current_speed * seconds;

    // Check completion
    if (ctx->scroll_offset >= ctx->total_content_height + ctx->height) {
        if (ctx->loop) {
            ctx->scroll_offset = -(float)ctx->height;  // restart from below
        } else {
            ctx->scrolling = false;
        }
    }
}
```

### Scroll Modes

| Mode | Behavior |
|---|---|
| **One-shot** | Scroll up until all content passes, then stop |
| **Loop** | After last item exits top, restart from bottom |
| **Manual** | Scroll position controlled by hotkey or property |
| **Triggered** | Start/stop via OBS hotkey or source activation |

### Timing

- `seconds` parameter in `video_tick` is the frame delta time (~0.016s at 60fps)
- For absolute time: `os_gettime_ns()` returns nanoseconds
- Easing: lerp current speed toward target for smooth start/stop

---

## 7. Data Model & Serialization

### Credits Structure

The credits are organized as an array of **sections**, each containing an array of **entries**:

```json
{
  "credits_config": {
    "scroll_speed": 60,
    "background_color": "#00000000",
    "default_font": "Arial",
    "default_font_size": 32,
    "default_text_color": "#FFFFFF",
    "width": 1920,
    "loop": false
  },
  "sections": [
    {
      "heading": "Directed By",
      "heading_style": {
        "font": "Georgia",
        "size": 56,
        "color": "#FFD700",
        "alignment": "center"
      },
      "entries": [
        {
          "type": "name_role",
          "name": "Kiernen Irons",
          "role": "Director & Developer"
        }
      ]
    },
    {
      "heading": "Server Boosters",
      "source": "discord",
      "discord_filter": "boosters",
      "entries": []
    },
    {
      "heading": "Stream Chat",
      "source": "live_chat",
      "platform": "all",
      "entries": []
    },
    {
      "heading": "Donations",
      "source": "donations",
      "entries": []
    },
    {
      "heading": "Highlights",
      "entries": [
        {
          "type": "clip",
          "path": "/clips/highlight01.mp4",
          "width": 480,
          "height": 270,
          "position": "left"
        }
      ]
    }
  ]
}
```

### Entry Types

| Type | Fields | Renders As |
|---|---|---|
| `name_role` | name, role, avatar_path | "Name ......... Role" with optional avatar |
| `name_only` | name | Centered name |
| `image` | path, width, height | Inline image (logo, emoji) |
| `clip` | path, width, height, position | Video clip on left/right side |
| `spacer` | height | Empty vertical space |
| `text` | text, style | Freeform text block |
| `divider` | style | Horizontal line or decorative divider |

### OBS Data Serialization

Stored using `obs_data_array_t` for sections and nested arrays for entries:

```c
// Saving
obs_data_array_t *sections = obs_data_array_create();
for (each section) {
    obs_data_t *sec = obs_data_create();
    obs_data_set_string(sec, "heading", section->heading);
    obs_data_set_string(sec, "source", section->source);  // "static", "discord", "live_chat"

    obs_data_array_t *entries = obs_data_array_create();
    for (each entry) {
        obs_data_t *ent = obs_data_create();
        obs_data_set_string(ent, "type", entry->type);
        obs_data_set_string(ent, "name", entry->name);
        obs_data_set_string(ent, "role", entry->role);
        obs_data_array_push_back(entries, ent);
        obs_data_release(ent);
    }
    obs_data_set_array(sec, "entries", entries);
    obs_data_array_release(entries);

    obs_data_array_push_back(sections, sec);
    obs_data_release(sec);
}
obs_data_set_array(settings, "sections", sections);
obs_data_array_release(sections);
```

**Alternative:** Load the full credits definition from an external JSON file (path set in properties). This is easier to edit by hand and supports richer structures.

---

## 8. Properties UI

### OBS Property Types Available

| Property Function | Use Case |
|---|---|
| `obs_properties_add_text(..., OBS_TEXT_MULTILINE)` | Credits text editor |
| `obs_properties_add_path(..., OBS_PATH_FILE)` | JSON config file, image paths, clip paths |
| `obs_properties_add_int(min, max, step)` | Scroll speed, font size |
| `obs_properties_add_float(min, max, step)` | Opacity |
| `obs_properties_add_color_alpha` | Text color, background color |
| `obs_properties_add_font` | Font picker (returns face, size, flags) |
| `obs_properties_add_list` | Alignment, scroll mode dropdowns |
| `obs_properties_add_bool` | Loop toggle, enable Discord, enable chat |
| `obs_properties_add_button` | "Reload", "Preview", "Fetch Discord Data" |
| `obs_properties_add_editable_list` | Add/remove/reorder entries |
| `obs_properties_add_group` | Collapsible sections (Discord settings, Chat settings) |

### Proposed UI Layout

```
[Credits Source Properties]

── General ──────────────────────────
  Credits File:     [/path/to/credits.json] [Browse]
  Scroll Speed:     [60] px/s
  Background:       [█ transparent]
  Loop:             [x]

── Default Text Style ───────────────
  Font:             [Arial] [32pt]
  Text Color:       [█ white]
  Heading Color:    [█ gold]

── Discord Integration ──────────────
  [x] Enable Discord
  Bot Token:        [••••••••••••••••]
  Guild ID:         [123456789012345678]
  Moderator Role:   [Moderator ▾]
  [Fetch Discord Data]

── Live Chat ────────────────────────
  [x] Enable Chat Tracking
  Service URL:      [http://localhost:9876]
  [x] YouTube  [x] Twitch  [ ] Kick

── Donations ────────────────────────
  [x] Enable Donations
  [x] Show Amounts

── Clips ────────────────────────────
  Clips Directory:  [/path/to/clips/] [Browse]
```

### Dynamic Property Visibility

Use `obs_property_set_modified_callback` to show/hide sections based on toggles:

```c
static bool discord_enabled_changed(obs_properties_t *props,
    obs_property_t *p, obs_data_t *settings)
{
    bool enabled = obs_data_get_bool(settings, "discord_enabled");
    obs_property_set_visible(obs_properties_get(props, "bot_token"), enabled);
    obs_property_set_visible(obs_properties_get(props, "guild_id"), enabled);
    obs_property_set_visible(obs_properties_get(props, "fetch_discord"), enabled);
    return true;  // refresh UI
}
```

---

## 9. Discord Bot Integration

### Overview

The plugin fetches live data from a Discord server to populate credits sections for boosters, moderators, VIPs, and honourable mentions.

### Required Bot Setup

1. Create a bot at https://discord.com/developers/applications
2. Enable **Server Members Intent** (privileged — required to list members)
3. Invite with scope `bot` and permission `1024` (View Channels):
   ```
   https://discord.com/oauth2/authorize?client_id=APP_ID&scope=bot&permissions=1024
   ```

### REST Endpoints

| Endpoint | Purpose | Notes |
|---|---|---|
| `GET /guilds/{id}/members?limit=1000&after={snowflake}` | List all members (paginated) | Requires GUILD_MEMBERS intent |
| `GET /guilds/{id}/roles` | List all roles | No special intent needed |
| `GET /guilds/{id}?with_counts=true` | Guild info (boost count, tier) | Returns `premium_subscription_count` |
| `GET /channels/{id}/messages` | Read honourable mentions channel | Needs View Channel permission |

### Authentication

Simple static token — no OAuth flow needed at runtime:

```
Authorization: Bot <token>
```

### Identifying Boosters

Each Guild Member object has `premium_since` (ISO8601 or null). Non-null = active booster.

```json
{
  "user": { "id": "123", "global_name": "CoolUser", "avatar": "abc123" },
  "nick": "Cool Nickname",
  "roles": ["111111", "222222"],
  "premium_since": "2023-06-01T12:00:00.000000+00:00"
}
```

**Important:** Individual boost count per user is NOT available via API. Only `premium_since` per member and total boost count on the guild object.

### Identifying Role Members (Mods, VIPs)

No direct "get members by role" endpoint. Must paginate all members and filter by `roles` array client-side:

```c
// For each member:
for (int r = 0; r < num_roles; r++) {
    if (strcmp(member_roles[r], moderator_role_id) == 0) {
        // This member is a moderator
    }
}
```

### Honourable Mentions

No built-in Discord concept. Options:
1. **Bot-managed database** — Slash commands like `/honour add @user reason` stored in SQLite/JSON
2. **Dedicated channel** — Bot reads messages from a specific channel via `GET /channels/{id}/messages`
3. **Local config file** — Maintained manually, plugin reads it directly

### Avatar URLs

```
https://cdn.discordapp.com/avatars/{user_id}/{avatar_hash}.png?size=64
```

Download to a local cache directory for the plugin to render as images.

### Rate Limits

| Endpoint | Limit |
|---|---|
| `GET /guilds/{id}/members` | ~10 req / 10 seconds |
| `GET /guilds/{id}/roles` | ~10 req / 10 seconds |
| `GET /guilds/{id}` | ~5 req / 5 seconds |
| Global | 50 req/second |

Handle `429 Too Many Requests` by reading `retry_after` from the response. For infrequent fetches (once per stream), rate limits are a non-issue.

### Integration Approach: Direct libcurl from C (Recommended)

The data is fetched infrequently (once when credits load, or on a manual "Refresh" button). Direct REST calls via libcurl are simple and self-contained:

```c
// Pseudocode
CURL *curl = curl_easy_init();
set_header("Authorization: Bot <token>");
GET("https://discord.com/api/v10/guilds/{id}/members?limit=1000");
// Parse JSON with cJSON
// Filter boosters (premium_since != null)
// Filter mods (role ID match)
curl_easy_cleanup(curl);
```

**Must run on a background thread** — never block the OBS graphics/UI thread with HTTP calls.

---

## 10. Live Chat — YouTube

### YouTube Live Chat API

**Polling-based** — no WebSocket. Use `liveChatMessages.list`:

```
GET https://www.googleapis.com/youtube/v3/liveChat/messages
    ?liveChatId={id}
    &part=snippet,authorDetails
    &pageToken={nextPageToken}
```

### Authentication

- **OAuth 2.0 required** (API key alone is insufficient for live chat)
- Desktop app flow: Authorization Code with PKCE
- Scopes: `https://www.googleapis.com/auth/youtube.readonly`

### Detecting Super Chats (Donations)

In the response, `snippet.type` indicates message type:
- `textMessageEvent` — regular chat
- `superChatEvent` — Super Chat (donation)
- `superStickerEvent` — Super Sticker

Super Chat details:
```json
{
  "snippet": {
    "type": "superChatEvent",
    "superChatDetails": {
      "amountMicros": "5000000",
      "currency": "USD",
      "tier": 4
    }
  },
  "authorDetails": {
    "channelId": "UC...",
    "displayName": "Generous Viewer"
  }
}
```

Amount in dollars: `amountMicros / 1,000,000`

### Tracking Unique Chatters

No "list all chatters" endpoint. Must track unique `authorDetails.channelId` values from polled messages.

### Quota Concerns

- **Default: 10,000 units/day**
- `liveChatMessages.list` = **5 units per call**
- Polling every 6 seconds for a 3-hour stream ≈ 9,000 units
- **Must apply for quota increase for production use**
- Respect `pollingIntervalMillis` returned by the API (typically 5-10 seconds)

---

## 11. Live Chat — Twitch

### Two Approaches

**IRC (Legacy but simple):**
```
Connect to irc.chat.twitch.tv:6697 (TLS)
PASS oauth:<token>
NICK <bot_username>
JOIN #<channel>
```
- Messages arrive as IRC PRIVMSG
- Request capabilities for bits/sub info in message tags:
  ```
  CAP REQ :twitch.tv/tags twitch.tv/commands
  ```

**EventSub WebSocket (Recommended):**
```
Connect to wss://eventsub.wss.twitch.tv/ws
Subscribe to events via REST after connection
```

Covers: chat messages, bits, subs, gift subs, follows, raids — all structured JSON.

### Authentication

- **Device Code Flow** recommended for desktop OBS plugin (user sees a code, authorizes in browser)
- Key scopes: `user:read:chat`, `bits:read`, `channel:read:subscriptions`

### Tracking Chatters

Listen for `PRIVMSG` (IRC) or `channel.chat.message` (EventSub) events. Track unique `user-id` from message tags.

### Detecting Donations

| Type | IRC Tag / EventSub | Value |
|---|---|---|
| Bits | `bits=100` tag / `channel.cheer` | 100 bits ≈ $1 to streamer |
| Sub | `msg-id=sub` / `channel.subscribe` | Tier 1=$4.99, Tier 2=$9.99, Tier 3=$24.99 |
| Gift Sub | `msg-id=subgift` / `channel.subscription.gift` | Same tiers |

---

## 12. Live Chat — Kick & Others

### Kick

- **No official public API** as of early 2025
- Community reverse-engineering uses Pusher WebSocket channels
- Unstable — endpoints and message formats can change without notice
- **Treat as experimental** — implement last, behind a feature flag

### Other Platforms

| Platform | API Status | Notes |
|---|---|---|
| Facebook Live | Graph API, polling | Restrictive access, requires app review |
| TikTok Live | Live Events API | Very restrictive access |
| Trovo | Documented WebSocket API | Smaller platform, well-documented |

### Recommendation

Focus on YouTube and Twitch first. Add Kick as experimental. Other platforms are low priority.

---

## 13. Donation Tracking

### Platform-Native Donations

| Platform | Mechanism | Detection |
|---|---|---|
| YouTube | Super Chat / Super Sticker | `superChatEvent` in live chat poll |
| Twitch | Bits / Subs / Gift Subs | IRC tags or EventSub events |

### Third-Party Services (Critical)

**Streamlabs** and **StreamElements** capture PayPal/credit card tips that platform APIs miss:

**Streamlabs:**
- Socket.IO real-time: connect to `https://sockets.streamlabs.com?token=<socket_token>`
- Events: `{ type: "donation", message: [{ name, amount, currency, message }] }`
- REST: `GET https://streamlabs.com/api/v1.0/donations?access_token=<token>`

**StreamElements:**
- Socket.IO: connect to `https://realtime.streamelements.com`
- Events: `event:test` and `event:update` with tip data
- REST: `GET https://api.streamelements.com/kappa/v2/tips/{channel_id}`

### Unified Donation Entry

```json
{
  "donor_name": "GenerousViewer",
  "platform": "youtube",
  "type": "superchat",
  "amount": 5.00,
  "currency": "USD",
  "message": "Great stream!",
  "timestamp": "2026-04-01T20:30:00Z"
}
```

---

## 14. Architecture & Integration Strategy

### Recommended: Companion Service + OBS Plugin

The live data features (chat, donations, Discord) are best handled by a **companion service** running locally, with the OBS plugin consuming aggregated data.

```
┌─────────────────────────────────────────────────┐
│                 OBS Studio                       │
│  ┌───────────────────────────────────────────┐  │
│  │          Credits Source Plugin (C)         │  │
│  │                                           │  │
│  │  Reads: participants.json (file-based)    │  │
│  │     or: ws://localhost:9876 (WebSocket)   │  │
│  │                                           │  │
│  │  Renders: scrolling credits with all data │  │
│  └────────────────────┬──────────────────────┘  │
│                       │ reads                    │
└───────────────────────┼─────────────────────────┘
                        │
         ┌──────────────┴──────────────┐
         │   Companion Service         │
         │   (Node.js / Python / Go)   │
         │                             │
         │  ┌─── YouTube Chat ───┐     │
         │  ├─── Twitch IRC ─────┤     │
         │  ├─── Kick (exp.) ────┤     │
         │  ├─── Discord REST ───┤     │
         │  ├─── Streamlabs ─────┤     │
         │  └─── StreamElements ─┘     │
         │                             │
         │  Writes: participants.json  │
         │     or: ws://localhost:9876  │
         └─────────────────────────────┘
```

### Why a Companion Service?

| Concern | Direct from C Plugin | Companion Service |
|---|---|---|
| OAuth flows | Painful in C | Native in JS/Python |
| WebSocket management | Complex in C | Trivial in JS/Python |
| Multi-platform chat | All logic in C | Adapters in high-level language |
| Deployment | Single DLL | Two components |
| Maintenance | Recompile for API changes | Update script only |
| Latency | Lower | Negligible difference |

### Phased Approach

**Phase 1 (MVP) — File-based:**
- Companion writes `participants.json` to a known path
- Plugin reads the file on a timer (every 5-10 seconds) or on button press
- Zero networking code in the C plugin
- Simple and debuggable

**Phase 2 — WebSocket:**
- Replace file polling with `ws://localhost:9876`
- Real-time updates as chatters/donations arrive
- Plugin uses a background thread for WebSocket client

### Discord: Direct from Plugin (Exception)

Discord data changes slowly and is fetched once. It can be fetched directly from the C plugin via libcurl without a companion service. This avoids requiring the companion for streamers who only want Discord credits.

---

## 15. Unified Data Structures

### Participant JSON (Written by Companion Service)

```json
{
  "stream_id": "abc123",
  "last_updated": "2026-04-01T20:45:00Z",
  "participants": [
    {
      "id": "yt_UC1234",
      "username": "ViewerOne",
      "display_name": "Viewer One",
      "platform": "youtube",
      "avatar_url": "https://...",
      "message_count": 15,
      "first_seen": "2026-04-01T19:05:00Z",
      "last_seen": "2026-04-01T20:40:00Z",
      "is_moderator": false,
      "is_subscriber": true,
      "donations": [
        {
          "type": "superchat",
          "amount": 10.00,
          "currency": "USD",
          "message": "Love the stream!",
          "timestamp": "2026-04-01T20:15:00Z"
        }
      ]
    }
  ],
  "donation_total": {
    "USD": 45.00,
    "EUR": 10.00
  }
}
```

### Discord Data JSON (Fetched by Plugin or Companion)

```json
{
  "guild_name": "Kiernen's Community",
  "boost_level": 2,
  "boost_count": 14,
  "last_fetched": "2026-04-01T19:00:00Z",
  "boosters": [
    {
      "name": "BoostFan",
      "display_name": "Boost Fan",
      "avatar_url": "https://cdn.discordapp.com/avatars/123/abc.png",
      "boosting_since": "2023-06-01T12:00:00Z"
    }
  ],
  "moderators": [
    {
      "name": "TrustedMod",
      "display_name": "Trusted Mod",
      "avatar_url": "https://cdn.discordapp.com/avatars/456/def.png"
    }
  ],
  "honourable_mentions": [
    {
      "name": "LegendUser",
      "display_name": "Legend User",
      "reason": "Outstanding community contributions"
    }
  ]
}
```

### C Struct (Plugin Side)

```c
struct credits_participant {
    char *username;
    char *display_name;
    char *platform;          // "youtube", "twitch", "kick"
    char *avatar_path;       // local cached path
    int message_count;
    bool is_moderator;
    bool is_subscriber;
    double total_donated;
    char *donation_currency;
};

struct credits_discord_member {
    char *display_name;
    char *avatar_path;
    char *role;              // "booster", "moderator", "honourable_mention"
    char *extra_info;        // boost date, mention reason, etc.
};

struct credits_live_data {
    struct credits_participant *participants;
    size_t num_participants;
    struct credits_discord_member *discord_members;
    size_t num_discord_members;
    double donation_total;
};
```

---

## 16. Implementation Roadmap

### Phase 1 — Core Credits Roll (MVP)

- [ ] Plugin skeleton: module load/unload, source registration
- [ ] Static credits rendering: headings, names, roles from JSON config
- [ ] Scrolling animation with speed control
- [ ] Basic text styling (font, size, color, alignment)
- [ ] Image support (logos, static images)
- [ ] Properties UI for basic configuration
- [ ] One-shot and loop scroll modes

### Phase 2 — Rich Media

- [ ] Video clip playback alongside credits
- [ ] Emoji support (font-native first)
- [ ] Discord integration (direct libcurl — boosters, mods, VIPs)
- [ ] Avatar image caching and rendering
- [ ] "Fetch Discord Data" button in properties

### Phase 3 — Live Data

- [ ] Companion service scaffold (Node.js recommended)
- [ ] YouTube Live Chat adapter (OAuth + polling)
- [ ] Twitch chat adapter (EventSub WebSocket)
- [ ] File-based data exchange (participants.json)
- [ ] Auto-populated "Stream Chat" credits section
- [ ] Donation tracking (Super Chat, Bits)

### Phase 4 — Polish & Extras

- [ ] Streamlabs / StreamElements integration
- [ ] WebSocket communication (replace file polling)
- [ ] Kick support (experimental)
- [ ] Custom credits editor (Qt dialog via button callback)
- [ ] Honourable mentions system (Discord bot commands)
- [ ] Cross-platform identity merging (same person on YouTube + Twitch)
- [ ] Configurable section ordering and templates
- [ ] Transition effects (fade in/out for sections)

---

## 17. Dependencies & Libraries

### C Plugin

| Library | Purpose | License | Notes |
|---|---|---|---|
| libobs | OBS plugin API | GPL-2.0 | Provided by OBS |
| obs-frontend-api | Frontend interaction | GPL-2.0 | Provided by OBS |
| libcurl | HTTP requests (Discord) | MIT-like | Ships with most systems; OBS may bundle it |
| cJSON | JSON parsing | MIT | Single header+source file, embed directly |
| FreeType2 | Text rendering (if not using child sources) | FTL/GPL-2.0 | Ships with OBS |

### Companion Service (Node.js)

| Package | Purpose |
|---|---|
| `ws` | WebSocket server (to OBS plugin) + client (to Twitch EventSub) |
| `node-fetch` or built-in `fetch` | HTTP requests (YouTube, Discord) |
| `googleapis` | YouTube Data API client |
| `tmi.js` or raw IRC | Twitch chat (if IRC approach) |
| `socket.io-client` | Streamlabs / StreamElements |
| `express` | Optional REST API for plugin queries |

### Build System

| Tool | Version |
|---|---|
| CMake | 3.16+ |
| C compiler | C11 (MSVC, GCC, Clang) |
| OBS Studio | 30+ |
| Qt 6 | Only if building custom editor dialogs |

---

## 18. Open Questions & Risks

### Open Questions

1. **Individual boost count** — Discord API does not expose per-user boost count. Can we get this from the bot via a workaround, or only show "Booster since [date]"?
2. **YouTube quota** — 10,000 units/day is tight for long streams. Should we apply for increased quota immediately, or offer a "lite mode" that polls less frequently?
3. **Companion service packaging** — How do we distribute the companion? Standalone executable (pkg/nexe)? Require Node.js? Docker?
4. **Credits editor** — Is the OBS properties UI sufficient, or do we need a full custom Qt editor from day one?
5. **Font-native emoji** — How well does `text_ft2_source` handle emoji across platforms? Need to test Windows/macOS/Linux.
6. **Clip sync** — Should video clips loop for the duration they're visible, or play once? What if the clip is longer than its scroll visibility window?

### Risks

| Risk | Impact | Mitigation |
|---|---|---|
| YouTube API quota exhaustion | Chat tracking stops mid-stream | Adjustable poll rate, lite mode, quota increase application |
| Kick API instability | Kick support breaks without notice | Feature flag, treat as experimental, degrade gracefully |
| Discord privileged intent approval | Bots in 100+ guilds need verification | Document as requirement; most users will have small bots |
| Complex Properties UI limitations | OBS properties are flat, poor for structured data | External JSON config file + custom Qt editor as escape hatch |
| Cross-platform text rendering | Font/emoji inconsistencies | Test on all platforms early; fallback to image-based emoji |
| Companion service reliability | Chat data stops if service crashes | Auto-restart, health checks, file-based fallback |

---

## References

- OBS Plugin API: `libobs/obs-source.h`, `libobs/graphics/graphics.h`
- OBS Plugin Template: https://github.com/obsproject/obs-plugintemplate
- Discord API: https://discord.com/developers/docs
- YouTube Live Streaming API: https://developers.google.com/youtube/v3/live/docs
- Twitch EventSub: https://dev.twitch.tv/docs/eventsub
- Streamlabs API: https://dev.streamlabs.com
- StreamElements API: https://docs.streamelements.com
- cJSON: https://github.com/DaveGamble/cJSON
- FreeType: https://freetype.org
