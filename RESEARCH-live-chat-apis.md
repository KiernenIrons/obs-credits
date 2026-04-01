# Live Stream Chat API Research

Research for the OBS Credits Plugin - tracking viewers, chatters, and donors across streaming platforms.

> **Note**: This research is based on API documentation as of early-to-mid 2025. Verify current endpoints and quota details against official docs before implementation, as platforms update frequently.
>
> Key documentation links to check:
> - https://developers.google.com/youtube/v3/live/docs
> - https://dev.twitch.tv/docs/
> - https://dev.kick.com/ (if available)
> - https://developers.streamlabs.com/
> - https://docs.streamelements.com/

---

## 1. YouTube Live Chat API

### Overview

YouTube provides live chat access through the **YouTube Data API v3**. There is no WebSocket/PubSub push mechanism for live chat - it is **polling-based only**.

### Endpoint: `liveChatMessages.list`

```
GET https://www.googleapis.com/youtube/v3/liveChat/messages
```

**Required parameters:**
- `liveChatId` - The ID of the live chat (obtained from `liveBroadcasts.list` or `videos.list` with `liveStreamingDetails` part)
- `part` - Comma-separated list: `snippet`, `authorDetails`, `id`

**Optional parameters:**
- `pageToken` - For pagination (returned in previous response as `nextPageToken`)
- `maxResults` - 200 to 2000 (default 500)
- `hl` - Language hint
- `profileImageSize` - Size of profile images returned

**Response structure (key fields):**
```json
{
  "pollingIntervalMillis": 6000,
  "nextPageToken": "...",
  "pageInfo": { "totalResults": 50, "resultsPerPage": 500 },
  "items": [
    {
      "id": "...",
      "snippet": {
        "type": "textMessageEvent",
        "liveChatId": "...",
        "authorChannelId": "UCxxxx",
        "publishedAt": "2025-01-01T00:00:00Z",
        "hasDisplayContent": true,
        "displayMessage": "Hello!",
        "textMessageDetails": { "messageText": "Hello!" },
        "superChatDetails": {
          "amountMicros": "5000000",
          "currency": "USD",
          "amountDisplayString": "$5.00",
          "userComment": "Great stream!",
          "tier": 4
        },
        "superStickerDetails": {
          "superStickerMetadata": { "stickerId": "...", "altText": "..." },
          "amountMicros": "2000000",
          "currency": "USD",
          "amountDisplayString": "$2.00",
          "tier": 2
        }
      },
      "authorDetails": {
        "channelId": "UCxxxx",
        "channelUrl": "...",
        "displayName": "Username",
        "profileImageUrl": "...",
        "isVerified": false,
        "isChatOwner": false,
        "isChatSponsor": false,
        "isChatModerator": false
      }
    }
  ]
}
```

### Polling workflow

1. Call `liveChatMessages.list` with the `liveChatId`
2. Process all `items` in the response
3. Store `nextPageToken` from response
4. Wait for `pollingIntervalMillis` (typically 5000-10000ms, server-controlled)
5. Call again with the stored `pageToken` to get only new messages
6. Repeat until stream ends

**Important**: The `pollingIntervalMillis` value is set by the server and varies. Polling faster than this value will return the same data and waste quota. Typical values are 5-10 seconds.

### Detecting Super Chats / Super Stickers

The `snippet.type` field indicates message type:
- `"textMessageEvent"` - Normal chat message
- `"superChatEvent"` - Super Chat (paid message)
- `"superStickerEvent"` - Super Sticker
- `"memberMilestoneChatEvent"` - Membership milestone
- `"newSponsorEvent"` - New channel member
- `"membershipGiftingEvent"` - Gift membership
- `"giftMembershipReceivedEvent"` - Received gift membership
- `"messageDeletedEvent"` - Deleted message
- `"userBannedEvent"` - Banned user

For Super Chats, extract donation info from `snippet.superChatDetails`:
- `amountMicros` - Amount in micros (divide by 1,000,000 for actual amount)
- `currency` - ISO 4217 currency code
- `amountDisplayString` - Formatted string like "$5.00"
- `tier` - 1-7, corresponds to color tier

### Getting unique chatters

There is no dedicated "list all chatters" endpoint. You must:
1. Poll `liveChatMessages.list` throughout the stream
2. Extract `authorDetails.channelId` and `authorDetails.displayName` from each message
3. Maintain a local set/map of unique channel IDs

