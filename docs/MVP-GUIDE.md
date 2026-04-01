# OBS Credits Plugin — MVP Implementation Guide

**Author:** Kiernen Irons
**Date:** 2026-04-01

This guide defines the minimum viable product: a working OBS source that scrolls credits with headings, names, roles, and images. No networking, no companion service, no Discord/chat integration yet. Those come in later phases. The goal is to get something visible in OBS as fast as possible, then iterate.

---

## What the MVP Does

1. User adds a "Credits Roll" source to their OBS scene
2. User points it at a `credits.json` file (or edits text directly in properties)
3. The source renders scrolling text: section headings, name/role pairs, images
4. User controls scroll speed, font, colors, and loop behavior from the properties panel
5. Credits scroll upward like a movie — one-shot or looping

**Explicitly NOT in the MVP:** Discord, live chat, donations, video clips, emoji images, companion service. Those are Phase 2+.

---

## File Structure

```
obs-credits/
├── CMakeLists.txt
├── CMakePresets.json
├── src/
│   ├── plugin-main.c          # Module entry point
│   ├── credits-source.c       # Source: create, destroy, update, tick, render, properties
│   ├── credits-source.h       # Public header
│   ├── credits-parser.c       # Load and parse credits.json via cJSON
│   ├── credits-parser.h
│   ├── credits-renderer.c     # Build and manage child text/image sources, layout math
│   └── credits-renderer.h
├── deps/
│   └── cJSON/
│       ├── cJSON.c
│       └── cJSON.h
├── data/
│   └── locale/
│       └── en-US.ini
├── examples/
│   └── credits.json           # Example credits file for users
├── docs/
│   ├── RESEARCH.md
│   └── MVP-GUIDE.md
├── .claude/
├── CLAUDE.md
└── README.md
```

---

## Step 1: Build System (CMakeLists.txt)

Set up CMake to produce an OBS plugin `.dll`/`.so`/`.dylib`.

```cmake
cmake_minimum_required(VERSION 3.16)
project(obs-credits VERSION 0.1.0)

# Find OBS libraries
find_package(libobs REQUIRED)

# Plugin shared library
add_library(obs-credits MODULE
    src/plugin-main.c
    src/credits-source.c
    src/credits-parser.c
    src/credits-renderer.c
    deps/cJSON/cJSON.c
)

target_include_directories(obs-credits PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/cJSON
)

target_link_libraries(obs-credits PRIVATE OBS::libobs)

# Install to OBS plugin directory structure
# (platform-specific install rules)
```

### CMakePresets.json

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "default",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      }
    }
  ]
}
```

### What to verify before moving on

- [ ] `cmake --preset default` succeeds (finds libobs)
- [ ] `cmake --build build` produces `obs-credits.dll` (or `.so`/`.dylib`)
- [ ] Copying the DLL into OBS plugins dir and launching OBS shows `[obs-credits]` in the log with no errors

---

## Step 2: Plugin Entry Point (plugin-main.c)

Minimal module that registers the credits source.

```c
#include <obs-module.h>
#include "credits-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-credits", "en-US")

bool obs_module_load(void)
{
    obs_register_source(&credits_source_info);
    blog(LOG_INFO, "[obs-credits] Plugin loaded (version %s)",
         OBS_CREDITS_VERSION);
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[obs-credits] Plugin unloaded");
}
```

Define `OBS_CREDITS_VERSION` from CMake:
```cmake
target_compile_definitions(obs-credits PRIVATE
    OBS_CREDITS_VERSION="${PROJECT_VERSION}"
)
```

### What to verify

- [ ] OBS log shows `[obs-credits] Plugin loaded (version 0.1.0)`
- [ ] "Credits Roll" appears in the Sources "+" menu

---

## Step 3: Source Skeleton (credits-source.h / credits-source.c)

### credits-source.h

```c
#pragma once
#include <obs-module.h>

extern struct obs_source_info credits_source_info;
```

### credits-source.c — The Core Struct

This is the central state for one instance of the credits source:

```c
#include "credits-source.h"
#include "credits-parser.h"
#include "credits-renderer.h"
#include <obs-module.h>
#include <util/platform.h>

struct credits_source {
    obs_source_t *self;            // back-reference to our obs_source_t

    /* Settings */
    char *credits_file;            // path to credits.json
    float scroll_speed;            // pixels per second
    bool loop;                     // loop when finished
    uint32_t width;                // source width in pixels
    uint32_t height;               // source height in pixels

    /* Font defaults */
    char *default_font_face;
    int default_font_size;
    uint32_t heading_color;        // ABGR packed
    uint32_t text_color;           // ABGR packed

