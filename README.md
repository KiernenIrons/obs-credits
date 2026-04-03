# OBS Credits Plugin

A fully customizable scrolling credits source for OBS Studio with Discord and YouTube live chat integration. Add professional end credits to your streams - with live chatter names populated automatically.

![Preview](images/Preview.png)

## Features

- **Scrolling credits roll** - Smooth, GPU-accelerated vertical scroll with configurable speed
- **Dynamic sections** - Add/remove as many sections as you need, each fully customizable
- **Per-section styling** - Fonts, colors, alignment, outline, shadow, and spacing per section
- **Discord integration** - Auto-fetch server members by role on scene switch
- **YouTube live chat** - Collect unique chatter names during your stream (no API key needed)
- **Hotkeys** - Start/Restart, Pause/Resume, Stop via OBS hotkeys
- **Start & loop delays** - Configurable delays before scrolling and between loops
- **Flicker-free updates** - Two-layer rendering for seamless live data updates

## Screenshots

### Manual Sections
Add headings, sub-headings, names, and roles with individual font pickers and full styling controls.

![Manual Input](images/ManualInput.png)

### Discord Integration
Fetch server members by role ID. Auto-fetches when you switch to the credits scene.

![Discord Integration](images/DiscordIntegration.png)

### YouTube Live Chat
Paste your channel URL and chatters are collected automatically during your stream.

![YouTube Chatters](images/YoutubeChatters.png)

## Installation

### Windows

1. Download the latest `obs-credits-x.x.x-windows-x64-installer.exe` from [Releases](https://github.com/KiernenIrons/obs-credits/releases)
2. Close OBS Studio
3. Run the installer (requires admin)
4. Open OBS Studio and add a **"Credits"** source

## Usage

1. In OBS, click **+** under Sources
2. Select **Credits**
3. Add sections with headings, names, and roles
4. Customize fonts, colors, outline, shadow, and spacing per section
5. Optionally enable Discord and/or YouTube chat integration
6. Switch to the credits scene at the end of your stream

### Discord Setup

1. Create a bot at https://discord.com/developers/applications
2. Enable **Server Members Intent** (privileged)
3. Invite bot with scope `bot` and permission `1024`
4. In the plugin: enter Bot Token, Guild ID, and add Discord sections with Role IDs
5. Click **Fetch Discord Data** or switch to the scene (auto-fetches)

### YouTube Chat Setup

1. Enable **YouTube Chat Credits** in the plugin properties
2. Paste your channel URL (e.g. `https://youtube.com/@yourchannel`)
3. Click **Start Collecting Chatters** or just start streaming - it auto-detects
4. Unique chatter names populate live and clear at each stream start

### Hotkeys

Bind these in **OBS Settings > Hotkeys**:
- **Start/Restart Credits** - Reset and start scrolling
- **Pause/Resume Credits** - Freeze/unfreeze at current position
- **Stop Credits** - Stop and reset to beginning

## Building from Source

### Requirements

- OBS Studio 30+ (tested with 32.1.0)
- CMake 3.16+
- Visual Studio 2022 Build Tools (Windows)
- OBS source code (for headers)

### Build

```bash
cmake --preset default
cmake --build build --config Release
```

## Author

**Kiernen Irons** - Design, development, and maintenance.

Built with [Claude Code](https://claude.ai/code) (Claude Opus 4.6).

## License

See [LICENSE](LICENSE) for details.
