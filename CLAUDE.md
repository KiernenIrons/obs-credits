# Credits Plugin - Development Guide

## Author

**Kiernen Irons** is the sole author of this project. All commits must be attributed to Kiernen Irons. Do not use Co-Authored-By trailers or any other author name.

## Documentation

- `docs/RESEARCH.md` - Full technical research covering OBS API, Discord integration, YouTube/Twitch chat, donations, architecture decisions, data structures, and implementation roadmap.
- `docs/MVP-GUIDE.md` - Step-by-step implementation guide for the minimum viable product. Contains complete code for all 7 source files, build system, data structures, and a definition-of-done checklist.
- `docs/MVP-PROMPTS.md` - 6 sequential copy-paste prompts to build the MVP. Each prompt is self-contained, references specific files and structures, and ends with `/code-check`. Follow with `/commit` after each passes.
- `README.md` - User-facing plugin overview, installation, and usage.

## CLI Skills

- `/code-check` - Run a full code review of all staged/changed files. Checks for OBS API misuse, threading violations, memory leaks, and common pitfalls. Reports CRITICAL/WARNING/INFO issues with a PASS/FAIL verdict.
- `/commit` - Stage changes, generate a detailed commit message with a full change log, and commit as Kiernen Irons. Never amends previous commits.

## Hooks

- **Pre-commit code review** - Automatically runs before every `git commit`. An agent reviews all staged changes for critical OBS plugin issues (graphics thread violations, use-after-free, memory leaks, thread safety). Blocks the commit if critical issues are found.

---

# OBS Studio Plugin Development

## Project Overview

This is an OBS Studio plugin project written in C/C++. OBS (Open Broadcaster Software) plugins extend the functionality of OBS Studio by providing new sources, filters, outputs, encoders, or services.

## Build System

- **Build tool**: CMake (minimum 3.16)
- **Build command**: `cmake --preset default && cmake --build build --config Release`
- **Clean build**: `rm -rf build && cmake --preset default && cmake --build build --config Release`
- **Install**: `cmake --install build --config Release --prefix <obs-plugins-dir>`
- OBS plugins link against `libobs` and optionally `obs-frontend-api`
- Use the OBS Plugin Template (https://github.com/obsproject/obs-plugintemplate) as the canonical starting point

## Project Structure

```
├── CMakeLists.txt          # Top-level CMake config
├── CMakePresets.json        # CMake presets (default, CI)
├── src/
│   ├── plugin-main.c       # Plugin entry point (obs_module_load, obs_module_unload)
│   ├── <feature>.c/.cpp    # Source/filter/output implementations
│   └── <feature>.h         # Headers
├── data/
│   └── locale/
│       └── en-US.ini       # Localization strings
├── build/                   # Build output (gitignored)
└── CLAUDE.md
```

## OBS Plugin API Conventions

### Module Lifecycle

Every plugin must implement these exported functions:

```c
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("plugin-name", "en-US")

bool obs_module_load(void) {
    // Register sources, filters, outputs here
    return true;
}

void obs_module_unload(void) {
    // Cleanup
}
```

### Registering Sources/Filters

- Use `obs_register_source()` with an `obs_source_info` struct
- Source types: `OBS_SOURCE_TYPE_INPUT`, `OBS_SOURCE_TYPE_FILTER`, `OBS_SOURCE_TYPE_TRANSITION`
- Required callbacks: `.id`, `.type`, `.get_name`, `.create`, `.destroy`
- Optional but common: `.update`, `.video_render`, `.get_width`, `.get_height`, `.get_properties`, `.get_defaults`

### Properties UI

- Build UIs with `obs_properties_create()` and `obs_properties_add_*()` functions
- Never use platform-native GUI - always use the OBS properties API
- Property types: `bool`, `int`, `float`, `text`, `path`, `list`, `color`, `button`, `font`, `editable_list`, `group`

### Threading Rules

- **Graphics calls** (`gs_*`) must only happen on the graphics thread (inside `video_render` or wrapped in `obs_enter_graphics()`/`obs_leave_graphics()`)
- **UI property callbacks** run on the UI thread
- **`update` callback** can be called from any thread - protect shared state with mutexes
- Use `os_atomic_*` or `pthread_mutex_t` for thread safety, not platform-specific primitives

### Memory Management

- Use OBS allocators: `bmalloc`, `bfree`, `bstrdup`, `bzalloc` instead of standard malloc/free
- Use `obs_data_t` ref counting: `obs_data_addref` / `obs_data_release`
- Use `bfree()` for strings returned by OBS API calls that transfer ownership
- Sources, scenes, and scene items are reference-counted - always release with the appropriate `_release()` call

### Logging

- Use `blog(LOG_INFO, ...)`, `blog(LOG_WARNING, ...)`, `blog(LOG_ERROR, ...)` - never printf/stdout
- Prefix log messages with `[plugin-name]` for easy filtering

## Coding Standards

- C11 or C++17 depending on project needs; prefer C for simple plugins
- Use `snake_case` for functions and variables
- Prefix all public symbols with the plugin name to avoid collisions (e.g., `credits_source_create`)
- Keep platform-specific code behind `#ifdef _WIN32` / `#ifdef __APPLE__` / `#ifdef __linux__` guards
- All user-visible strings must go through `obs_module_text("StringKey")` for localization
- Do not use `extern "C"` blocks in .c files - only in .cpp files wrapping C API headers

## Common Pitfalls

- Never call `obs_source_release` on a source you don't own (didn't create or addref)
- Never access `obs_data_t` settings after releasing them
- Never call graphics functions outside the graphics thread - this will crash
- Filter plugins must call `obs_source_process_filter_begin` / `obs_source_process_filter_end` for video processing
- Always null-check return values from `obs_data_get_*` and `obs_source_get_*` families
- On Windows, DLLs must export `obs_module_load` - ensure `OBS_DECLARE_MODULE()` is present and `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` or a .def file is used

## Testing

- Manual testing: copy built `.dll`/`.so`/`.dylib` to OBS plugin directory and launch OBS
- Windows plugin path: `%APPDATA%/obs-studio/plugins/<name>/bin/64bit/`
- macOS plugin path: `~/Library/Application Support/obs-studio/plugins/<name>/bin/`
- Linux plugin path: `~/.config/obs-studio/plugins/<name>/bin/64bit/`
- Check OBS log file for load errors: Help -> Log Files -> View Current Log
- Use `blog()` liberally during development for tracing

## Dependencies

- OBS Studio 30+ (libobs, obs-frontend-api)
- CMake 3.16+
- Platform SDK (Windows SDK / macOS SDK / Linux dev packages)
- Optional: Qt 6 (only if building custom dock widgets via obs-frontend-api)

## Packaging & Distribution

- Use CPack or the OBS plugin installer framework
- Windows: ship as `.dll` in `bin/64bit/` alongside `data/` folder
- Include `data/locale/en-US.ini` for all user-facing text
- Version the plugin in CMakeLists.txt via `project(plugin-name VERSION x.y.z)`