    /* Parsed data */
    struct credits_data *data;     // parsed credits sections + entries

    /* Renderer */
    struct credits_layout *layout; // positioned elements ready to draw
    gs_texrender_t *texrender;     // offscreen render target

    /* Animation */
    float scroll_offset;           // current Y offset (pixels from top)
    float current_speed;           // smoothed scroll speed
    float total_height;            // total content height
    bool scrolling;                // is scroll active
    bool started;                  // has the scroll ever been triggered

    /* Thread safety */
    pthread_mutex_t mutex;         // protects data/layout during update
};
```

### Callback Implementations

```c
/* --- get_name --- */
static const char *credits_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("CreditsRoll");
}

/* --- create --- */
static void *credits_create(obs_data_t *settings, obs_source_t *source)
{
    struct credits_source *ctx = bzalloc(sizeof(struct credits_source));
    ctx->self = source;
    pthread_mutex_init(&ctx->mutex, NULL);

    // Load settings
    credits_update(ctx, settings);

    return ctx;
}

/* --- destroy --- */
static void credits_destroy(void *data)
{
    struct credits_source *ctx = data;

    pthread_mutex_lock(&ctx->mutex);
    credits_renderer_free(ctx->layout);
    credits_data_free(ctx->data);
    pthread_mutex_unlock(&ctx->mutex);

    obs_enter_graphics();
    gs_texrender_destroy(ctx->texrender);
    obs_leave_graphics();

    pthread_mutex_destroy(&ctx->mutex);
    bfree(ctx->credits_file);
    bfree(ctx->default_font_face);
    bfree(ctx);
}

/* --- update --- */
/* Called when user changes settings. Can be called from any thread. */
static void credits_update(void *data, obs_data_t *settings)
{
    struct credits_source *ctx = data;

    /* Read settings */
    const char *file = obs_data_get_string(settings, "credits_file");
    float speed      = (float)obs_data_get_double(settings, "scroll_speed");
    bool loop        = obs_data_get_bool(settings, "loop");
    int width        = (int)obs_data_get_int(settings, "width");
    int height       = (int)obs_data_get_int(settings, "height");

    /* Font defaults */
    obs_data_t *font = obs_data_get_obj(settings, "font");
    const char *face = font ? obs_data_get_string(font, "face") : "Arial";
    int size         = font ? (int)obs_data_get_int(font, "size") : 32;
    if (font) obs_data_release(font);

    uint32_t heading_color = (uint32_t)obs_data_get_int(settings, "heading_color");
    uint32_t text_color    = (uint32_t)obs_data_get_int(settings, "text_color");

    pthread_mutex_lock(&ctx->mutex);

    /* Store */
    bfree(ctx->credits_file);
    ctx->credits_file = bstrdup(file);
    ctx->scroll_speed = speed > 0 ? speed : 60.0f;
    ctx->loop = loop;
    ctx->width = width > 0 ? (uint32_t)width : 1920;
    ctx->height = height > 0 ? (uint32_t)height : 1080;
    bfree(ctx->default_font_face);
    ctx->default_font_face = bstrdup(face);
    ctx->default_font_size = size > 0 ? size : 32;
    ctx->heading_color = heading_color;
    ctx->text_color = text_color;

    /* Re-parse credits file */
    credits_data_free(ctx->data);
    ctx->data = NULL;
    if (ctx->credits_file && *ctx->credits_file) {
        ctx->data = credits_parse_file(ctx->credits_file);
        if (!ctx->data)
            blog(LOG_WARNING, "[obs-credits] Failed to parse: %s",
                 ctx->credits_file);
    }

    /* Mark layout for rebuild on next video_tick */
    credits_renderer_free(ctx->layout);
    ctx->layout = NULL;

    /* Reset scroll */
    ctx->scroll_offset = 0.0f;
    ctx->current_speed = 0.0f;
    ctx->scrolling = true;
    ctx->started = false;

    pthread_mutex_unlock(&ctx->mutex);
}

/* --- get_defaults --- */
static void credits_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_double(settings, "scroll_speed", 60.0);
    obs_data_set_default_bool(settings, "loop", false);
    obs_data_set_default_int(settings, "width", 1920);
    obs_data_set_default_int(settings, "height", 1080);
    obs_data_set_default_int(settings, "heading_color", 0xFFFFD700);
    obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF);
}

