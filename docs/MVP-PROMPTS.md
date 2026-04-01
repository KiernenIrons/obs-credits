# OBS Credits Plugin — MVP Implementation Prompts

**Author:** Kiernen Irons
**Date:** 2026-04-01

Copy-paste each prompt into Claude Code in order. Each prompt is self-contained with full context so Claude doesn't have to guess intent. Wait for completion and verify the checklist before moving to the next prompt.

---

## How to Use This File

1. Run each prompt in sequence (Prompt 1, then 2, etc.)
2. Each prompt ends with `/code-check` — Claude will run the code review skill automatically
3. After `/code-check` passes, verify the manual checklist items before moving on
4. If `/code-check` reports CRITICAL issues, fix them before proceeding
5. Use `/commit` after each prompt to save your progress

---

## Prompt 1: Project Scaffold & Build System

```
I'm building an OBS Studio plugin called "obs-credits" — a scrolling credits roll source. Set up the project scaffold and build system. Here's exactly what to create:

FILES TO CREATE:
- CMakeLists.txt — CMake 3.16+, project name "obs-credits" version 0.1.0. Build a MODULE library from: src/plugin-main.c, src/credits-source.c, src/credits-parser.c, src/credits-renderer.c, deps/cJSON/cJSON.c. Include deps/cJSON/ as a private include dir. Link against OBS::libobs via find_package(libobs REQUIRED). Add a compile definition OBS_CREDITS_VERSION="${PROJECT_VERSION}". Add platform-specific install rules for the plugin DLL into the standard OBS plugin directory structure (bin/64bit/ on Windows, bin/ on macOS/Linux) and data/ alongside it.
- CMakePresets.json — Version 3, one preset named "default" with binaryDir "${sourceDir}/build" and CMAKE_BUILD_TYPE=Release.
- deps/cJSON/cJSON.c and deps/cJSON/cJSON.h — Download or create stubs for the cJSON library (MIT license, single-file JSON parser by Dave Gamble). If you can't download, create minimal placeholder files with a comment pointing to https://github.com/DaveGamble/cJSON for the real source.
- src/plugin-main.c — Minimal OBS module entry point with OBS_DECLARE_MODULE(), OBS_MODULE_USE_DEFAULT_LOCALE("obs-credits", "en-US"), obs_module_load() that calls obs_register_source(&credits_source_info) and logs "[obs-credits] Plugin loaded (version X)", and obs_module_unload() that logs unloaded.
- src/credits-source.h — Just declares `extern struct obs_source_info credits_source_info;`
- src/credits-source.c — Minimal stub: define credits_source_info with .id="obs_credits_roll", .type=OBS_SOURCE_TYPE_INPUT, .output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW, .get_name returning obs_module_text("CreditsRoll"). All other callbacks (create, destroy, update, tick, render, properties, defaults, get_width, get_height) should be stub functions that do nothing but won't crash. create should bzalloc a minimal struct and return it, destroy should bfree it.
- src/credits-parser.h — Empty header with just the #pragma once guard
- src/credits-parser.c — Empty implementation file
- src/credits-renderer.h — Empty header with just the #pragma once guard
- src/credits-renderer.c — Empty implementation file
- data/locale/en-US.ini — Locale strings: CreditsRoll="Credits Roll"
- .gitignore — Ignore build/, *.dll, *.so, *.dylib, *.obj, *.o, CMakeCache.txt, cmake_install.cmake, CMakeFiles/

IMPORTANT RULES:
- Use OBS allocators (bzalloc, bfree) not malloc/free
- Use blog() not printf
- Prefix all public symbols with credits_
- All user-visible strings through obs_module_text()
- Include proper OBS headers: <obs-module.h>

After creating all files, run /code-check to review everything.
```

### Verify before continuing:
- [ ] All files exist in the correct locations
- [ ] `credits_source_info` is declared in the header and defined in the .c
- [ ] `plugin-main.c` has `OBS_DECLARE_MODULE()` and registers the source
- [ ] `.gitignore` covers build artifacts

---

## Prompt 2: Credits Parser — Data Structures & JSON Loading

