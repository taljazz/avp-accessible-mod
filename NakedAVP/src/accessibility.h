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

/* Announce Predator energy cell / field charge status */
void Accessibility_AnnounceEnergy(void);

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

/* Set cooldown to prevent render hooks from double-announcing
 * Call this after explicit menu announcements (like Accessibility_AnnounceMenuSelection) */
void Menu_SetAnnouncementCooldown(void);

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

/* Navigation strategies for pathfinding */
typedef enum {
    NAV_STRATEGY_DIRECT,           /* Go straight toward target */
    NAV_STRATEGY_WALL_FOLLOW_LEFT, /* Follow left wall */
    NAV_STRATEGY_WALL_FOLLOW_RIGHT,/* Follow right wall */
    NAV_STRATEGY_BACKTRACK,        /* Back up and retry */
    NAV_STRATEGY_WIDE_AROUND_LEFT, /* Wide arc left */
    NAV_STRATEGY_WIDE_AROUND_RIGHT /* Wide arc right */
} NAV_STRATEGY;

/* Position record for history tracking */
typedef struct {
    int x, y, z;
    unsigned int timestamp;
} POSITION_RECORD;

#define NAV_POSITION_HISTORY_SIZE 30  /* ~5 seconds at 6 samples/sec */

/* Door wait state for autonavigation */
typedef struct {
    void* doorSB;              /* STRATEGYBLOCK* - door we're waiting for */
    int lastState;             /* Last known door state */
    unsigned int waitStartTime;/* When we started waiting */
    int announced;             /* Have we announced waiting? */
} DOOR_WAIT_STATE;

/* Lift tracking state */
typedef struct {
    void* liftSB;              /* STRATEGYBLOCK* - lift we're tracking */
    int lastState;             /* Last known lift state */
    int playerOnLift;          /* Is player currently on lift? */
    unsigned int rideStartTime;
} LIFT_TRACK_STATE;

/* Current navigation state (extended) */
typedef struct {
    int enabled;              /* Is autonavigation active */
    int auto_rotate;          /* Automatically rotate toward target */
    int auto_move;            /* Automatically move toward target */
    NAV_TARGET_TYPE target_type;
    int target_x, target_y, target_z;  /* Target world position */
    int target_distance;      /* Distance to target */
    const char* target_name;  /* Name for TTS */

    /* Position history for loop detection */
    POSITION_RECORD position_history[NAV_POSITION_HISTORY_SIZE];
    int history_index;
    int history_count;

    /* Progress tracking */
    int last_announced_distance;  /* For "getting closer/farther" */
    int closest_achieved;         /* Closest we've ever been to this target */
    unsigned int last_progress_time;

    /* Strategy management */
    NAV_STRATEGY current_strategy;
    int strategy_frames;          /* How long current strategy has been active */
    int strategy_failures;        /* How many times strategies have failed */

    /* Door/Lift awareness */
    DOOR_WAIT_STATE door_wait;
    LIFT_TRACK_STATE lift_track;

    /* Arrival handling */
    int arrival_announced;
    int target_reached;
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

/* Pathfinding functions */
void PathFind_RecordPosition(int x, int y, int z);
int PathFind_DetectOscillation(void);
int PathFind_DetectLoop(int* loopSize);
void PathFind_EscalateStrategy(void);

/* Progress and arrival functions */
void AutoNav_CheckProgress(void);
void AutoNav_CheckArrival(void);

/* ============================================
 * Spatial Awareness System
 * ============================================ */

/* Tracked threat for proximity awareness */
typedef struct {
    void* threat;              /* STRATEGYBLOCK* */
    int last_distance;
    int current_distance;
    int approach_rate;         /* Negative = getting closer */
    unsigned int last_update;
    int warning_given;
} TRACKED_THREAT;

#define MAX_TRACKED_THREATS 8

/* Spatial awareness state */
typedef struct {
    TRACKED_THREAT threats[MAX_TRACKED_THREATS];
    int threat_count;
    int threat_check_counter;
    int is_enclosed;           /* Walls close on 3+ sides */
    int is_open;               /* Mostly open space */
    unsigned int last_threat_tts;
} SPATIAL_AWARENESS_STATE;

extern SPATIAL_AWARENESS_STATE SpatialState;

/* Update spatial awareness - call each frame */
void SpatialAwareness_Update(void);

/* Update threat tracking */
void SpatialAwareness_UpdateThreats(void);

/* Play threat proximity tone */
void SpatialAwareness_ThreatTone(void);

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
 * Environment Description System
 * ============================================ */

/* Describe the environment in all directions (8 horizontal + up/down)
 * Announces features sorted by priority (enemies first, then interactive, then walls)
 * Includes distance and spatial description (immediately, nearby, in the distance)
 * Bound to Tab key
 */
void Environment_Describe(void);

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