/* --- video_tick --- */
/* Called every frame on the graphics thread. seconds = delta time. */
static void credits_video_tick(void *data, float seconds)
{
    struct credits_source *ctx = data;

    pthread_mutex_lock(&ctx->mutex);

    /* Build layout on first tick after update (needs graphics thread) */
    if (ctx->data && !ctx->layout) {
        ctx->layout = credits_renderer_build(ctx->data,
            ctx->width, ctx->default_font_face,
            ctx->default_font_size, ctx->heading_color,
            ctx->text_color);
        if (ctx->layout)
            ctx->total_height = credits_renderer_total_height(ctx->layout);
    }

    /* Advance scroll */
    if (ctx->scrolling && ctx->layout) {
        if (!ctx->started) {
            ctx->scroll_offset = -(float)ctx->height; /* start below viewport */
            ctx->started = true;
        }

        /* Smooth acceleration toward target speed */
        float target = ctx->scroll_speed;
        ctx->current_speed +=
            (target - ctx->current_speed) * 5.0f * seconds;
        ctx->scroll_offset += ctx->current_speed * seconds;

        /* Check if we've scrolled past all content */
        if (ctx->scroll_offset > ctx->total_height) {
            if (ctx->loop) {
                ctx->scroll_offset = -(float)ctx->height;
            } else {
                ctx->scrolling = false;
            }
        }
    }

    pthread_mutex_unlock(&ctx->mutex);
}

/* --- video_render --- */
static void credits_video_render(void *data, gs_effect_t *effect)
{
    struct credits_source *ctx = data;

    pthread_mutex_lock(&ctx->mutex);

    if (!ctx->layout || !ctx->texrender) {
        /* Create texrender on first render */
        if (!ctx->texrender)
            ctx->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }

    /* Render all content into offscreen target */
    gs_texrender_reset(ctx->texrender);
    if (gs_texrender_begin(ctx->texrender, ctx->width, ctx->height)) {
        /* Orthographic projection with scroll offset */
        gs_ortho(0.0f, (float)ctx->width,
                 ctx->scroll_offset,
                 ctx->scroll_offset + (float)ctx->height,
                 -1.0f, 1.0f);

        /* Clear to transparent */
        struct vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);

        /* Draw all elements at absolute Y positions */
        credits_renderer_draw(ctx->layout, ctx->width,
                              ctx->scroll_offset, (float)ctx->height);

        gs_texrender_end(ctx->texrender);
    }

    /* Draw composited texture to screen */
    gs_texture_t *tex = gs_texrender_get_texture(ctx->texrender);
    if (tex) {
        gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *img = gs_effect_get_param_by_name(eff, "image");
        gs_effect_set_texture(img, tex);

        gs_technique_t *tech = gs_effect_get_technique(eff, "Draw");
        gs_technique_begin(tech);
        gs_technique_begin_pass(tech, 0);
        gs_draw_sprite(tex, 0, ctx->width, ctx->height);
        gs_technique_end_pass(tech);
        gs_technique_end(tech);
    }

    pthread_mutex_unlock(&ctx->mutex);

    UNUSED_PARAMETER(effect);
}

/* --- get_width / get_height --- */
static uint32_t credits_get_width(void *data)
{
    return ((struct credits_source *)data)->width;
}

static uint32_t credits_get_height(void *data)
{
    return ((struct credits_source *)data)->height;
}

/* --- get_properties --- */
static obs_properties_t *credits_get_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "credits_file",
        obs_module_text("CreditsFile"),
        OBS_PATH_FILE, "JSON files (*.json)", NULL);

    obs_properties_add_float(props, "scroll_speed",
        obs_module_text("ScrollSpeed"), 10.0, 500.0, 5.0);

    obs_properties_add_bool(props, "loop",
        obs_module_text("Loop"));

    obs_properties_add_int(props, "width",
        obs_module_text("Width"), 640, 3840, 1);
    obs_properties_add_int(props, "height",
        obs_module_text("Height"), 480, 2160, 1);

    obs_properties_add_font(props, "font",
        obs_module_text("DefaultFont"));

    obs_properties_add_color(props, "heading_color",
        obs_module_text("HeadingColor"));
    obs_properties_add_color(props, "text_color",
        obs_module_text("TextColor"));

    return props;
}

/* --- Registration --- */
struct obs_source_info credits_source_info = {
    .id             = "obs_credits_roll",
    .type           = OBS_SOURCE_TYPE_INPUT,
    .output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
    .get_name       = credits_get_name,
    .create         = credits_create,
    .destroy        = credits_destroy,
    .update         = credits_update,
    .video_tick     = credits_video_tick,
    .video_render   = credits_video_render,
    .get_width      = credits_get_width,
    .get_height     = credits_get_height,
    .get_properties = credits_get_properties,
    .get_defaults   = credits_get_defaults,
};
```

### What to verify

- [ ] Source appears as "Credits Roll" in OBS
- [ ] Opening properties shows all fields (file picker, speed, loop, font, colors)
- [ ] Changing a setting triggers `update` (log messages confirm)
- [ ] No crash on create/destroy cycle (add source, delete it, repeat)

---

## Step 4: Credits Parser (credits-parser.h / credits-parser.c)

Reads a `credits.json` file into an in-memory structure.

### credits-parser.h

```c
#pragma once
#include <obs-module.h>