```
Implement the credits JSON parser for the obs-credits plugin. This reads a credits.json file and produces an in-memory data structure.

CONTEXT: The project scaffold already exists. src/credits-parser.h and src/credits-parser.c are empty stubs. The plugin uses cJSON (in deps/cJSON/) for JSON parsing and OBS allocators (bmalloc, bfree, bstrdup, bzalloc) for all memory.

FILE: src/credits-parser.h — Define these types:

enum credits_entry_type with values: CREDITS_ENTRY_NAME_ROLE, CREDITS_ENTRY_NAME_ONLY, CREDITS_ENTRY_IMAGE, CREDITS_ENTRY_SPACER, CREDITS_ENTRY_TEXT.

struct credits_entry with fields:
- enum credits_entry_type type
- char *name (for NAME_ROLE, NAME_ONLY)
- char *role (for NAME_ROLE)
- char *text (for TEXT)
- char *image_path (for IMAGE)
- int image_width, image_height
- int spacer_height (for SPACER)

struct credits_section with fields:
- char *heading
- char *heading_font (optional override, NULL = use default)
- int heading_font_size (0 = use default)
- uint32_t heading_color (0 = use default)
- char *alignment ("left", "center", "right", or NULL)
- struct credits_entry *entries
- size_t num_entries

struct credits_data with fields:
- struct credits_section *sections
- size_t num_sections

Declare: struct credits_data *credits_parse_file(const char *path); and void credits_data_free(struct credits_data *data);

FILE: src/credits-parser.c — Implement:

1. Static read_file() helper: opens file with fopen("rb"), seeks to get length, bmalloc's a buffer, reads content, null-terminates, returns it. Returns NULL on failure. Caller bfree's.

2. Static parse_entry_type(): maps string "name_role"/"name_only"/"image"/"spacer"/"text" to enum. Unknown types default to CREDITS_ENTRY_NAME_ONLY.

3. Static parse_entry(): reads a cJSON object into a credits_entry struct. Uses cJSON_GetObjectItem for each field, null-checks before bstrdup. Default spacer_height is 40.

4. Static parse_section(): reads heading, alignment, heading_style (nested object with font/size/color), and entries array. Color is a hex string like "#FFD700" — parse with strtoul(str+1, NULL, 16). bstrdup all strings.

5. credits_parse_file(): read_file, cJSON_Parse, get "sections" array, iterate with cJSON_ArrayForEach, parse each section. Log with blog(LOG_INFO) on success showing section count and filename. Log LOG_WARNING on any failure. Always cJSON_Delete the root. Return NULL on any error.

6. credits_data_free(): null-check, iterate all sections, free all entry strings (name, role, text, image_path), free entries array, free section strings (heading, heading_font, alignment), free sections array, free data.

ALSO CREATE: examples/credits.json with this content:
{
  "sections": [
    {
      "heading": "Directed By",
      "heading_style": { "font": "Georgia", "size": 56, "color": "#FFD700" },
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
      "heading_style": { "size": 48, "color": "#FF6B6B" },
      "entries": [
        { "type": "name_only", "name": "The Community" },
        { "type": "name_only", "name": "All the Viewers" },
        { "type": "spacer", "height": 30 },
        { "type": "text", "text": "Thank you for watching!" }
      ]
    }
  ]
}

IMPORTANT RULES:
- ALL memory allocation with bmalloc/bzalloc/bstrdup, ALL freeing with bfree
- NULL-check every cJSON return value before accessing
- blog() for all logging, never printf
- credits_data_free must handle NULL input gracefully
- No memory leaks: every bstrdup'd string must have a matching bfree in credits_data_free

After implementing, run /code-check to review for memory safety, null-check coverage, and OBS API compliance.
```

### Verify before continuing:
- [ ] Header has all structs and function declarations
- [ ] Parser handles missing/malformed JSON without crashing
- [ ] `credits_data_free(NULL)` is a no-op
- [ ] examples/credits.json is valid JSON

---

## Prompt 3: Credits Renderer — Layout Engine & Drawing

