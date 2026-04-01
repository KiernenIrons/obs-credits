# OBS Credits Plugin

A customizable credits roll plugin for OBS Studio. Add professional, scrolling end credits to your streams and recordings — just like in movies and TV.

## Features

- **Scrolling credits roll** — Smooth, GPU-accelerated vertical scroll
- **Customizable text** — Set titles, names, roles, and sections via OBS properties
- **Styling options** — Font, size, color, alignment, and spacing controls
- **Speed control** — Adjustable scroll speed to match your content
- **Loop / one-shot modes** — Roll once and stop, or loop continuously
- **Localization ready** — All UI strings support translation

## Installation

### Windows

1. Download the latest release `.zip`
2. Extract to `%APPDATA%/obs-studio/plugins/obs-credits/`
3. Restart OBS Studio

Your folder structure should look like:
```
%APPDATA%/obs-studio/plugins/obs-credits/
├── bin/
│   └── 64bit/
│       └── obs-credits.dll
└── data/
    └── locale/
        └── en-US.ini
```

### macOS

1. Download the latest release `.tar.gz`
2. Extract to `~/Library/Application Support/obs-studio/plugins/obs-credits/`
3. Restart OBS Studio

### Linux

1. Download the latest release `.tar.gz`
2. Extract to `~/.config/obs-studio/plugins/obs-credits/`
3. Restart OBS Studio

## Usage

1. In OBS, click **+** under Sources
2. Select **Credits Roll**
3. Configure your credits text and styling in the source properties
4. Position and resize the source in your scene
5. Use Studio Mode or scene transitions to trigger the credits during your stream

## Building from Source

### Requirements

- OBS Studio 30+ (with development headers)
- CMake 3.16+
- C11/C++17 compiler
- Platform SDK (Windows SDK / macOS SDK / Linux dev packages)

### Build

```bash
cmake --preset default
cmake --build build --config Release
```

### Install locally

```bash
cmake --install build --config Release --prefix <obs-plugins-dir>
```

## Project Structure

```
obs-credits/
├── CMakeLists.txt           # Build configuration
├── CMakePresets.json         # CMake presets
├── src/
│   ├── plugin-main.c        # Plugin entry point
│   ├── credits-source.c     # Credits roll source implementation
│   └── credits-source.h     # Header
├── data/
│   └── locale/
│       └── en-US.ini        # English locale strings
├── .claude/
│   ├── settings.json        # Claude Code hooks & config
│   └── skills/
│       ├── code-check.md    # Pre-commit code review skill
│       └── commit.md        # Git commit skill
├── CLAUDE.md                # Development guidelines
└── README.md                # This file
```

## Configuration Options

| Property       | Type   | Description                          | Default       |
|---------------|--------|--------------------------------------|---------------|
| Credits Text  | text   | The full credits content (multiline) | —             |
| Font          | font   | Font family and style                | System default|
| Font Size     | int    | Text size in points                  | 36            |
| Text Color    | color  | Text color                           | White         |
| Background    | color  | Background color                     | Transparent   |
| Scroll Speed  | float  | Pixels per frame                     | 2.0           |
| Alignment     | list   | Left / Center / Right                | Center        |
| Loop          | bool   | Restart after finishing               | false         |

## Contributing

This plugin is developed by **Kiernen Irons**.

If you'd like to report a bug or request a feature, please open an issue on the repository.

## License

See [LICENSE](LICENSE) for details.

## Author

**Kiernen Irons** — Design, development, and maintenance.