/* Entry types */
enum credits_entry_type {
    CREDITS_ENTRY_NAME_ROLE,   /* name + role pair */
    CREDITS_ENTRY_NAME_ONLY,   /* centered name */
    CREDITS_ENTRY_IMAGE,       /* inline image */
    CREDITS_ENTRY_SPACER,      /* vertical space */
    CREDITS_ENTRY_TEXT,        /* freeform text block */
};

struct credits_entry {
    enum credits_entry_type type;
    char *name;                /* for NAME_ROLE, NAME_ONLY */
    char *role;                /* for NAME_ROLE */
    char *text;                /* for TEXT */
    char *image_path;          /* for IMAGE */
    int image_width;
    int image_height;
    int spacer_height;         /* for SPACER */
};

struct credits_section {
    char *heading;

    /* Optional per-section style overrides (0 = use default) */
    char *heading_font;
    int heading_font_size;
    uint32_t heading_color;
    char *alignment;           /* "left", "center", "right" */

    struct credits_entry *entries;
    size_t num_entries;
};

struct credits_data {
    struct credits_section *sections;
    size_t num_sections;
};

/* Parse credits.json file. Returns NULL on failure. Caller must free. */
struct credits_data *credits_parse_file(const char *path);

/* Free parsed data. */
void credits_data_free(struct credits_data *data);
```

### credits-parser.c — Core Logic

```c
#include "credits-parser.h"
#include "cJSON.h"
#include <util/platform.h>

/* Read entire file to string. Caller must bfree. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) { fclose(f); return NULL; }

    char *buf = bmalloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static enum credits_entry_type parse_entry_type(const char *type_str)
{
    if (!type_str) return CREDITS_ENTRY_NAME_ONLY;
    if (strcmp(type_str, "name_role") == 0) return CREDITS_ENTRY_NAME_ROLE;
    if (strcmp(type_str, "name_only") == 0) return CREDITS_ENTRY_NAME_ONLY;
    if (strcmp(type_str, "image") == 0)     return CREDITS_ENTRY_IMAGE;
    if (strcmp(type_str, "spacer") == 0)    return CREDITS_ENTRY_SPACER;
    if (strcmp(type_str, "text") == 0)      return CREDITS_ENTRY_TEXT;
    return CREDITS_ENTRY_NAME_ONLY;
}

static struct credits_entry parse_entry(const cJSON *json)
{
    struct credits_entry e = {0};
    const cJSON *type_j = cJSON_GetObjectItem(json, "type");
    e.type = parse_entry_type(cJSON_GetStringValue(type_j));

    const cJSON *name = cJSON_GetObjectItem(json, "name");
    if (cJSON_IsString(name)) e.name = bstrdup(name->valuestring);

    const cJSON *role = cJSON_GetObjectItem(json, "role");
    if (cJSON_IsString(role)) e.role = bstrdup(role->valuestring);

    const cJSON *text = cJSON_GetObjectItem(json, "text");
    if (cJSON_IsString(text)) e.text = bstrdup(text->valuestring);

    const cJSON *img = cJSON_GetObjectItem(json, "image_path");
    if (cJSON_IsString(img)) e.image_path = bstrdup(img->valuestring);

    const cJSON *iw = cJSON_GetObjectItem(json, "image_width");
    if (cJSON_IsNumber(iw)) e.image_width = iw->valueint;

    const cJSON *ih = cJSON_GetObjectItem(json, "image_height");
    if (cJSON_IsNumber(ih)) e.image_height = ih->valueint;

    const cJSON *sh = cJSON_GetObjectItem(json, "height");
    if (cJSON_IsNumber(sh)) e.spacer_height = sh->valueint;
    else e.spacer_height = 40;  /* default spacer */

    return e;
}

static struct credits_section parse_section(const cJSON *json)
{
    struct credits_section s = {0};

    const cJSON *heading = cJSON_GetObjectItem(json, "heading");
    if (cJSON_IsString(heading)) s.heading = bstrdup(heading->valuestring);

    const cJSON *align = cJSON_GetObjectItem(json, "alignment");
    if (cJSON_IsString(align)) s.alignment = bstrdup(align->valuestring);

