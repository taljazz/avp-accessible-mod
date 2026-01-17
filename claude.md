# AVP Accessibility Mod

This project adds comprehensive accessibility features to Aliens vs Predator (2000) Classic, making it playable for blind and visually impaired gamers.

## Project Structure

```
C:\Coding Projects\AVP access\
├── NakedAVP\                    # Source code (cloned from GitHub)
│   ├── src\
│   │   ├── accessibility.cpp    # Main accessibility implementation
│   │   ├── accessibility.h      # Accessibility API header
│   │   ├── main.c              # Game main loop (modified)
│   │   ├── menus.c             # Menu system (modified)
│   │   └── avp\
│   │       ├── missions.cpp    # Mission system (modified for objectives)
│   │       └── win95\frontend\
│   │           └── avp_menus.c # Menu accessibility hooks
│   ├── build\                  # CMake build directory
│   │   └── Release\
│   │       └── avp.exe         # Built executable
│   └── CMakeLists.txt          # Build configuration
└── claude.md                   # This documentation
```

## Building the Project

### Prerequisites

1. **Visual Studio 2022** with C++ development tools
2. **vcpkg** package manager installed at `C:\vcpkg`
3. **Dependencies** (install via vcpkg):
   ```cmd
   vcpkg install sdl3:x64-windows openal-soft:x64-windows
   ```

### Build Steps

1. Open a command prompt or PowerShell

2. Navigate to the build directory:
   ```cmd
   cd "C:\Coding Projects\AVP access\NakedAVP\build"
   ```

3. Configure with CMake (first time only):
   ```cmd
   cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   ```

4. Build the project:
   ```cmd
   cmake --build . --config Release
   ```

5. The built executable will be at:
   ```
   C:\Coding Projects\AVP access\NakedAVP\build\Release\avp.exe
   ```

### Deployment

Copy the built executable to the game folder:
```cmd
copy "C:\Coding Projects\AVP access\NakedAVP\build\Release\avp.exe" "C:\Program Files (x86)\Steam\steamapps\common\Aliens versus Predator Classic\avp_accessible.exe"
```

Then run `avp_accessible.exe` from the game folder.

## Accessibility Features

### Text-to-Speech (TTS)

Uses Windows SAPI (Speech API) for all voice announcements. Features:

- **Menu narration**: All menu items, sliders, toggles, and options are announced
- **State announcements**: Health, armor, weapon, and ammo changes
- **Weapon state tracking**: Automatic announcements for weapon switches, reloading, out of ammo, jammed
- **On-screen messages**: All game messages (pickups, events) automatically announced via TTS
- **Interaction prompts**: "Press SPACE to interact" when near switches, terminals, etc.
- **Mission objectives**: Current objectives read on demand
- **Interactive element scanning**: Nearby switches, doors, lifts, generators, and terminals announced with directions
- **Location awareness**: Current level/area announced
- **Redundancy prevention**: Same text won't be repeated consecutively

### Audio Radar System

Detects nearby enemies and plays **directional tones** (not TTS) to indicate their position:

- **Stereo panning**: Tone plays from left/right based on enemy direction relative to player facing
- **Pitch variation**:
  - Higher pitch = enemy is above you
  - Lower pitch = enemy is below you
  - Normal pitch = enemy at same level
- **Distance-based volume**: Closer enemies are louder
- **Tone type**: Soft sine wave (440Hz base, A4 note) with gentle envelope

### Pitch Indicator

Helps players understand their view orientation:

- **Sawtooth wave tone** plays when looking significantly up or down
- **Higher pitch** = looking up
- **Lower pitch** = looking down
- Plays periodically while maintaining non-level view angle
- Threshold: ~15 degrees from horizontal triggers the indicator

### Menu Accessibility

Complete menu system accessibility:

- **User profile selection**: Actual profile names announced
- **Episode selection**: Level names announced when navigating
- **Text sliders**: Label and current value (e.g., "Difficulty: Hard")
- **Numeric sliders**: Label and percentage (e.g., "Volume: 75 percent")
- **Toggle options**: Label and On/Off state
- **Number fields**: Label, value, and units
- **All other menu items**: Their text labels

### Autonavigation System

Assists blind players in navigating to objectives:

- **Target Types**: Cycle between interactive objects, NPCs, exits, or items
- **Directional Audio Tone**: 660Hz (E5 note) tone with stereo panning to indicate target direction
  - Left = turn left, Right = turn right, Center = facing target