### Authentication: OAuth 2.0

**Required**: OAuth 2.0 user authorization (not just an API key).

The `liveChatMessages.list` endpoint requires an authenticated user. An API key alone is NOT sufficient for live chat endpoints.

**OAuth2 flow (Authorization Code with PKCE recommended for desktop apps):**
1. Register app in Google Cloud Console, enable YouTube Data API v3
2. Create OAuth 2.0 credentials (Desktop app type)
3. Redirect user to: `https://accounts.google.com/o/oauth2/v2/auth` with:
   - `client_id`, `redirect_uri`, `response_type=code`, `scope`, `code_challenge`, `code_challenge_method=S256`
4. User authorizes, Google redirects to your redirect URI with `code`
5. Exchange code for tokens at: `https://oauth2.googleapis.com/token`
6. Use `access_token` in API requests via `Authorization: Bearer <token>` header
7. Refresh with `refresh_token` when access token expires (1 hour)

**Required scopes:**
- `https://www.googleapis.com/auth/youtube.readonly` - Read live chat
- `https://www.googleapis.com/auth/youtube` - Full access (needed if also sending messages)

### Quota costs and rate limits

YouTube Data API v3 has a **quota system** (not simple rate limits):

- **Default quota**: 10,000 units per day per project
- `liveChatMessages.list` costs **5 quota units** per call (with `snippet` part)
- `liveBroadcasts.list` costs **100 quota units** per call (needed once to get `liveChatId`)
- `videos.list` costs **1 quota unit** per call (alternative way to get `liveChatId`)

**Quota math for a stream:**
- Polling every 6 seconds = 10 calls/min = 600 calls/hour = 3,000 units/hour
- A 3-hour stream = ~9,000 units, nearly exhausting the daily 10,000 quota
- Polling every 10 seconds = 6 calls/min = 360 calls/hour = 1,800 units/hour
- A 3-hour stream at 10s = ~5,400 units (more sustainable)

**Recommendation**: Respect the server-provided `pollingIntervalMillis` and apply for a **quota increase** through Google Cloud Console if needed for production use. Increases are reviewed and typically granted for legitimate applications.

### Getting the `liveChatId`

**Option A** - From `liveBroadcasts.list` (100 quota units):
```
GET https://www.googleapis.com/youtube/v3/liveBroadcasts
  ?part=snippet&broadcastStatus=active&mine=true
```
Returns `snippet.liveChatId`.

**Option B** - From `videos.list` (1 quota unit, much cheaper):
```
GET https://www.googleapis.com/youtube/v3/videos
  ?part=liveStreamingDetails&id=VIDEO_ID
```
Returns `liveStreamingDetails.activeLiveChatId`.

---

## 2. Twitch Chat Integration

### Approach A: IRC (Internet Relay Chat)

Twitch chat is built on IRC. You can connect using any IRC client.

**Connection details:**
- Server: `irc.chat.twitch.tv`
- Port: `6667` (plaintext) or `6697` (TLS - recommended)
- WebSocket: `wss://irc-ws.chat.twitch.tv:443`

**Authentication:**
```
PASS oauth:<access_token>
NICK <bot_username>
```

**Joining a channel and receiving messages:**
```
JOIN #channelname
```

Messages arrive as:
```
:username!username@username.tmi.twitch.tv PRIVMSG #channel :Hello world
```

**Requesting capabilities (essential for metadata):**
```
CAP REQ :twitch.tv/tags twitch.tv/commands twitch.tv/membership
```

With `tags` capability, messages include rich metadata:
```
@badges=subscriber/12;color=#FF0000;display-name=User123;emotes=;
first-msg=0;id=msg-id;mod=0;subscriber=1;tmi-sent-ts=1234567890;
turbo=0;user-id=12345;user-type=
:user123!user123@user123.tmi.twitch.tv PRIVMSG #channel :Hello!
```

**Detecting bits from IRC tags:**
The `bits` tag is present on messages containing Cheers:
```
@bits=100;display-name=User123;... PRIVMSG #channel :Cheer100 Great stream!
```