    /* Optional heading_style overrides */
    const cJSON *style = cJSON_GetObjectItem(json, "heading_style");
    if (style) {
        const cJSON *font = cJSON_GetObjectItem(style, "font");
        if (cJSON_IsString(font)) s.heading_font = bstrdup(font->valuestring);

        const cJSON *size = cJSON_GetObjectItem(style, "size");
        if (cJSON_IsNumber(size)) s.heading_font_size = size->valueint;

        const cJSON *color = cJSON_GetObjectItem(style, "color");
        if (cJSON_IsString(color)) {
            /* Parse "#RRGGBB" or "#AARRGGBB" hex string */
            s.heading_color = (uint32_t)strtoul(color->valuestring + 1, NULL, 16);
        }
    }

    /* Entries array */
    const cJSON *entries = cJSON_GetObjectItem(json, "entries");
    if (cJSON_IsArray(entries)) {
        s.num_entries = (size_t)cJSON_GetArraySize(entries);
        s.entries = bzalloc(sizeof(struct credits_entry) * s.num_entries);
        size_t i = 0;
        const cJSON *entry;
        cJSON_ArrayForEach(entry, entries) {
            s.entries[i++] = parse_entry(entry);
        }
    }

    return s;
}

struct credits_data *credits_parse_file(const char *path)
{
    char *json_str = read_file(path);
    if (!json_str) {
        blog(LOG_WARNING, "[obs-credits] Cannot read file: %s", path);
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    bfree(json_str);
    if (!root) {
        blog(LOG_WARNING, "[obs-credits] JSON parse error: %s",
             cJSON_GetErrorPtr());
        return NULL;
    }

    const cJSON *sections_j = cJSON_GetObjectItem(root, "sections");
    if (!cJSON_IsArray(sections_j)) {
        blog(LOG_WARNING, "[obs-credits] 'sections' array not found");
        cJSON_Delete(root);
        return NULL;
    }

    struct credits_data *data = bzalloc(sizeof(struct credits_data));
    data->num_sections = (size_t)cJSON_GetArraySize(sections_j);
    data->sections = bzalloc(sizeof(struct credits_section) * data->num_sections);

    size_t i = 0;
    const cJSON *sec;
    cJSON_ArrayForEach(sec, sections_j) {
        data->sections[i++] = parse_section(sec);
    }

    cJSON_Delete(root);
    blog(LOG_INFO, "[obs-credits] Parsed %zu sections from %s",
         data->num_sections, path);
    return data;
}

void credits_data_free(struct credits_data *data)
{
    if (!data) return;
    for (size_t i = 0; i < data->num_sections; i++) {
        struct credits_section *s = &data->sections[i];
        bfree(s->heading);
        bfree(s->heading_font);
        bfree(s->alignment);
        for (size_t j = 0; j < s->num_entries; j++) {
            struct credits_entry *e = &s->entries[j];
            bfree(e->name);
            bfree(e->role);
            bfree(e->text);
            bfree(e->image_path);
        }
        bfree(s->entries);
    }
    bfree(data->sections);
    bfree(data);
}
```

### What to verify

- [ ] `credits_parse_file("examples/credits.json")` returns a valid struct
- [ ] Log shows `[obs-credits] Parsed N sections from ...`
- [ ] `credits_data_free` doesn't leak (test with AddressSanitizer if possible)
- [ ] Malformed JSON logs a warning and returns NULL (doesn't crash)

---

## Step 5: Renderer (credits-renderer.h / credits-renderer.c)

The renderer takes parsed data and creates positioned, drawable elements.

### credits-renderer.h

```c
#pragma once
#include "credits-parser.h"

struct credits_layout;

/*
 * Build a layout from parsed credits data.
 * Creates private child text sources and loads images.
 * Must be called from the graphics thread (during video_tick).
 */
struct credits_layout *credits_renderer_build(
    const struct credits_data *data,
    uint32_t viewport_width,
    const char *default_font,
    int default_font_size,
    uint32_t heading_color,
    uint32_t text_color);

/* Get total height of all content. */
float credits_renderer_total_height(const struct credits_layout *layout);

/*
 * Draw all visible elements.
 * Called from video_render inside an active texrender.
 * scroll_y = current scroll offset, viewport_h = visible height.
 * Only draws elements that overlap the visible window.
 */
void credits_renderer_draw(const struct credits_layout *layout,
                           uint32_t viewport_width,
                           float scroll_y, float viewport_h);

/* Free all child sources, textures, and layout memory. */
void credits_renderer_free(struct credits_layout *layout);
```

### credits-renderer.c — Key Design

Each "element" in the layout is one drawable thing at a specific Y position:

```c
#include "credits-renderer.h"
#include <obs-module.h>
#include <graphics/image-file.h>

enum layout_elem_type {
    ELEM_TEXT,    /* private text source */
    ELEM_IMAGE,   /* gs_image_file3 */
    ELEM_SPACER,  /* empty gap */
};

struct layout_elem {
    enum layout_elem_type type;
    float y;                      /* absolute Y position */
    float height;                 /* element height */
    float x;                      /* X position (for alignment) */