```
Implement the credits renderer for the obs-credits plugin. This takes parsed credits data and creates positioned, drawable elements using OBS private child sources.

CONTEXT: credits-parser.h defines struct credits_data/credits_section/credits_entry. The renderer creates OBS text sources as children and positions them vertically for a scrolling layout.

FILE: src/credits-renderer.h — Define:

Opaque struct: struct credits_layout; (forward declaration only, definition is in .c)

Functions:
- struct credits_layout *credits_renderer_build(const struct credits_data *data, uint32_t viewport_width, const char *default_font, int default_font_size, uint32_t heading_color, uint32_t text_color);
  — Builds layout from parsed data. Creates private child text sources and loads images. Must be called from the graphics thread.
- float credits_renderer_total_height(const struct credits_layout *layout);
- void credits_renderer_draw(const struct credits_layout *layout, uint32_t viewport_width, float scroll_y, float viewport_h);
  — Draws visible elements. Called inside an active gs_texrender. Only draws elements overlapping the visible window (viewport culling).
- void credits_renderer_free(struct credits_layout *layout);
  — Frees all child sources, textures, and layout memory. Must call obs_enter_graphics()/obs_leave_graphics() around graphics resource cleanup.

FILE: src/credits-renderer.c — Implement:

Internal types:
- enum layout_elem_type { ELEM_TEXT, ELEM_IMAGE, ELEM_SPACER }
- struct layout_elem { type, float y, float height, float x, obs_source_t *text_source, gs_image_file3_t *image, uint32_t image_width/height }
- struct credits_layout { struct layout_elem *elems, size_t num_elems, float total_height }

Helper: make_text_source(name, text, font_face, font_size, color)
- Creates obs_data_t for font (face, size, flags=0) and settings (text, font obj, color1, color2)
- Uses "text_gdiplus" on Windows (#ifdef _WIN32), "text_ft2_source" elsewhere
- Calls obs_source_create_private()
- Releases both obs_data_t objects before returning
- Returns the obs_source_t* (caller owns)

Helper: text_source_height/width()
- Returns obs_source_get_height/width(), with fallback of 40.0f/100.0f if 0 (source may not have rendered yet)

credits_renderer_build():
1. Count total elements: for each section, 1 heading + num_entries + 1 spacer
2. bzalloc the layout and elems array
3. Walk sections with a y_cursor starting at 0. For each section:
   a. Create heading text source with section's font/size/color overrides (fall back to defaults). Center horizontally. Advance y_cursor by height + entry_gap*2.
   b. For each entry, create appropriate element:
      - NAME_ROLE: single text source with "Name  —  Role" format, centered
      - NAME_ONLY: text source with just the name, centered
      - TEXT: text source with the text content, centered
      - IMAGE: gs_image_file3_init + gs_image_file3_init_texture, use specified or intrinsic dimensions, centered
      - SPACER: just height, no visual
      Advance y_cursor by height + entry_gap (8px)
   c. Add section gap spacer (60px) between sections
4. Set total_height = y_cursor
5. Log element count and total height
6. Return layout

Element name format: "credits_heading_N", "credits_entry_N_M", "credits_name_N_M", "credits_text_N_M" using snprintf with %zu format.

credits_renderer_draw():
- For each element, check if it's visible: skip if (e->y + e->height < scroll_y) or (e->y > scroll_y + viewport_h)
- ELEM_TEXT: gs_matrix_push, gs_matrix_translate3f(x, y, 0), obs_source_video_render(text_source), gs_matrix_pop
- ELEM_IMAGE: get default effect, set texture param, gs_matrix_push, translate, begin technique/pass, gs_draw_sprite, end pass/technique, gs_matrix_pop
- ELEM_SPACER: do nothing

credits_renderer_free():
- NULL check
- obs_enter_graphics()
- For each element: obs_source_release for text sources, gs_image_file3_free + bfree for images
- obs_leave_graphics()
- bfree elems array, bfree layout

IMPORTANT RULES:
- All gs_* calls are only valid on the graphics thread
- obs_source_create_private creates sources we own — we MUST obs_source_release them
- gs_image_file3_init_texture must be called in graphics context (it's called during build, which runs in video_tick on the graphics thread)
- credits_renderer_free wraps graphics cleanup in obs_enter/leave_graphics because it can be called from update (any thread)
- Use OBS allocators everywhere
- NULL-check layout parameter in all public functions

After implementing, run /code-check to review for graphics thread safety, ref counting correctness, and memory management.
```

### Verify before continuing:
- [ ] All text sources are created with `obs_source_create_private` and released in `free`
- [ ] Image loading uses `gs_image_file3` API correctly
- [ ] Viewport culling skips off-screen elements
- [ ] `credits_renderer_free(NULL)` is safe

---

## Prompt 4: Source Callbacks — Wiring Everything Together