**Detecting subscriptions from IRC:**
With `twitch.tv/commands` capability, you receive USERNOTICE messages:
```
@msg-id=sub;... :tmi.twitch.tv USERNOTICE #channel
@msg-id=resub;... :tmi.twitch.tv USERNOTICE #channel :message
@msg-id=subgift;... :tmi.twitch.tv USERNOTICE #channel
@msg-id=submysterygift;... :tmi.twitch.tv USERNOTICE #channel
```

Key `msg-id` values:
- `sub` - New subscription
- `resub` - Resubscription
- `subgift` - Gift sub to specific user
- `submysterygift` - Anonymous mass gift subs
- `raid` - Incoming raid
- `ritual` - New viewer ritual

**IRC rate limits:**
- Regular users: 20 messages per 30 seconds
- Moderators/VIPs: 100 messages per 30 seconds
- Read-only (no sending): Effectively unlimited for receiving
- JOIN rate: 20 JOINs per 10 seconds

### Approach B: Twitch EventSub (WebSocket)

EventSub is Twitch's modern event system. For real-time events, use the **WebSocket transport**.

**WebSocket connection:**
```
wss://eventsub.wss.twitch.tv/ws
```

On connect, you receive a `session_welcome` message with a `session_id`. Use this to create subscriptions via the Twitch API.

**Relevant subscription types:**

| Type | Description | Scope Required |
|------|-------------|----------------|
| `channel.chat.message` | Chat messages in channel | `user:read:chat` |
| `channel.subscribe` | New subscription | `channel:read:subscriptions` |
| `channel.subscription.gift` | Gift subscriptions | `channel:read:subscriptions` |
| `channel.subscription.message` | Resub with message | `channel:read:subscriptions` |
| `channel.cheer` | Bits cheer | `bits:read` |
| `channel.raid` | Incoming raid | None |
| `channel.follow` | New follow | `moderator:read:followers` |
| `channel.channel_points_custom_reward_redemption.add` | Channel point redemption | `channel:read:redemptions` |

**Creating a subscription (after WebSocket connect):**
```
POST https://api.twitch.tv/helix/eventsub/subscriptions
Authorization: Bearer <token>
Client-Id: <client_id>
Content-Type: application/json

{
  "type": "channel.chat.message",
  "version": "1",
  "condition": {
    "broadcaster_user_id": "12345",
    "user_id": "67890"
  },
  "transport": {
    "method": "websocket",
    "session_id": "<from welcome message>"
  }
}
```

**EventSub WebSocket limits:**
- Max 300 subscriptions per WebSocket connection
- Max 3 WebSocket connections per `user_id`
- Keepalive timeout: configurable, default 10 seconds
- Must handle `session_keepalive` messages; if missed for >timeout, reconnect
- Reconnect via `session_reconnect` message when Twitch requests it

### IRC vs EventSub comparison

| Factor | IRC | EventSub WebSocket |
|--------|-----|-------------------|
| Setup complexity | Simple | Moderate (API calls needed) |
| Chat messages | Yes | Yes |
| Bits/Cheers | Yes (via tags) | Yes |
| Subscriptions | Yes (USERNOTICE) | Yes |
| Gift subs | Yes | Yes |
| Follows | No | Yes |
| Channel points | No | Yes |
| Message metadata | Tags (parse manually) | Structured JSON |
| Library support | Mature (many libs) | Growing |
| Future-proof | IRC is legacy but stable | Preferred by Twitch |

**Recommendation**: Use **EventSub WebSocket** for new development. It's Twitch's preferred approach, provides structured JSON, and covers more event types. Fall back to IRC only if you need simpler initial implementation.

### Twitch Authentication

**OAuth 2.0 flows:**

1. **Authorization Code Flow** (recommended for apps acting on behalf of a user):
   - Redirect to `https://id.twitch.tv/oauth2/authorize`
   - Parameters: `client_id`, `redirect_uri`, `response_type=code`, `scope`
   - Exchange code at `https://id.twitch.tv/oauth2/token`
   - Returns `access_token` + `refresh_token`

2. **Client Credentials Flow** (app-only, no user context):
   - `POST https://id.twitch.tv/oauth2/token`
   - Parameters: `client_id`, `client_secret`, `grant_type=client_credentials`
   - Limited: cannot read chat, bits, or subs (no user context)

3. **Device Code Flow** (good for OBS plugins / TV apps):
   - `POST https://id.twitch.tv/oauth2/device`
   - Returns a `user_code` and `verification_uri` for user to visit
   - Poll `https://id.twitch.tv/oauth2/token` with `device_code`
   - Good UX for desktop plugin: show user a code, they authorize in browser

