/*
 * AVP Accessibility Module
 *
 * Provides accessibility features for blind and visually impaired players:
 * - Text-to-Speech (TTS) using Windows SAPI
 * - Audio radar for enemy/item detection
 * - Player state announcements (health, ammo, weapons)
 * - Navigation audio cues
 * - Menu narration
 *
 * Copyright (c) 2024 - Accessibility modifications
 * Based on NakedAVP source code
 */

#ifndef _ACCESSIBILITY_H_
#define _ACCESSIBILITY_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * Configuration
 * ============================================ */

/* Enable/disable accessibility features */
typedef struct AccessibilityConfig {
    int enabled;                    /* Master enable/disable */
    int tts_enabled;                /* Text-to-speech */
    int audio_radar_enabled;        /* Audio radar for entities */
    int navigation_cues_enabled;    /* Wall/door audio cues */
    int state_announcements_enabled; /* Health/ammo/weapon announcements */
    int menu_narration_enabled;     /* Menu text narration */

    /* Audio radar settings */
    int radar_update_interval_ms;   /* How often to update radar (default: 500ms) */
    int radar_max_enemies;          /* Max enemies to announce (default: 5) */
    int radar_range;                /* Detection range in game units */

    /* TTS settings */
    int tts_rate;                   /* Speech rate (-10 to 10, default: 0) */
    int tts_volume;                 /* Volume (0-100, default: 100) */
    int tts_interrupt;              /* Interrupt previous speech (default: 1) */

    /* State announcement thresholds */
    int health_warning_threshold;   /* Announce when health below this % */
    int ammo_warning_threshold;     /* Announce when ammo below this count */

} ACCESSIBILITY_CONFIG;

extern ACCESSIBILITY_CONFIG AccessibilitySettings;

/* ============================================
 * Initialization and Shutdown
 * ============================================ */

/* Initialize the accessibility system - call once at game startup */
int Accessibility_Init(void);

/* Shutdown the accessibility system - call at game exit */
void Accessibility_Shutdown(void);

/* Check if accessibility is available and initialized */
int Accessibility_IsAvailable(void);

/* ============================================
 * Text-to-Speech (TTS) Functions
 * ============================================ */

/* Speak text immediately (interrupts current speech if configured) */
void TTS_Speak(const char* text);

/* Speak text without interrupting current speech (queued) */
void TTS_SpeakQueued(const char* text);

/* Speak with priority (always interrupts) */
void TTS_SpeakPriority(const char* text);

/* Stop all current speech */
void TTS_Stop(void);

/* Check if TTS is currently speaking */
int TTS_IsSpeaking(void);

/* Set speech rate (-10 to 10) */
void TTS_SetRate(int rate);

/* Set speech volume (0-100) */
void TTS_SetVolume(int volume);

/* ============================================
 * Audio Radar System
 * ============================================ */

/* Entity types for audio radar */
typedef enum {
    RADAR_ENTITY_ALIEN,
    RADAR_ENTITY_PREDATOR,
    RADAR_ENTITY_MARINE,
    RADAR_ENTITY_FACEHUGGER,
    RADAR_ENTITY_XENOBORG,
    RADAR_ENTITY_QUEEN,
    RADAR_ENTITY_ITEM_HEALTH,
    RADAR_ENTITY_ITEM_AMMO,
    RADAR_ENTITY_ITEM_WEAPON,
    RADAR_ENTITY_DOOR,
    RADAR_ENTITY_LIFT,
    RADAR_ENTITY_SWITCH,
    RADAR_ENTITY_GENERATOR,
    RADAR_ENTITY_TERMINAL,
    RADAR_ENTITY_UNKNOWN
} RADAR_ENTITY_TYPE;

/* Direction for audio cues */
typedef enum {
    DIR_FRONT,
    DIR_FRONT_LEFT,
    DIR_LEFT,
    DIR_BACK_LEFT,
    DIR_BACK,
    DIR_BACK_RIGHT,
    DIR_RIGHT,
    DIR_FRONT_RIGHT,
    DIR_ABOVE,
    DIR_BELOW
} AUDIO_DIRECTION;

/* Radar entity info */
typedef struct {
    RADAR_ENTITY_TYPE type;
    AUDIO_DIRECTION direction;
    int distance;               /* In game units */
    int is_threat;              /* Is this entity hostile */
    const char* name;           /* Entity name for TTS */
} RADAR_ENTITY_INFO;

/* Update the audio radar - call each frame from game loop */
void AudioRadar_Update(void);

/* Force immediate radar scan and announcement */
void AudioRadar_ScanNow(void);

/* Announce nearest threat */
void AudioRadar_AnnounceNearestThreat(void);

/* Announce all nearby entities */
void AudioRadar_AnnounceAll(void);

/* Get direction name as string */
const char* AudioRadar_GetDirectionName(AUDIO_DIRECTION dir);

/* Get entity type name as string */
const char* AudioRadar_GetEntityTypeName(RADAR_ENTITY_TYPE type);

/* ============================================
 * Player State Announcements
 * ============================================ */

/* Update player state tracking - call each frame */
void PlayerState_Update(void);

/* Force announce current health */
void PlayerState_AnnounceHealth(void);

/* Force announce current armor */
void PlayerState_AnnounceArmor(void);

/* Force announce current weapon */
void PlayerState_AnnounceWeapon(void);

/* Force announce current ammo */
void PlayerState_AnnounceAmmo(void);

/* Announce all current stats */
void PlayerState_AnnounceAll(void);

/* ============================================
 * Navigation Audio Cues
 * ============================================ */

/* Update navigation cues - call each frame */
void Navigation_Update(void);

/* Check for and announce nearby walls */
void Navigation_CheckWalls(void);

