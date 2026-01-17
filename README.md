# AVP Accessibility Mod

A comprehensive accessibility modification for **Aliens vs Predator (2000) Classic**, making the game playable for blind and visually impaired gamers.

This mod adds text-to-speech narration, audio radar, autonavigation, and numerous other features to provide full accessibility to this classic game.

## Features

### Text-to-Speech (TTS)
- **Menu narration** - All menu items, sliders, toggles, and options announced
- **Game state** - Health, armor, weapon, and ammo changes announced
- **Weapon tracking** - Automatic announcements for weapon switches, reloading, out of ammo, jammed
- **On-screen messages** - All game messages (pickups, events) read aloud
- **Interaction prompts** - "Press SPACE to interact" when near switches, terminals, etc.
- **Mission objectives** - Current objectives read on demand

### Audio Radar
Directional audio tones indicate nearby enemy positions:
- **Stereo panning** - Enemies to your left/right indicated by sound direction
- **Pitch variation** - Higher pitch = above you, lower = below you
- **Distance-based volume** - Closer enemies are louder

### Autonavigation System
Assists players in navigating to objectives:
- **Directional tones** guide you toward targets
- **Auto-rotation** - Optionally turn automatically toward targets
- **Auto-movement** - Optionally move toward targets at reduced speed
- **Auto-jump** - Automatically jumps over low obstacles
- **Obstacle avoidance** - Intelligent pathfinding around walls

### Obstruction Detection
Real-time awareness of walls and obstacles:
- **Forward detection** - Raycasts detect walls, steps, and obstacles
- **Obstacle classification** - Steps, low obstacles (jumpable), and walls
- **Proximity alerts** - Automatic warnings when near obstructions
- **Surroundings scan** - Check all four directions on demand

### Pitch Indicator
Helps understand view orientation:
- Tone plays when looking significantly up or down
- Higher pitch = looking up, lower = looking down

## Keyboard Controls

### Accessibility (F1-F12)

| Key | Function |
|-----|----------|
| F1 | Toggle accessibility on/off |
| F2 | Toggle audio radar on/off |
| F3 | Announce health and armor |
| F4 | Announce weapon and ammo |
| F5 | Full radar scan (TTS) |
| F6 | Announce current location |
| F7 | Repeat last announcement |
| F8 | Toggle TTS on/off |
| F9 | Announce mission objectives |
| F10 | Toggle pitch indicator |
| F11 | Scan for interactive elements |
| F12 | Full status report |

### Autonavigation

| Key | Function |
|-----|----------|
| Insert | Toggle autonavigation on/off |
| Home | Toggle auto-rotation |
| End | Toggle auto-movement |
| Page Up | Cycle target type |
| Page Down | Find nearest target |

### Obstruction Detection

| Key | Function |
|-----|----------|
| Delete | Check what's ahead |
| Backslash | Distances in all directions |
| ~ (Grave) | Toggle automatic alerts |

## Requirements

- **Aliens vs Predator Classic 2000** from [Steam](https://store.steampowered.com/app/3730/) or [GOG](https://www.gog.com/game/aliens_versus_predator_classic_2000)
- Windows (uses Windows SAPI for text-to-speech)

## Installation

### Pre-built Release

1. Download the latest release
2. Copy `avp.exe` to your game folder (e.g., `C:\Program Files (x86)\Steam\steamapps\common\Aliens versus Predator Classic\`)
3. Run the executable from the game folder

### Building from Source

#### Prerequisites

- Visual Studio 2022 with C++ tools
- [vcpkg](https://vcpkg.io/) package manager
- Dependencies:
  ```cmd
  vcpkg install sdl3:x64-windows openal-soft:x64-windows
  ```

#### Build Steps

```cmd
cd NakedAVP
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

The executable will be at `NakedAVP/build/Release/avp.exe`.

## Credits & License

### Original Game

The original **Aliens vs Predator** source code is **copyright (c) 1999-2000 Rebellion Developments**.

This project is based on the [NakedAVP](http://icculus.org/avp/) source port.

### License Terms

```
The source code to Aliens Vs Predator is copyright (c) 1999-2000 Rebellion and
is provided as is with no warranty for its suitability for use. You may not
use this source code in full or in part for commercial purposes. Any use must
include a clearly visible credit to Rebellion as the creators and owners, and
reiteration of this license.

Any changes made after the fact are not copyright Rebellion and are provided
as is with no warranty for its suitability for use. You still may not use
this source code in full or in part for commercial purposes.
```

### This Modification

The accessibility features added by this mod are provided under the same non-commercial terms. This is a free, non-commercial accessibility project intended to make a classic game accessible to blind and visually impaired players.

### Acknowledgments

- **Rebellion Developments** - Original game and source code release
- **icculus.org / NakedAVP team** - Linux/macOS source port
- **Gibbon** - SDL3 implementation and cross-platform improvements

## Contributing

Contributions are welcome! Please ensure any contributions comply with the non-commercial license terms.

## Disclaimer

This is an unofficial modification. It is not affiliated with or endorsed by Rebellion Developments, Fox Interactive, or any rights holders of the Aliens vs Predator franchise.