**Required scopes for credits plugin:**
- `user:read:chat` - Read chat messages (EventSub)
- `chat:read` - Read chat via IRC
- `bits:read` - Read bits/cheers
- `channel:read:subscriptions` - Read sub events
- `moderator:read:followers` - Read follow events

**Token validation:**
- Validate tokens periodically: `GET https://id.twitch.tv/oauth2/validate`
- Access tokens expire (typically 4 hours for user tokens)
- Use refresh tokens to get new access tokens

**App registration:**
- Register at https://dev.twitch.tv/console/apps
- Set redirect URI (for auth code flow) or use device code flow

---

## 3. Kick and Other Platforms

### Kick

As of early 2025, **Kick does NOT have a stable public API**. The situation:

- **No official public API documentation** published by Kick
- Community members have reverse-engineered the internal API:
  - Chat uses **Pusher WebSocket** protocol (a third-party real-time service)
  - Channel: `chatrooms.<chatroom_id>.v2`
  - Event: `App\Events\ChatMessageEvent`
  - WebSocket URL: `wss://ws-us2.pusher.com/app/<app_key>`
- These reverse-engineered endpoints are **unstable** and can break at any time
- There are unofficial community libraries (e.g., `kick-js`, `kick.py`) but they rely on these undocumented endpoints
- Kick has mentioned plans for an official API but timelines are unclear

**Recommendation**: Kick support should be considered **experimental/optional**. If implemented, isolate it behind a feature flag and expect maintenance burden from breaking changes. Monitor for official API announcements.

### Other platforms with chat APIs

**Facebook Live / Meta:**
- Graph API provides live video comments: `GET /{live-video-id}/comments`
- Requires Facebook app review for production use
- Page Access Token needed
- Polling-based, no WebSocket

**TikTok Live:**
- TikTok has a **Live Events API** (part of TikTok for Developers)
- WebSocket-based push notifications for gifts, likes, comments
- Requires TikTok developer account and app review
- Still relatively new and restrictive in access

**Rumble:**
- No official public API as of 2025
- Community reverse-engineering exists but is fragile

**Trovo:**
- Has a documented Chat API using WebSocket
- Relatively small platform but properly documented
- https://developer.trovo.live/

**Priority recommendation**: Focus on **YouTube + Twitch** first (covers vast majority of streamers), then add Kick as experimental, then other platforms based on user demand.

---

## 4. Integration Architecture for C/C++ OBS Plugin

### Option A: Direct API connections from the plugin

The plugin itself makes HTTP/WebSocket connections to each platform.

**Pros:**
- Single binary, no external dependencies
- Simple deployment

**Cons:**
- Complex C/C++ HTTP/WebSocket code
- OAuth flows awkward from a native plugin (need to open browser, handle redirects)
- Hard to maintain - API changes require plugin rebuilds
- C/C++ JSON parsing is verbose
- Concurrent connections to multiple platforms complicate threading

### Option B: Companion middleware service (RECOMMENDED)

A separate lightweight process (written in Node.js, Python, or Go) handles all API connections and exposes a simple local interface to the OBS plugin.

```
[YouTube API] ──\
[Twitch IRC/EventSub] ──> [Companion Service] ──> [OBS Plugin]
[Kick WebSocket] ──/          (localhost)         (C/C++)
```

**Pros:**
- Each platform's API client is written in a high-level language with mature HTTP/WebSocket/OAuth libraries
- Plugin stays simple: just reads aggregated data
- Can update API integrations without rebuilding the OBS plugin
- OAuth flows handled naturally (open browser, local HTTP callback server)
- Easier to test and debug each component independently

**Cons:**
- User must run a companion app (can be auto-launched by plugin)
- Two processes to manage

### Communication between companion service and OBS plugin

**Option B1: Local WebSocket server (RECOMMENDED)**

The companion service runs a WebSocket server on `ws://localhost:<port>`. The OBS plugin connects as a client.

```
Companion (WS Server on :9876) <──WebSocket──> OBS Plugin (WS Client)
```

- Real-time push of new chat events
- Bidirectional: plugin can request current participant list
- Use JSON messages
- OBS has `libwebsockets` available, or use a lightweight C WebSocket library
- obs-websocket (bundled with OBS 28+) proves WebSocket is viable in OBS context