    obs_source_t *text_source;    /* for ELEM_TEXT */
    gs_image_file3_t *image;      /* for ELEM_IMAGE */
    uint32_t image_width;
    uint32_t image_height;
};

struct credits_layout {
    struct layout_elem *elems;
    size_t num_elems;
    float total_height;
};
```

### Building the Layout

The build function walks through sections and entries, creating elements and stacking them vertically:

```c
/*
 * Helper: create a private text source with given text, font, size, color.
 * Returns the obs_source_t* (caller owns it).
 */
static obs_source_t *make_text_source(const char *name,
    const char *text, const char *font_face, int font_size,
    uint32_t color)
{
    obs_data_t *font_data = obs_data_create();
    obs_data_set_string(font_data, "face", font_face);
    obs_data_set_int(font_data, "size", font_size);
    obs_data_set_int(font_data, "flags", 0);  /* 1=bold, 2=italic */

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", text);
    obs_data_set_obj(settings, "font", font_data);
    obs_data_set_int(settings, "color1", color);
    obs_data_set_int(settings, "color2", color);

    /* Use text_ft2_source for cross-platform, text_gdiplus for Windows */
#ifdef _WIN32
    const char *text_id = "text_gdiplus";
#else
    const char *text_id = "text_ft2_source";
#endif

    obs_source_t *src = obs_source_create_private(text_id, name, settings);

    obs_data_release(font_data);
    obs_data_release(settings);
    return src;
}

/* Get the rendered height of a text source (may be 0 first frame). */
static float text_source_height(obs_source_t *src)
{
    uint32_t h = obs_source_get_height(src);
    return h > 0 ? (float)h : 40.0f;  /* fallback if not yet rendered */
}

static float text_source_width(obs_source_t *src)
{
    return (float)obs_source_get_width(src);
}
```

The main build function:

```c
struct credits_layout *credits_renderer_build(
    const struct credits_data *data,
    uint32_t viewport_width,
    const char *default_font,
    int default_font_size,
    uint32_t heading_color,
    uint32_t text_color)
{
    if (!data || data->num_sections == 0)
        return NULL;

    /* Count total elements (heading + entries per section + spacers) */
    size_t total = 0;
    for (size_t i = 0; i < data->num_sections; i++) {
        total++;  /* heading */
        total += data->sections[i].num_entries;
        total++;  /* spacer between sections */
    }

    struct credits_layout *layout = bzalloc(sizeof(struct credits_layout));
    layout->elems = bzalloc(sizeof(struct layout_elem) * total);
    layout->num_elems = 0;

    float y_cursor = 0.0f;
    float section_gap = 60.0f;
    float entry_gap = 8.0f;
    char elem_name[256];

    for (size_t s = 0; s < data->num_sections; s++) {
        const struct credits_section *sec = &data->sections[s];

        /* --- Section heading --- */
        if (sec->heading && *sec->heading) {
            const char *h_font = sec->heading_font
                ? sec->heading_font : default_font;
            int h_size = sec->heading_font_size > 0
                ? sec->heading_font_size : (int)(default_font_size * 1.5f);
            uint32_t h_color = sec->heading_color
                ? sec->heading_color : heading_color;

            snprintf(elem_name, sizeof(elem_name),
                     "credits_heading_%zu", s);

            struct layout_elem *e = &layout->elems[layout->num_elems++];
            e->type = ELEM_TEXT;
            e->text_source = make_text_source(
                elem_name, sec->heading, h_font, h_size, h_color);
            e->y = y_cursor;
            e->height = text_source_height(e->text_source);
            /* Center heading */
            e->x = ((float)viewport_width
                    - text_source_width(e->text_source)) / 2.0f;

            y_cursor += e->height + entry_gap * 2;
        }

        /* --- Entries --- */
        for (size_t j = 0; j < sec->num_entries; j++) {
            const struct credits_entry *ent = &sec->entries[j];
            struct layout_elem *e = &layout->elems[layout->num_elems++];

            switch (ent->type) {
            case CREDITS_ENTRY_NAME_ROLE: {
                /* "Name .... Role" as a single text source for now */
                char buf[512];
                snprintf(buf, sizeof(buf), "%s  —  %s",
                         ent->name ? ent->name : "",
                         ent->role ? ent->role : "");
                snprintf(elem_name, sizeof(elem_name),
                         "credits_entry_%zu_%zu", s, j);
                e->type = ELEM_TEXT;
                e->text_source = make_text_source(
                    elem_name, buf, default_font,
                    default_font_size, text_color);
                e->y = y_cursor;
                e->height = text_source_height(e->text_source);
                e->x = ((float)viewport_width
                        - text_source_width(e->text_source)) / 2.0f;
                break;
            }
            case CREDITS_ENTRY_NAME_ONLY: {
                snprintf(elem_name, sizeof(elem_name),
                         "credits_name_%zu_%zu", s, j);
                e->type = ELEM_TEXT;
                e->text_source = make_text_source(
                    elem_name, ent->name ? ent->name : "",
                    default_font, default_font_size, text_color);
                e->y = y_cursor;
                e->height = text_source_height(e->text_source);
                e->x = ((float)viewport_width
                        - text_source_width(e->text_source)) / 2.0f;
                break;
            }
            case CREDITS_ENTRY_TEXT: {
                snprintf(elem_name, sizeof(elem_name),
                         "credits_text_%zu_%zu", s, j);
                e->type = ELEM_TEXT;
                e->text_source = make_text_source(
                    elem_name, ent->text ? ent->text : "",
                    default_font, default_font_size, text_color);
                e->y = y_cursor;
                e->height = text_source_height(e->text_source);
                e->x = ((float)viewport_width
                        - text_source_width(e->text_source)) / 2.0f;
                break;
            }
            case CREDITS_ENTRY_IMAGE: {
                e->type = ELEM_IMAGE;
                e->image = bzalloc(sizeof(gs_image_file3_t));
                gs_image_file3_init(e->image, ent->image_path,
                    GS_IMAGE_ALPHA_PREMULTIPLY_SRGB);
                gs_image_file3_init_texture(e->image);
                e->image_width = ent->image_width > 0
                    ? (uint32_t)ent->image_width
                    : e->image->image2.image.cx;
                e->image_height = ent->image_height > 0
                    ? (uint32_t)ent->image_height
                    : e->image->image2.image.cy;
                e->y = y_cursor;
                e->height = (float)e->image_height;
                e->x = ((float)viewport_width - (float)e->image_width) / 2.0f;
                break;
            }
            case CREDITS_ENTRY_SPACER: {
                e->type = ELEM_SPACER;
                e->y = y_cursor;
                e->height = ent->spacer_height > 0
                    ? (float)ent->spacer_height : 40.0f;
                break;
            }
            }

            y_cursor += e->height + entry_gap;
        }

        /* Gap between sections */
        struct layout_elem *gap = &layout->elems[layout->num_elems++];
        gap->type = ELEM_SPACER;
        gap->y = y_cursor;
        gap->height = section_gap;
        y_cursor += section_gap;
    }

    layout->total_height = y_cursor;
    blog(LOG_INFO, "[obs-credits] Layout: %zu elements, %.0f px total height",
         layout->num_elems, layout->total_height);
    return layout;
}
```

### Drawing

```c
void credits_renderer_draw(const struct credits_layout *layout,
                           uint32_t viewport_width,
                           float scroll_y, float viewport_h)
{
    if (!layout) return;

    float view_top = scroll_y;
    float view_bottom = scroll_y + viewport_h;

    for (size_t i = 0; i < layout->num_elems; i++) {
        const struct layout_elem *e = &layout->elems[i];

        /* Cull: skip elements entirely outside the viewport */
        if (e->y + e->height < view_top || e->y > view_bottom)
            continue;

        switch (e->type) {
        case ELEM_TEXT:
            if (e->text_source) {
                gs_matrix_push();
                gs_matrix_translate3f(e->x, e->y, 0.0f);
                obs_source_video_render(e->text_source);
                gs_matrix_pop();
            }
            break;

        case ELEM_IMAGE:
            if (e->image && e->image->image2.image.texture) {
                gs_effect_t *eff =
                    obs_get_base_effect(OBS_EFFECT_DEFAULT);
                gs_eparam_t *img =
                    gs_effect_get_param_by_name(eff, "image");
                gs_effect_set_texture(
                    img, e->image->image2.image.texture);

                gs_matrix_push();
                gs_matrix_translate3f(e->x, e->y, 0.0f);

                gs_technique_t *tech =
                    gs_effect_get_technique(eff, "Draw");
                gs_technique_begin(tech);
                gs_technique_begin_pass(tech, 0);
                gs_draw_sprite(e->image->image2.image.texture, 0,
                               e->image_width, e->image_height);
                gs_technique_end_pass(tech);
                gs_technique_end(tech);

                gs_matrix_pop();
            }
            break;

        case ELEM_SPACER:
            break; /* nothing to draw */
        }
    }
}
```

### Cleanup

```c
float credits_renderer_total_height(const struct credits_layout *layout)
{
    return layout ? layout->total_height : 0.0f;
}

void credits_renderer_free(struct credits_layout *layout)
{
    if (!layout) return;

    obs_enter_graphics();
    for (size_t i = 0; i < layout->num_elems; i++) {
        struct layout_elem *e = &layout->elems[i];
        if (e->text_source)
            obs_source_release(e->text_source);
        if (e->image) {
            gs_image_file3_free(e->image);
            bfree(e->image);
        }
    }
    obs_leave_graphics();

    bfree(layout->elems);
    bfree(layout);
}
```

### What to verify

- [ ] Layout builds with correct total height logged
- [ ] Credits scroll smoothly in OBS preview
- [ ] Headings render in larger/colored text, names render in default style
- [ ] Images render at correct size and centered position
- [ ] Viewport culling works (elements offscreen aren't drawn — check GPU usage)
- [ ] Destroying the source frees all child sources and textures cleanly

---

## Step 6: Locale Strings (data/locale/en-US.ini)

```ini
CreditsRoll="Credits Roll"
CreditsFile="Credits File"
ScrollSpeed="Scroll Speed (px/s)"
Loop="Loop"
Width="Width"
Height="Height"
DefaultFont="Default Font"
HeadingColor="Heading Color"
TextColor="Text Color"
```

---

## Step 7: Example Credits File (examples/credits.json)

```json
{
  "sections": [
    {
      "heading": "Directed By",
      "heading_style": {
        "font": "Georgia",
        "size": 56,
        "color": "#FFD700"
      },
      "entries": [
        { "type": "name_role", "name": "Kiernen Irons", "role": "Director & Developer" }
      ]
    },
    {
      "heading": "Cast",
      "entries": [
        { "type": "name_role", "name": "Alice", "role": "Lead" },
        { "type": "name_role", "name": "Bob", "role": "Supporting" },
        { "type": "name_role", "name": "Charlie", "role": "Extra" }
      ]
    },
    {
      "heading": "Special Thanks",
      "heading_style": {
        "size": 48,
        "color": "#FF6B6B"
      },
      "entries": [
        { "type": "name_only", "name": "The Community" },
        { "type": "name_only", "name": "All the Viewers" },
        { "type": "spacer", "height": 30 },
        { "type": "text", "text": "Thank you for watching!" }
      ]
    }
  ]
}
```

---

## MVP Checklist (Definition of Done)

All of these must pass before moving to Phase 2:

- [ ] **Builds** on Windows with MSVC + CMake (Linux/macOS are a bonus)
- [ ] **Loads** in OBS 30+ without errors in the log
- [ ] **Appears** as "Credits Roll" in the Sources menu
- [ ] **Renders** headings, name/role pairs, name-only entries, freeform text, images
- [ ] **Scrolls** smoothly at configurable speed, frame-rate independent
- [ ] **Loops** or stops based on the Loop toggle
- [ ] **Starts from below** — content scrolls up into view from the bottom
- [ ] **Properties** work: file picker, scroll speed, loop, width/height, font, colors
- [ ] **Hot-reload** — changing the credits file or settings updates the scroll live
- [ ] **No crashes** — create/destroy cycles, empty files, missing files, malformed JSON all handled
- [ ] **No leaks** — all child sources released, all textures freed, all strings bfree'd
- [ ] **No thread violations** — all `gs_*` calls on graphics thread, mutex protects shared state
- [ ] **Locale** — all UI strings go through `obs_module_text()`

---

## What Comes Next (Phase 2 Preview)

Once the MVP is stable, the next priorities are:

1. **Video clips** — add `CREDITS_ENTRY_CLIP` type, create `ffmpeg_source` children
2. **Emoji** — test font-native emoji, fall back to image sprites if needed
3. **Discord** — add libcurl + cJSON calls on a background thread, "Fetch" button
4. **Better layout** — two-column name/role alignment, dot leaders, left/right alignment
5. **Hotkey** — register an OBS hotkey to start/stop/reset the scroll

Each of these is additive and doesn't change the core architecture established in the MVP.