```
Wire up the full credits source callbacks in src/credits-source.c. The parser and renderer are already implemented. This is the core file that ties everything together.

CONTEXT:
- src/credits-parser.h provides: credits_parse_file(), credits_data_free(), struct credits_data
- src/credits-renderer.h provides: credits_renderer_build(), credits_renderer_draw(), credits_renderer_total_height(), credits_renderer_free(), struct credits_layout
- The source is registered as obs_credits_roll with OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW

FILE: src/credits-source.c — Replace the stub implementation with the full version:

struct credits_source fields:
- obs_source_t *self (back-reference)
- char *credits_file, float scroll_speed, bool loop, uint32_t width/height
- char *default_font_face, int default_font_size, uint32_t heading_color/text_color
- struct credits_data *data, struct credits_layout *layout, gs_texrender_t *texrender
- float scroll_offset, float current_speed, float total_height, bool scrolling, bool started
- pthread_mutex_t mutex

Implement ALL callbacks:

credits_get_name: return obs_module_text("CreditsRoll")

credits_create: bzalloc the struct, store self=source, pthread_mutex_init, call credits_update(ctx, settings), return ctx

credits_destroy: lock mutex, free layout and data, unlock. obs_enter_graphics, gs_texrender_destroy, obs_leave_graphics. pthread_mutex_destroy. bfree credits_file, default_font_face, ctx.

credits_update (can be called from ANY thread):
- Read all settings: credits_file (string), scroll_speed (double), loop (bool), width/height (int), font obj (face string + size int), heading_color/text_color (int)
- Lock mutex
- bfree and bstrdup strings, store numeric values with sane defaults (speed>0 else 60, width>0 else 1920, height>0 else 1080, size>0 else 32)
- Free old data and layout
- Parse new credits file (if path is non-empty)
- Set layout=NULL (will be rebuilt on next video_tick since it needs graphics thread)
- Reset scroll: offset=0, current_speed=0, scrolling=true, started=false
- Unlock mutex

credits_get_defaults: scroll_speed=60.0, loop=false, width=1920, height=1080, heading_color=0xFFFFD700, text_color=0xFFFFFFFF

credits_video_tick(data, seconds):
- Lock mutex
- If data exists but layout is NULL: call credits_renderer_build (we're on graphics thread now), store total_height
- If scrolling and layout exists:
  - If not started: set scroll_offset = -(float)height (start below viewport), started=true
  - Smooth acceleration: current_speed += (scroll_speed - current_speed) * 5.0f * seconds
  - Advance: scroll_offset += current_speed * seconds
  - If scroll_offset > total_height: loop resets to -height, non-loop sets scrolling=false
- Unlock mutex

credits_video_render(data, effect):
- Lock mutex
- Early return if no layout. Create texrender on first call if NULL.
- gs_texrender_reset, gs_texrender_begin(width, height)
- gs_ortho(0, width, scroll_offset, scroll_offset+height, -1, 1)
- gs_clear with transparent vec4
- credits_renderer_draw(layout, width, scroll_offset, height)
- gs_texrender_end
- Get texture, draw with default effect: get "image" param, set texture, begin Draw technique/pass, gs_draw_sprite, end
- Unlock mutex
- UNUSED_PARAMETER(effect)

credits_get_width/get_height: return ctx->width / ctx->height

credits_get_properties:
- obs_properties_add_path for credits_file (OBS_PATH_FILE, "JSON files (*.json)")
- obs_properties_add_float for scroll_speed (10.0 to 500.0, step 5.0)
- obs_properties_add_bool for loop
- obs_properties_add_int for width (640-3840) and height (480-2160)
- obs_properties_add_font for font
- obs_properties_add_color for heading_color and text_color
- All labels use obs_module_text()

credits_source_info struct: wire all callbacks.

ALSO UPDATE data/locale/en-US.ini with all locale keys:
CreditsRoll="Credits Roll"
CreditsFile="Credits File"
ScrollSpeed="Scroll Speed (px/s)"
Loop="Loop"
Width="Width"
Height="Height"
DefaultFont="Default Font"
HeadingColor="Heading Color"
TextColor="Text Color"

IMPORTANT RULES:
- pthread_mutex protects data, layout, and scroll state — lock in update, tick, and render
- gs_texrender_create only in video_render (graphics thread)
- credits_renderer_build only in video_tick (graphics thread)
- credits_renderer_free is safe from any thread (it wraps graphics calls in obs_enter/leave_graphics)
- Never hold the mutex while calling obs_enter_graphics (deadlock risk) — the destroy callback handles this by unlocking before entering graphics
- All strings through obs_module_text()

After implementing, run /code-check to review for thread safety, deadlock risks, ref counting, and OBS API compliance.
```

### Verify before continuing:
- [ ] All 10 callbacks are implemented and wired into `credits_source_info`
- [ ] Mutex is locked in `update`, `video_tick`, and `video_render`
- [ ] `destroy` doesn't hold mutex while calling `obs_enter_graphics`
- [ ] All locale keys exist in `en-US.ini`
- [ ] Properties use `obs_module_text()` for all labels

---

## Prompt 5: Integration Test — Verify the Full Pipeline