**Option B2: File-based approach**

The companion writes a JSON file to disk; the plugin reads it periodically.

```
Companion ──writes──> participants.json ──reads──> OBS Plugin
```

- Simplest to implement
- No networking code in the plugin at all
- OBS plugin just reads a file on a timer (e.g., every 5 seconds)
- Risk: file locking issues, slight latency
- Good for MVP / prototyping

**Option B3: HTTP polling**

Companion runs an HTTP server; plugin polls it.

```
Companion (HTTP on :9876/api/participants) <──HTTP GET──> OBS Plugin
```

- Simple REST API
- Plugin uses libcurl (widely available) for HTTP requests
- Slightly more overhead than WebSocket for frequent updates

### Recommended architecture

```
┌─────────────────────────────────────────────────────┐
│  Companion Service (Node.js / Python / Go)          │
│                                                     │
│  ┌─────────────┐ ┌──────────────┐ ┌──────────────┐ │
│  │ YouTube     │ │ Twitch       │ │ Kick         │ │
│  │ Poller      │ │ EventSub WS  │ │ Pusher WS    │ │
│  └──────┬──────┘ └──────┬───────┘ └──────┬───────┘ │
│         │               │                │          │
│         v               v                v          │
│  ┌─────────────────────────────────────────────┐    │
│  │        Unified Participant Store            │    │
│  │   (in-memory, persisted to JSON on disk)    │    │
│  └──────────────────┬──────────────────────────┘    │
│                     │                               │
│         ┌───────────┴───────────┐                   │
│         v                       v                   │
│  ┌──────────────┐   ┌─────────────────────┐        │
│  │ WebSocket    │   │ File output         │        │
│  │ Server :9876 │   │ participants.json   │        │
│  └──────────────┘   └─────────────────────┘        │
│                                                     │
│  ┌─────────────────────────────────────────────┐    │
│  │  OAuth Token Manager                        │    │
│  │  (browser-based auth, token refresh)        │    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘
                    │                │
          WebSocket │          File  │
                    v                v
┌─────────────────────────────────────────────────────┐
│  OBS Plugin (C/C++)                                 │
│                                                     │
│  - Connects to companion via WS or reads file       │
│  - Maintains sorted participant list for rendering  │
│  - Renders credits roll using OBS graphics API      │
└─────────────────────────────────────────────────────┘
```

### Handling multiple platforms simultaneously

The companion service runs one connection handler per platform in parallel:
- **YouTube**: HTTP polling loop (respects `pollingIntervalMillis`)
- **Twitch**: Persistent WebSocket (EventSub) or IRC connection
- **Kick**: Persistent WebSocket (Pusher) connection

Each handler normalizes events into the unified format and feeds them into the shared participant store. The store is the single source of truth and handles deduplication (same user across platforms, if detectable).

### File-based MVP approach (simplest starting point)

For the initial implementation in C, the plugin can simply:
1. Read a JSON file path from OBS source properties
2. On a timer (every 2-5 seconds), read and parse the file
3. Render credits from the parsed data
4. The companion service (separate project) writes this file

This avoids any networking code in the C plugin entirely. Libraries like `cJSON` (single header) or `json-c` make JSON parsing in C straightforward.

---

## 5. Donation Tracking

### YouTube Super Chat

Covered above in Section 1. Key fields:
- `snippet.type == "superChatEvent"`
- `snippet.superChatDetails.amountMicros` (divide by 1,000,000)
- `snippet.superChatDetails.currency`
- `snippet.superChatDetails.amountDisplayString`

### Twitch Bits

**Via IRC**: Parse the `bits` tag from PRIVMSG:
```
@bits=500;display-name=User;... PRIVMSG #channel :Cheer500
```

**Via EventSub** (`channel.cheer`):
```json
{
  "user_id": "12345",
  "user_login": "username",
  "user_name": "Username",
  "broadcaster_user_id": "67890",
  "message": "Cheer500 Great stream!",
  "bits": 500
}
```

Bits to USD conversion: 100 bits = ~$1.40 USD (viewer cost), but the streamer receives $1.00 per 100 bits. For credits display, show the bits count or convert at the $0.01/bit rate the streamer receives.

### Twitch Subscriptions