- **Auto-Rotation**: Optionally rotate the player automatically toward the target
- **Auto-Movement**: Optionally move the player forward toward the target (at reduced speed for better control)
- **Auto-Jump**: Automatically jumps over low obstacles when auto-move is enabled
- **Distance Feedback**: Announcements include target distance (very close, close, medium range, far)
- **Target Announcements**: TTS describes what the target is and where it is

**How to Use:**
1. Press **Insert** to enable autonavigation
2. Press **Page Up** to cycle target types (interactive/NPC/exit/item)
3. Press **Page Down** to find the nearest target and hear its description
4. Listen to the directional tone - turn toward where it's centered
5. Optionally enable **Home** (auto-rotate) or **End** (auto-move) for assisted navigation

**Movement Speed:**
- Auto-movement uses ~40% of normal walking speed for precise navigation
- This allows time to react to announcements and stop near interactive objects
- Manual movement (WASD) still uses full speed

**Auto-Jump:**
- When auto-move is enabled and a jumpable obstacle is detected ahead (<2.5m)
- The player will automatically jump to clear the obstacle
- Only triggers for obstacles that require jumping (not steps that can be walked over)

**Obstacle Avoidance:**
- Autonavigation includes intelligent obstacle avoidance
- When a wall blocks the path to target, it automatically navigates around
- Stuck detection: if player isn't moving, it tries alternative routes
- Announces "Rerouting." when changing direction to avoid obstacles

### Obstruction Detection System

Real-time awareness of walls and obstacles:

- **Forward Detection**: Raycasts ahead to detect walls, steps, and obstacles
- **Obstacle Classification**:
  - **Step** (< 45cm): Can be walked over automatically
  - **Low obstacle** (45cm - 1.2m): Requires jumping
  - **Wall** (> 1.2m): Cannot be passed
- **Proximity Alerts**: Automatic warnings when very close to obstructions
- **Surroundings Scan**: On-demand check of all four directions
- **Distance Reporting**: Reports distance in millimeters for precision

**How to Use:**
- Press **Delete** to check what's directly ahead
- Press **Backslash** to hear distances in all directions
- Press **~** (Grave/Tilde) to toggle automatic wall alerts

## Keyboard Shortcuts

### Accessibility Keys (F1-F12)

| Key | Function |
|-----|----------|
| F1  | Toggle accessibility on/off |
| F2  | Toggle audio radar on/off |
| F3  | Announce current health and armor |
| F4  | Announce current weapon and ammo |
| F5  | Full radar scan (TTS announcement of all nearby threats) |
| F6  | Announce current location/level |
| F7  | Repeat last announcement |
| F8  | Toggle TTS on/off |
| F9  | Announce mission objectives (also scans for interactive elements) |
| F10 | Toggle pitch indicator on/off |
| F11 | Scan for interactive elements (switches, doors, lifts, generators, terminals) |
| F12 | Full status (health, armor, weapon, ammo, magazines) |

### Autonavigation Keys (Insert/Home/End/PgUp/PgDown)

| Key      | Function |
|----------|----------|
| Insert   | Toggle autonavigation on/off |
| Home     | Toggle auto-rotation toward target |
| End      | Toggle auto-movement toward target |
| Page Up  | Cycle target type (interactive/NPC/exit/item) |
| Page Down | Find nearest target and announce it |

### Obstruction Detection Keys (Del/Backslash/Grave)

| Key       | Function |
|-----------|----------|
| Delete    | Announce what's directly ahead (wall/step/obstacle/clear) |
| Backslash | Announce distances to walls in all four directions |
| ~ (Grave) | Toggle automatic obstruction alerts on/off |

### Automatic Announcements

The following are announced automatically without pressing any key:

- **Weapon switch**: "[Weapon name]. [X] rounds." when changing weapons
- **Reloading**: "Reloading." when reload starts
- **Out of ammo**: "Out of ammo!" when magazine empties
- **Weapon jammed**: "Weapon jammed!" if weapon jams
- **Pickups**: Weapon, ammo, health, and armor pickups announced
- **Interaction available**: "Switch nearby. Press SPACE to interact." when near operable objects
- **Low health warning**: "Warning! Health critical!" when health drops below 25%
- **Wall ahead**: "Wall ahead." when about to walk into a wall
- **Step ahead**: "Step ahead." when approaching a small obstacle that can be walked over
- **Low obstacle**: "Low obstacle. Jump." when approaching something that requires jumping

## Technical Implementation

### Files Modified