```
Review the entire obs-credits plugin codebase for correctness and fix any issues. This is a pre-build integration check.

Read ALL of these files in order:
1. CMakeLists.txt
2. src/plugin-main.c
3. src/credits-source.h
4. src/credits-source.c
5. src/credits-parser.h
6. src/credits-parser.c
7. src/credits-renderer.h
8. src/credits-renderer.c
9. data/locale/en-US.ini
10. examples/credits.json

Check for and fix these issues:

COMPILATION:
- Missing #include directives (every file must include what it uses)
- Forward declaration mismatches between .h and .c
- Type mismatches (uint32_t vs int, float vs double casts)
- Missing UNUSED_PARAMETER() for unused callback parameters
- Platform-specific issues (#ifdef _WIN32 for text_gdiplus)

THREAD SAFETY:
- Mutex must be locked in update, video_tick, video_render
- credits_renderer_build called ONLY in video_tick (graphics thread)
- gs_texrender_create called ONLY in video_render (graphics thread)
- destroy must not deadlock: unlock mutex before obs_enter_graphics
- credits_renderer_free wraps graphics cleanup in obs_enter/leave_graphics

MEMORY:
- Every bstrdup has a matching bfree in destroy/update/free
- Every obs_source_create_private has a matching obs_source_release
- Every obs_data_create has a matching obs_data_release
- Every gs_image_file3_init has a matching gs_image_file3_free
- credits_data_free and credits_renderer_free handle NULL
- No dangling pointers after free (set to NULL)

OBS API:
- OBS_DECLARE_MODULE() present in plugin-main.c
- obs_source_info has all required fields (.id, .type, .get_name, .create, .destroy)
- obs_register_source called in obs_module_load
- All user strings use obs_module_text()
- Locale file has all referenced keys

DATA:
- examples/credits.json is valid JSON
- Parser handles empty sections array, missing fields, unknown entry types

Fix any issues you find. Do NOT add new features — only fix bugs and correctness issues.

After fixing everything, run /code-check for a final review.
```

### Verify before continuing:
- [ ] `/code-check` reports zero CRITICAL issues
- [ ] All includes are present and correct
- [ ] No compilation errors visible in the code
- [ ] Thread safety is sound
- [ ] Memory management is complete

---

## Prompt 6: Build Verification & Final Polish

```
Final polish pass on the obs-credits plugin. Read every source file and verify the following, fixing anything that's wrong:

1. HEADER GUARDS: Every .h file has #pragma once as the first line.

2. INCLUDE ORDER: In each .c file, includes should be ordered:
   - Own header first (e.g., credits-source.c includes credits-source.h first)
   - OBS headers (<obs-module.h>, <util/platform.h>, <graphics/image-file.h>)
   - Library headers ("cJSON.h")
   - Standard headers (<string.h>, <stdio.h>) only if needed

3. SYMBOL VISIBILITY: On Windows, OBS_DECLARE_MODULE() handles DLL exports. Verify no extra export macros are needed. On Linux/macOS, the module functions need to be visible — CMakeLists.txt should set CMAKE_C_VISIBILITY_PRESET to "default" or not hide symbols for MODULE targets (CMake handles this for MODULE libraries).

4. DEFENSIVE CODING in credits-source.c:
   - credits_update: if credits_file is empty string, treat as NULL (don't try to parse "")
   - credits_video_tick: if seconds <= 0 or > 1.0, clamp (protects against bad delta times)
   - credits_video_render: if width or height is 0, return early
   - credits_get_properties: verify all obs_properties_add_* calls match the setting names used in update/get_defaults

5. CMAKE COMPLETENESS:
   - Verify the install() commands produce the correct directory structure
   - Add a data/ install for locale files
   - Set C standard to C11: set(CMAKE_C_STANDARD 11)

6. CONSISTENT NAMING: All public functions prefixed with credits_. All static functions use descriptive names. All struct fields use snake_case.

7. LOG PREFIX: Every blog() call starts with "[obs-credits]".

Do not add features. Only fix issues, improve robustness, and ensure build correctness.

After all fixes, run /code-check for the final review.
```

### Final MVP verification:
- [ ] `/code-check` shows PASS with zero CRITICAL issues
- [ ] All 10 source files are clean and consistent
- [ ] CMakeLists.txt includes install rules and C11 standard
- [ ] The plugin would load in OBS and show "Credits Roll" as a source
- [ ] Scrolling credits render from a JSON file with headings, names, roles, text, images

---

## After All Prompts: Commit

Once all 6 prompts are complete and `/code-check` passes clean, run:

```
/commit
```

This will stage all files, generate a detailed commit message, and commit as Kiernen Irons.