**Via EventSub:**
- `channel.subscribe` - New sub (tier 1/2/3)
- `channel.subscription.gift` - Gift sub
- `channel.subscription.message` - Resub with message

Subscription tiers and approximate values:
- Tier 1: $4.99/month
- Tier 2: $9.99/month
- Tier 3: $24.99/month

### Streamlabs API

Streamlabs provides an API for accessing donation/tip data:

**Authentication**: OAuth 2.0
- Authorize: `https://streamlabs.com/api/v2.0/authorize`
- Token: `https://streamlabs.com/api/v2.0/token`
- Scopes: `donations.read`, `alerts.create`

**Get recent donations:**
```
GET https://streamlabs.com/api/v2.0/donations
Authorization: Bearer <token>
```

Response includes:
- `donation_id`, `name`, `amount`, `currency`, `message`, `created_at`

**Socket API for real-time events:**
```
https://sockets.streamlabs.com?token=<socket_token>
```
- Connect via Socket.IO
- Listen for event type `"event"` with `type: "donation"`
- Also receives follows, subs, bits, raids - already aggregated across platforms

**Streamlabs is particularly useful** because it aggregates donations from multiple sources (PayPal, credit card tips, etc.) that are NOT captured by platform-native APIs.

### StreamElements API

**Authentication**: JWT token (obtained from StreamElements dashboard) or OAuth.

**Get recent tips/donations:**
```
GET https://api.streamelements.com/kappa/v2/tips/{channelId}
Authorization: Bearer <JWT>
```

**WebSocket for real-time events:**
- Connect to StreamElements WebSocket
- Events include tips, subs, cheers, follows
- Uses Socket.IO protocol

**Get recent activity feed:**
```
GET https://api.streamelements.com/kappa/v2/activities/{channelId}
Authorization: Bearer <JWT>
```

### Normalizing donation data across platforms

All donations should be normalized to a common structure:

```json
{
  "donor_username": "User123",
  "platform": "youtube|twitch|streamlabs|streamelements",
  "type": "superchat|bits|subscription|tip|gift_sub",
  "amount_raw": 500,
  "amount_unit": "USD_cents|bits|tier",
  "amount_usd_cents": 500,
  "currency": "USD",
  "display_string": "$5.00",
  "message": "Great stream!",
  "timestamp": "2025-01-01T00:00:00Z"
}
```

**Currency conversion considerations:**
- YouTube Super Chats come in many currencies; use `amountDisplayString` for display or convert using a currency API
- Twitch Bits: use $0.01 per bit (streamer payout rate)
- Twitch Subs: use tier face value ($4.99/$9.99/$24.99)
- Streamlabs/StreamElements tips: already in specified currency

**Recommendation**: For credits, display the original amount and currency rather than converting. Show "$5.00" or "500 bits" or "Tier 1 Sub" as appropriate. Attempting currency normalization adds complexity and inaccuracy.

---

## 6. Unified Data Structure

### Stream Participants Schema

```json
{
  "stream_session": {
    "id": "session-uuid",
    "started_at": "2025-01-01T18:00:00Z",
    "ended_at": null,
    "platforms": ["youtube", "twitch"]
  },
  "participants": [
    {
      "id": "participant-uuid",
      "platform": "twitch",
      "platform_user_id": "12345",
      "username": "user123",
      "display_name": "User 123",
      "profile_image_url": "https://...",

      "roles": ["viewer", "chatter", "subscriber", "moderator"],

      "first_seen_at": "2025-01-01T18:05:00Z",
      "last_seen_at": "2025-01-01T20:30:00Z",

      "chat": {
        "message_count": 42,
        "first_message_at": "2025-01-01T18:05:00Z",
        "last_message_at": "2025-01-01T20:30:00Z"
      },

      "donations": [
        {
          "type": "bits",
          "amount_raw": 500,
          "amount_display": "500 bits",
          "amount_usd_cents": 500,
          "message": "Cheer500 Love the stream!",
          "timestamp": "2025-01-01T19:15:00Z"
        },
        {
          "type": "subscription",
          "amount_raw": 1,
          "amount_display": "Tier 1 Sub",
          "amount_usd_cents": 499,
          "message": "13 months!",
          "timestamp": "2025-01-01T18:30:00Z"
        }
      ],

      "donation_total_usd_cents": 999,

      "is_moderator": false,
      "is_subscriber": true,
      "is_vip": false
    }
  ],

  "summary": {
    "total_unique_chatters": 150,
    "total_messages": 3200,
    "total_donations_usd_cents": 45000,
    "total_new_subscribers": 5,
    "total_gift_subs": 10,
    "total_bits": 12500,
    "platforms": {
      "twitch": { "chatters": 120, "messages": 2800 },
      "youtube": { "chatters": 30, "messages": 400 }
    }
  }
}
```