1. **accessibility.cpp** / **accessibility.h**
   - Core accessibility module
   - TTS using Windows SAPI COM interface
   - Audio radar with OpenAL 3D positioned tones
   - Pitch indicator with sawtooth wave generation
   - Player state tracking and announcements
   - Navigation cues
   - Interactive element scanning (switches, doors, lifts, generators, terminals)
   - Interaction detection ("Press SPACE" prompts)
   - Weapon state tracking (reload, empty, jammed)
   - On-screen message hook for automatic TTS
   - **Autonavigation system** with directional tones, auto-movement, and obstacle avoidance
   - **Obstruction detection** with raycasting, jumpable analysis, and proximity alerts

2. **main.c**
   - Added `#include "accessibility.h"`
   - Added `Accessibility_Init()` call at startup
   - Added update calls in game loop:
     - `AudioRadar_Update()`
     - `PlayerState_Update()`
     - `Navigation_Update()`
     - `PitchIndicator_Update()`
     - `Accessibility_CheckInteraction()`
     - `Accessibility_WeaponStateUpdate()`
     - `AutoNav_Update()`
     - `Obstruction_Update()`
     - `Accessibility_ProcessInput()`
   - Added `Accessibility_Shutdown()` at exit

3. **hud.c**
   - Modified `NewOnScreenMessage()` to call `Accessibility_OnScreenMessage()`
   - All on-screen messages now automatically announced via TTS

4. **avp_menus.c**
   - Added `Accessibility_AnnounceMenuSelection()` function
   - Comprehensive handling for all menu element types
   - Called on menu navigation and menu entry

5. **missions.cpp**
   - Added `GetMissionObjectivesText()` function
   - Returns all visible mission objectives as text

6. **CMakeLists.txt**
   - Added `src/accessibility.cpp` to sources
   - Added Windows libraries: `sapi`, `ole32`, `winmm`

### Audio System Details

**Radar Tone (Sine Wave)**
- Sample rate: 44100 Hz
- Duration: 150ms
- Base frequency: 440 Hz (A4)
- Soft attack/decay envelope (20% fade)
- 3D positioned using OpenAL

**Pitch Indicator Tone (Sawtooth Wave)**
- Sample rate: 44100 Hz
- Duration: 100ms
- Base frequency: 220 Hz (A3)
- Soft envelope (15% fade)
- Plays at listener position (not 3D)

### Interactive Element Scanning

Since mission objectives in AVP are text-based and don't have world positions, we provide directional guidance through interactive element scanning:

**Scanned Elements:**
- **Switches** (binary and link switches) - Often trigger doors or objectives
- **Doors** (proximity, lift, and switch doors) - Navigation waypoints
- **Lifts** - Vertical navigation
- **Generators** - Common mission objectives
- **Terminals/Databases** - Information or objective points

**How it works:**
- Scans all active strategy blocks in an extended range (2x radar range)
- Filters for interactive entity types only
- Sorts by distance (nearest first)
- Announces up to 8 elements with direction and distance
- Automatically called after mission objectives (F9) for context
- Can be called directly with F11

### Edge Case Handling

- Null pointer checks for Player, StrategyBlock, DynamicsBlock
- Empty string validation before TTS calls
- OpenAL error checking on buffer/source creation
- TTS initialization failure graceful degradation
- Redundancy tracking per announcement category

## Configuration

Default settings in `accessibility.cpp`:

```cpp
ACCESSIBILITY_CONFIG AccessibilitySettings = {
    1,      /* enabled */
    1,      /* tts_enabled */
    1,      /* audio_radar_enabled */
    1,      /* navigation_cues_enabled */
    1,      /* state_announcements_enabled */
    1,      /* menu_narration_enabled */
    500,    /* radar_update_interval_ms */
    5,      /* radar_max_enemies */
    50000,  /* radar_range (game units) */
    0,      /* tts_rate (-10 to 10) */
    100,    /* tts_volume (0-100) */
    1,      /* tts_interrupt */
    25,     /* health_warning_threshold % */
    10      /* ammo_warning_threshold */
};
```

## Notes

- The game requires original AVP data files from Steam
- Run the accessible exe from the game's installation folder
- Audio radar uses tones (not TTS) for real-time feedback
- F5 provides a detailed TTS scan for manual checks
- Mission objectives are text-based hints without specific positions
- F9 now also scans for nearby interactive elements to provide directional guidance
- F11 provides dedicated interactive element scanning for mission progression hints