/* Check for and announce nearby doors */
void Navigation_CheckDoors(void);

/* Announce player's current location/area */
void Navigation_AnnounceLocation(void);

/* ============================================
 * Pitch Indicator (View Angle Feedback)
 * ============================================ */

/* Update pitch indicator - plays sawtooth tone when looking significantly up/down */
void PitchIndicator_Update(void);

/* ============================================
 * Menu Accessibility
 * ============================================ */

/* Hook for menu text rendering - call when menu text is displayed */
void Menu_OnTextDisplayed(const char* text, int isSelected);

/* Announce current menu item */
void Menu_AnnounceCurrentItem(void);

/* Announce menu navigation (up/down/select) */
void Menu_OnNavigate(int direction); /* -1 = up, 1 = down, 0 = select */

/* ============================================
 * Mission Objectives
 * ============================================ */

/* Announce current mission objectives via TTS */
void Mission_AnnounceObjectives(void);

/* ============================================
 * Keyboard Shortcuts for Accessibility
 * ============================================ */

/* Process accessibility hotkeys - call from input handler */
void Accessibility_ProcessInput(void);

/* Default hotkeys:
 * F1  - Toggle accessibility on/off
 * F2  - Toggle audio radar
 * F3  - Announce current health/armor
 * F4  - Announce current weapon/ammo
 * F5  - Scan and announce nearby enemies (TTS)
 * F6  - Announce current location
 * F7  - Repeat last announcement
 * F8  - Toggle TTS on/off
 * F9  - Announce mission objectives
 * F10 - Toggle pitch indicator on/off
 * F11 - Scan for interactive elements (switches, doors, lifts)
 * F12 - Full status announcement (health, armor, weapon, ammo)
 */

/* ============================================
 * On-Screen Message Hook
 * ============================================ */

/* Called when a message is displayed on screen - announce via TTS */
void Accessibility_OnScreenMessage(const char* message);

/* ============================================
 * Interaction Detection
 * ============================================ */

/* Check for nearby interactive objects and announce "Press SPACE to interact" */
void Accessibility_CheckInteraction(void);

/* ============================================
 * Weapon State Tracking
 * ============================================ */

/* Track weapon state changes (reload, empty, jammed) and announce */
void Accessibility_WeaponStateUpdate(void);

/* Announce full player status (health, armor, weapon, ammo) */
void Accessibility_AnnounceFullStatus(void);

/* ============================================
 * Auto-Navigation System
 * ============================================ */

/* Navigation target types */
typedef enum {
    NAV_TARGET_NONE,
    NAV_TARGET_INTERACTIVE,   /* Switch, terminal, door */
    NAV_TARGET_NPC,           /* Enemy or ally */
    NAV_TARGET_EXIT,          /* Module exit/door to next area */
    NAV_TARGET_ITEM           /* Health, ammo, weapon pickup */
} NAV_TARGET_TYPE;

/* Current navigation state */
typedef struct {
    int enabled;              /* Is autonavigation active */
    int auto_rotate;          /* Automatically rotate toward target */
    int auto_move;            /* Automatically move toward target */
    NAV_TARGET_TYPE target_type;
    int target_x, target_y, target_z;  /* Target world position */
    int target_distance;      /* Distance to target */
    const char* target_name;  /* Name for TTS */
} AUTONAV_STATE;

extern AUTONAV_STATE AutoNavState;

/* Initialize autonavigation system */
void AutoNav_Init(void);

/* Update autonavigation - call each frame */
void AutoNav_Update(void);

/* Toggle autonavigation on/off */
void AutoNav_Toggle(void);

/* Toggle auto-rotation */
void AutoNav_ToggleRotation(void);

/* Toggle auto-movement */
void AutoNav_ToggleMovement(void);

/* Cycle to next target type */
void AutoNav_CycleTargetType(void);

/* Find and set nearest target of current type */
void AutoNav_FindTarget(void);

/* Announce current navigation target */
void AutoNav_AnnounceTarget(void);

/* ============================================
 * Interactive Element Scanning
 * ============================================ */

/* Scan and announce nearby interactive elements (switches, doors, lifts, etc.)
 * These provide actionable directional guidance for mission progression
 */
void Interactive_ScanAndAnnounce(void);

/* ============================================
 * Obstruction Detection System
 * ============================================ */

/* Update obstruction detection - call each frame
 * Casts rays ahead to detect walls and obstacles
 * Announces warnings when very close to obstructions
 */
void Obstruction_Update(void);

/* Announce what's directly ahead (on-demand)
 * Reports: wall, step (can walk over), obstacle (can jump), or clear
 */
void Obstruction_AnnounceAhead(void);

/* Announce distances to walls in all four directions
 * Reports front, left, right, back distances in meters
 */
void Obstruction_AnnounceSurroundings(void);

/* Toggle obstruction alert system on/off */
void Obstruction_Toggle(void);

/* ============================================
 * Utility Functions
 * ============================================ */

/* Convert game position to direction relative to player */
AUDIO_DIRECTION Accessibility_GetDirection(int px, int py, int pz,
                                           int tx, int ty, int tz,
                                           int player_yaw);

/* Calculate distance between two points */
int Accessibility_GetDistance(int x1, int y1, int z1,
                              int x2, int y2, int z2);

/* Format distance for speech (e.g., "5 meters", "very close") */
const char* Accessibility_FormatDistance(int distance);

/* ============================================
 * Debug and Logging
 * ============================================ */

/* Enable/disable accessibility debug logging */
void Accessibility_SetDebugMode(int enabled);

/* Log a message to the accessibility log */
void Accessibility_Log(const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif /* _ACCESSIBILITY_H_ */