### C struct equivalent (for the OBS plugin side)

```c
#define MAX_USERNAME_LEN 64
#define MAX_DISPLAY_LEN  128
#define MAX_PLATFORM_LEN 16

typedef struct donation_entry {
    char type[32];           /* "bits", "superchat", "sub", "tip" */
    char display[64];        /* "500 bits", "$5.00", "Tier 1 Sub" */
    int  amount_usd_cents;
    long timestamp_unix;
    struct donation_entry *next;
} donation_entry_t;

typedef struct participant {
    char platform[MAX_PLATFORM_LEN];
    char platform_user_id[64];
    char username[MAX_USERNAME_LEN];
    char display_name[MAX_DISPLAY_LEN];

    int  message_count;
    int  donation_total_usd_cents;

    bool is_moderator;
    bool is_subscriber;
    bool is_vip;

    long first_seen_unix;
    long last_seen_unix;

    donation_entry_t *donations;    /* linked list */
    struct participant *next;       /* linked list of all participants */
} participant_t;

typedef struct credits_data {
    participant_t *participants;     /* linked list head */
    int  total_chatters;
    int  total_messages;
    int  total_donations_usd_cents;
    long session_start_unix;
    long session_end_unix;
    pthread_mutex_t lock;           /* protect concurrent access */
} credits_data_t;
```

### Sorting for credits display

The plugin should support multiple sort orders for the credits roll:
1. **By donation amount** (descending) - Top donors first
2. **By message count** (descending) - Most active chatters first
3. **By first appearance** (ascending) - Order they arrived
4. **Alphabetical** - By display name
5. **By role** - Moderators, then subscribers, then regular chatters

### Credits roll sections (suggested)

```
=== SPECIAL THANKS ===
(Donors, sorted by amount descending)

=== MODERATORS ===
(Moderators who were active)

=== SUBSCRIBERS ===
(Subscribers who chatted)

=== COMMUNITY ===
(All other chatters, alphabetical)
```

---

## 7. Implementation Roadmap

### Phase 1: File-based MVP
1. Define the JSON schema for `participants.json`
2. OBS plugin reads and parses the file, renders credits
3. Build a simple companion script (Python or Node.js) that connects to ONE platform (Twitch IRC is easiest to start with)
4. Companion writes `participants.json` on a timer

### Phase 2: Multi-platform companion
1. Add YouTube live chat polling to companion
2. Add Streamlabs/StreamElements socket for donation aggregation
3. Add OAuth token management with browser-based auth flow
4. Persist tokens securely (OS keychain or encrypted file)

### Phase 3: WebSocket integration
1. Replace file-based communication with local WebSocket
2. Real-time updates to the credits display
3. Plugin can show live "currently chatting" overlay during stream

### Phase 4: Polish
1. Add Kick support (experimental)
2. Cross-platform identity merging (same person on YouTube + Twitch)
3. Configurable credits sections and sort orders in OBS properties
4. Export credits data for post-stream use

---

## Key Reference Links

- YouTube Data API v3 Live: https://developers.google.com/youtube/v3/live/docs
- YouTube Live Chat Messages: https://developers.google.com/youtube/v3/live/docs/liveChatMessages
- YouTube API Quota Calculator: https://developers.google.com/youtube/v3/determine_quota_cost
- Twitch IRC Guide: https://dev.twitch.tv/docs/irc/
- Twitch EventSub: https://dev.twitch.tv/docs/eventsub/
- Twitch Authentication: https://dev.twitch.tv/docs/authentication/
- Twitch EventSub Types: https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/
- Streamlabs API: https://developers.streamlabs.com/
- StreamElements API: https://docs.streamelements.com/
- OBS Plugin Template: https://github.com/obsproject/obs-plugintemplate
- cJSON (C JSON library): https://github.com/DaveGamble/cJSON
- libwebsockets: https://libwebsockets.org/
