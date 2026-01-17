/*
 * AVP Accessibility Module - Implementation
 *
 * Uses Tolk library for screen reader support (NVDA, JAWS, etc.)
 * Uses OpenAL for directional audio radar tones.
 * Tolk provides a unified interface to screen readers with SAPI fallback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "Tolk.h"
#endif

/* OpenAL for directional tones */
#include "al.h"
#include "alc.h"

#include "accessibility.h"

extern "C" {
#include "fixer.h"
#include "3dc.h"
#include "platform.h"
#include "gamedef.h"
#include "stratdef.h"
#include "bh_types.h"
#include "dynblock.h"
#include "dynamics.h"
#include "psndplat.h"
#include "avpview.h"
#include "equipmnt.h"
#include "los.h"

/* Mission objectives function from missions.cpp */
int GetMissionObjectivesText(char* buffer, int bufferSize);

/* External declarations for interaction detection */
extern int NumOnScreenBlocks;
extern DISPLAYBLOCK *OnScreenBlockList[];
extern TEMPLATE_WEAPON_DATA TemplateWeapon[];
extern TEMPLATE_AMMO_DATA TemplateAmmo[];

/* Vision mode for predator equipment tracking */
extern enum VISION_MODE_ID CurrentVisionMode;
/* Note: PLAYERCLOAK_MAXENERGY is defined in bh_types.h */

/* Line of sight check */
int IsThisObjectVisibleFromThisPosition_WithIgnore(DISPLAYBLOCK *ignoredObjectPtr,
    DISPLAYBLOCK *objectPtr, VECTORCH *positionPtr, int maxRange);
}

/* ============================================
 * Global State
 * ============================================ */

ACCESSIBILITY_CONFIG AccessibilitySettings = {
    1,      /* enabled */
    1,      /* tts_enabled */
    1,      /* audio_radar_enabled */
    1,      /* navigation_cues_enabled */
    1,      /* state_announcements_enabled */
    1,      /* menu_narration_enabled */
    500,    /* radar_update_interval_ms */
    5,      /* radar_max_enemies */
    50000,  /* radar_range */
    0,      /* tts_rate */
    100,    /* tts_volume */
    1,      /* tts_interrupt */
    25,     /* health_warning_threshold */
    10      /* ammo_warning_threshold */
};

static int g_AccessibilityInitialized = 0;
static int g_DebugMode = 0;

/* ============================================
 * Logging System
 * ============================================ */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3
} LOG_LEVEL;

static FILE* g_LogFile = NULL;
static int g_LoggingEnabled = 1;
static LOG_LEVEL g_LogLevel = LOG_DEBUG;  /* Default to DEBUG for troubleshooting */

static const char* LogLevelNames[] = { "DEBUG", "INFO", "WARNING", "ERROR" };

/* Initialize logging system */
static void Log_Init(void)
{
    if (g_LogFile != NULL) return;  /* Already initialized */

    char logPath[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, logPath);
    strcat(logPath, "\\accessibility_log.txt");

    g_LogFile = fopen(logPath, "w");
    if (g_LogFile) {
        /* Write header */
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_LogFile, "=== AVP Accessibility Log ===\n");
        fprintf(g_LogFile, "Started: %04d-%02d-%02d %02d:%02d:%02d\n\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_LogFile);
    }
}

/* Shutdown logging system */
static void Log_Shutdown(void)
{
    if (g_LogFile) {
        fprintf(g_LogFile, "\n=== Log Closed ===\n");
        fclose(g_LogFile);
        g_LogFile = NULL;
    }
}

/* Write a log entry */
static void Log_Write(LOG_LEVEL level, const char* format, ...)
{
    if (!g_LoggingEnabled || !g_LogFile) return;
    if (level < g_LogLevel) return;  /* Filter by log level */

    SYSTEMTIME st;
    GetLocalTime(&st);

    /* Timestamp and level */
    fprintf(g_LogFile, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            LogLevelNames[level]);

    /* Message */
    va_list args;
    va_start(args, format);
    vfprintf(g_LogFile, format, args);
    va_end(args);

    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);
}

/* Convenience macros */
#define LOG_DBG(fmt, ...) Log_Write(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INF(fmt, ...) Log_Write(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) Log_Write(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) Log_Write(LOG_ERROR, fmt, ##__VA_ARGS__)

/* Player state tracking for change detection */
static int g_LastHealth = -1;
static int g_LastArmor = -1;
static int g_LastWeaponSlot = -1;
static int g_LastAmmo = -1;
static int g_HealthWarningGiven = 0;

/* Timing for radar updates */
static unsigned int g_LastRadarUpdate = 0;

/* Last spoken text for repeat function */
static char g_LastSpokenText[512] = {0};

/* Redundancy prevention - track last spoken text per category */
static char g_LastMenuText[256] = {0};
static char g_LastStateText[256] = {0};
static char g_LastLocationText[256] = {0};
static int g_LastPitchZone = 0;  /* -1 = looking down, 0 = level, 1 = looking up */

/* Pitch indicator state */
static int g_PitchIndicatorEnabled = 1;
static int g_LastPitchToneTime = 0;

/* Arrow key rotation/look control */
#define ARROW_ROTATION_SPEED 40      /* Rotation speed per frame (EulerY angular velocity) */
#define ARROW_PITCH_SPEED 512        /* Pitch adjustment per frame (ViewPanX units) */
#define PITCH_CENTER_THRESHOLD 64    /* Pitch considered "centered" if within this range */
static int g_WasPitchOffCenter = 0;  /* Track if pitch was off-center for centering tone */
static int g_LastCenteringToneTime = 0;  /* Debounce centering tone */

/* Interaction detection state */
static int g_LastInteractiveNearby = 0;  /* Was an interactive nearby last frame? */
static char g_LastInteractiveType[64] = {0};  /* Type of last interactive */
static int g_InteractionCheckFrames = 0;  /* Frame counter for interaction checks */

/* Weapon state tracking */
static int g_LastWeaponState = -1;  /* Last weapon state for change detection */
static int g_LastPrimaryRounds = -1;  /* Track ammo for reload detection */
static int g_LastSecondaryRounds = -1;

/* On-screen message tracking */
static char g_LastOnScreenMessage[256] = {0};  /* Prevent duplicate announcements */

/* Predator equipment tracking */
static int g_LastCloakOn = -1;           /* Cloak state (-1 = unknown) */
static int g_LastVisionMode = -1;        /* Vision mode enum value */
static int g_LastFieldChargePercent = -1; /* Energy percentage (0-100) */

/* Obstruction announcement tracking */
static char g_LastObstructionText[256] = {0};  /* Prevent repeating same obstruction */
static int g_LastObstructionTime = 0;          /* Time of last obstruction announcement */

/* Forward declaration of obstruction state for use in AutoNav */
typedef struct {
    int enabled;
    int last_check_frame;
    int forward_blocked;
    int forward_distance;
    int forward_is_jumpable;      /* Height < MAXIMUM_STEP_HEIGHT - can walk over */
    int forward_is_clearable;     /* Height < JUMP_CLEARANCE_HEIGHT - can jump */
    int left_distance;
    int right_distance;
    int ceiling_distance;
    int floor_distance;
    int last_announced_forward;
    int last_announced_left;
    int last_announced_right;
} OBSTRUCTION_STATE;

static OBSTRUCTION_STATE g_ObstructionState = {
    1,      /* enabled */
    0,      /* last_check_frame */
    0, 0, 0, 0,  /* forward state */
    0, 0,   /* left/right */
    0, 0,   /* ceiling/floor */
    0, 0, 0 /* last announced */
};

/* ============================================
 * Announcement Priority/Cooldown System
 * Prevents auditory overload during intense gameplay
 * ============================================ */

/* Priority levels for announcements */
typedef enum {
    ANNOUNCE_PRIORITY_CRITICAL = 0,  /* Damage, health critical - always plays, triggers cooldown */
    ANNOUNCE_PRIORITY_HIGH = 1,      /* Obstruction warnings, interaction prompts */
    ANNOUNCE_PRIORITY_NORMAL = 2,    /* Weapon changes, pickups */
    ANNOUNCE_PRIORITY_LOW = 3        /* Radar, navigation updates */
} ANNOUNCE_PRIORITY;

/* Cooldown tracking */
static unsigned int g_LastCriticalAnnouncementTime = 0;  /* When last critical was played */
static unsigned int g_LastHighAnnouncementTime = 0;      /* When last high priority was played */
static unsigned int g_LastNormalAnnouncementTime = 0;    /* When last normal was played */

/* Cooldown durations in milliseconds */
#define COOLDOWN_AFTER_CRITICAL_MS 600   /* Suppress non-critical for 600ms after damage */
#define COOLDOWN_AFTER_HIGH_MS 400       /* Suppress low priority for 400ms after high */
#define COOLDOWN_BETWEEN_SAME_MS 200     /* Min time between same-priority announcements */

/* Check if an announcement at given priority is allowed based on cooldowns */
static int Announcement_IsAllowed(ANNOUNCE_PRIORITY priority)
{
    unsigned int currentTime = GetTickCount();

    /* Critical announcements always allowed */
    if (priority == ANNOUNCE_PRIORITY_CRITICAL) {
        return 1;
    }

    /* High priority - blocked during critical cooldown */
    if (priority == ANNOUNCE_PRIORITY_HIGH) {
        if ((currentTime - g_LastCriticalAnnouncementTime) < COOLDOWN_AFTER_CRITICAL_MS) {
            return 0;  /* Still in critical cooldown */
        }
        return 1;
    }

    /* Normal priority - blocked during critical and high cooldowns */
    if (priority == ANNOUNCE_PRIORITY_NORMAL) {
        if ((currentTime - g_LastCriticalAnnouncementTime) < COOLDOWN_AFTER_CRITICAL_MS) {
            return 0;
        }
        if ((currentTime - g_LastHighAnnouncementTime) < COOLDOWN_AFTER_HIGH_MS) {
            return 0;
        }
        return 1;
    }

    /* Low priority - blocked during all higher priority cooldowns */
    if (priority == ANNOUNCE_PRIORITY_LOW) {
        if ((currentTime - g_LastCriticalAnnouncementTime) < COOLDOWN_AFTER_CRITICAL_MS) {
            return 0;
        }
        if ((currentTime - g_LastHighAnnouncementTime) < COOLDOWN_AFTER_HIGH_MS) {
            return 0;
        }
        if ((currentTime - g_LastNormalAnnouncementTime) < COOLDOWN_BETWEEN_SAME_MS) {
            return 0;
        }
        return 1;
    }

    return 1;
}

/* Record that an announcement was made at a given priority */
static void Announcement_RecordTime(ANNOUNCE_PRIORITY priority)
{
    unsigned int currentTime = GetTickCount();

    switch (priority) {
        case ANNOUNCE_PRIORITY_CRITICAL:
            g_LastCriticalAnnouncementTime = currentTime;
            break;
        case ANNOUNCE_PRIORITY_HIGH:
            g_LastHighAnnouncementTime = currentTime;
            break;
        case ANNOUNCE_PRIORITY_NORMAL:
            g_LastNormalAnnouncementTime = currentTime;
            break;
        case ANNOUNCE_PRIORITY_LOW:
            /* Low priority doesn't block anything */
            break;
    }
}

#ifdef _WIN32
/* Tolk screen reader library state */
static int g_TolkInitialized = 0;
#endif

/* ============================================
 * Audio Radar Tone System (OpenAL)
 * ============================================ */

#define RADAR_TONE_SAMPLE_RATE 44100
#define RADAR_TONE_DURATION_MS 150
#define RADAR_TONE_SAMPLES (RADAR_TONE_SAMPLE_RATE * RADAR_TONE_DURATION_MS / 1000)
#define RADAR_BASE_FREQUENCY 440.0f  /* A4 note - pleasant base frequency */

static ALuint g_RadarToneBuffer = 0;
static ALuint g_RadarToneSource = 0;
static int g_RadarToneInitialized = 0;

/* Generate a soft sine wave tone buffer */
static int RadarTone_GenerateBuffer(void)
{
    if (g_RadarToneBuffer != 0) {
        return 1; /* Already generated */
    }

    /* Allocate buffer for 16-bit mono samples */
    short* samples = (short*)malloc(RADAR_TONE_SAMPLES * sizeof(short));
    if (!samples) return 0;

    /* Generate sine wave with soft attack/decay envelope */
    for (int i = 0; i < RADAR_TONE_SAMPLES; i++) {
        float t = (float)i / RADAR_TONE_SAMPLE_RATE;
        float phase = 2.0f * 3.14159265f * RADAR_BASE_FREQUENCY * t;
        float sample = sinf(phase);

        /* Soft envelope: fade in/out over 20% of duration */
        float envelope = 1.0f;
        float fadeLen = RADAR_TONE_SAMPLES * 0.2f;
        if (i < (int)fadeLen) {
            envelope = (float)i / fadeLen;
        } else if (i > RADAR_TONE_SAMPLES - (int)fadeLen) {
            envelope = (float)(RADAR_TONE_SAMPLES - i) / fadeLen;
        }

        /* Apply envelope and convert to 16-bit */
        samples[i] = (short)(sample * envelope * 24000.0f);
    }

    /* Create OpenAL buffer */
    alGenBuffers(1, &g_RadarToneBuffer);
    if (alGetError() != AL_NO_ERROR) {
        free(samples);
        return 0;
    }

    alBufferData(g_RadarToneBuffer, AL_FORMAT_MONO16, samples,
                 RADAR_TONE_SAMPLES * sizeof(short), RADAR_TONE_SAMPLE_RATE);

    free(samples);

    if (alGetError() != AL_NO_ERROR) {
        alDeleteBuffers(1, &g_RadarToneBuffer);
        g_RadarToneBuffer = 0;
        return 0;
    }

    return 1;
}

/* Initialize radar tone source */
static int RadarTone_Init(void)
{
    if (g_RadarToneInitialized) {
        return 1;
    }

    /* Generate the tone buffer first */
    if (!RadarTone_GenerateBuffer()) {
        Accessibility_Log("Failed to generate radar tone buffer\n");
        return 0;
    }

    /* Create source */
    alGenSources(1, &g_RadarToneSource);
    if (alGetError() != AL_NO_ERROR) {
        Accessibility_Log("Failed to create radar tone source\n");
        return 0;
    }

    /* Attach buffer to source */
    alSourcei(g_RadarToneSource, AL_BUFFER, g_RadarToneBuffer);

    /* Set source properties for 3D positioning */
    alSourcef(g_RadarToneSource, AL_GAIN, 0.7f);
    alSourcef(g_RadarToneSource, AL_REFERENCE_DISTANCE, 5000.0f);
    alSourcef(g_RadarToneSource, AL_MAX_DISTANCE, 50000.0f);
    alSourcei(g_RadarToneSource, AL_SOURCE_RELATIVE, AL_FALSE);

    g_RadarToneInitialized = 1;
    Accessibility_Log("Radar tone system initialized\n");
    return 1;
}

/* Shutdown radar tone system */
static void RadarTone_Shutdown(void)
{
    if (g_RadarToneSource != 0) {
        alSourceStop(g_RadarToneSource);
        alDeleteSources(1, &g_RadarToneSource);
        g_RadarToneSource = 0;
    }

    if (g_RadarToneBuffer != 0) {
        alDeleteBuffers(1, &g_RadarToneBuffer);
        g_RadarToneBuffer = 0;
    }

    g_RadarToneInitialized = 0;
}

/* Get pitch multiplier for enemy type - different enemies have distinct tones
 * Aliens: Lower, menacing (0.75x)
 * Predators: Mid-range, distinctive (1.2x)
 * Marines: Higher, sharp (1.5x)
 * Facehuggers: Very high, frantic (1.8x)
 * Queen: Very low, ominous (0.6x)
 */
static float GetEnemyPitchMultiplier(AVP_BEHAVIOUR_TYPE bhvr)
{
    switch (bhvr) {
        case I_BehaviourAlien:
            return 0.75f;  /* Lower, menacing */
        case I_BehaviourQueenAlien:
            return 0.6f;   /* Very low, ominous */
        case I_BehaviourFaceHugger:
            return 1.8f;   /* High, frantic */
        case I_BehaviourPredator:
            return 1.2f;   /* Distinctive mid-high */
        case I_BehaviourXenoborg:
            return 0.9f;   /* Slightly lower, mechanical feel */
        case I_BehaviourMarine:
        case I_BehaviourSeal:
            return 1.5f;   /* Higher, sharp */
        default:
            return 1.0f;   /* Default */
    }
}

/* Play a directional radar tone for an entity
 * - Position determines stereo panning (left/right/front/back)
 * - Vertical offset determines pitch (above = higher, below = lower)
 * - Distance affects volume
 * - Entity type affects base pitch (different enemies have distinct tones)
 */
static void RadarTone_PlayDirectional(int targetX, int targetY, int targetZ,
                                       int playerX, int playerY, int playerZ,
                                       int playerYaw, AVP_BEHAVIOUR_TYPE entityType)
{
    if (!g_RadarToneInitialized) {
        if (!RadarTone_Init()) return;
    }

    /* Check if source is already playing - if so, let it finish (allows parallel sounds) */
    ALint state;
    alGetSourcei(g_RadarToneSource, AL_SOURCE_STATE, &state);
    if (state == AL_PLAYING) {
        return;  /* Let current sound finish, don't interrupt */
    }

    /* Use SOURCE_RELATIVE for UI sounds to not interfere with game 3D audio */
    alSourcei(g_RadarToneSource, AL_SOURCE_RELATIVE, AL_TRUE);

    /* Calculate relative position */
    float dx = (float)(targetX - playerX);
    float dy = (float)(targetY - playerY);
    float dz = (float)(targetZ - playerZ);

    /* Convert player yaw to radians (game uses 0-4096 range) */
    float yawRad = (float)playerYaw * (3.14159265f * 2.0f / 4096.0f);

    /* Rotate relative position by inverse of player yaw to get position relative to player's view */
    float cosYaw = cosf(-yawRad);
    float sinYaw = sinf(-yawRad);
    float relX = dx * cosYaw - dz * sinYaw;
    float relZ = dx * sinYaw + dz * cosYaw;

    /* Scale for OpenAL (game units to a reasonable 3D space) */
    float scale = 0.001f;  /* Convert game units to OpenAL units */
    float posX = relX * scale;
    float posY = dy * scale;
    float posZ = -relZ * scale;  /* OpenAL uses -Z as forward */

    /* Set source position in 3D space */
    alSource3f(g_RadarToneSource, AL_POSITION, posX, posY, posZ);

    /* Calculate pitch based on:
     * 1. Enemy type (different enemies have distinct base tones)
     * 2. Vertical offset (above = higher pitch, below = lower)
     */
    float distance = sqrtf(dx*dx + dy*dy + dz*dz);
    float verticalRatio = 0.0f;
    if (distance > 0.0f) {
        verticalRatio = dy / distance;  /* -1 to 1 range */
    }

    /* Get enemy-specific pitch multiplier */
    float enemyPitch = GetEnemyPitchMultiplier(entityType);

    /* Apply vertical variation: -1 (below) -> -0.3, 0 (level) -> 0, 1 (above) -> +0.3 */
    float pitch = enemyPitch + (verticalRatio * 0.3f);

    /* Clamp to reasonable range */
    if (pitch < 0.4f) pitch = 0.4f;
    if (pitch > 2.5f) pitch = 2.5f;

    alSourcef(g_RadarToneSource, AL_PITCH, pitch);

    /* Distance-based volume (closer = louder) */
    float maxRange = (float)AccessibilitySettings.radar_range;
    float volumeScale = 1.0f - (distance / maxRange);
    if (volumeScale < 0.2f) volumeScale = 0.2f;
    if (volumeScale > 1.0f) volumeScale = 1.0f;
    /* Lower gain to not overpower game sounds */
    alSourcef(g_RadarToneSource, AL_GAIN, 0.35f * volumeScale);

    /* Play the tone */
    alSourceRewind(g_RadarToneSource);
    alSourcePlay(g_RadarToneSource);
}

/* ============================================
 * Pitch Indicator Tone System (Sawtooth Wave)
 * ============================================ */

#define PITCH_TONE_SAMPLE_RATE 44100
#define PITCH_TONE_DURATION_MS 100
#define PITCH_TONE_SAMPLES (PITCH_TONE_SAMPLE_RATE * PITCH_TONE_DURATION_MS / 1000)
#define PITCH_BASE_FREQUENCY 220.0f  /* A3 note */

static ALuint g_PitchToneBuffer = 0;
static ALuint g_PitchToneSource = 0;
static int g_PitchToneInitialized = 0;

/* Generate a soft sawtooth wave tone buffer */
static int PitchTone_GenerateBuffer(void)
{
    if (g_PitchToneBuffer != 0) {
        return 1; /* Already generated */
    }

    short* samples = (short*)malloc(PITCH_TONE_SAMPLES * sizeof(short));
    if (!samples) return 0;

    /* Generate sawtooth wave with soft envelope */
    for (int i = 0; i < PITCH_TONE_SAMPLES; i++) {
        float t = (float)i / PITCH_TONE_SAMPLE_RATE;
        float phase = fmodf(PITCH_BASE_FREQUENCY * t, 1.0f);
        /* Sawtooth: goes from -1 to 1 linearly */
        float sample = 2.0f * phase - 1.0f;

        /* Soft envelope for pleasant sound */
        float envelope = 1.0f;
        float fadeLen = PITCH_TONE_SAMPLES * 0.15f;
        if (i < (int)fadeLen) {
            envelope = (float)i / fadeLen;
        } else if (i > PITCH_TONE_SAMPLES - (int)fadeLen) {
            envelope = (float)(PITCH_TONE_SAMPLES - i) / fadeLen;
        }

        /* Soften the sawtooth slightly with envelope and lower amplitude */
        samples[i] = (short)(sample * envelope * 16000.0f);
    }

    alGenBuffers(1, &g_PitchToneBuffer);
    if (alGetError() != AL_NO_ERROR) {
        free(samples);
        return 0;
    }

    alBufferData(g_PitchToneBuffer, AL_FORMAT_MONO16, samples,
                 PITCH_TONE_SAMPLES * sizeof(short), PITCH_TONE_SAMPLE_RATE);

    free(samples);

    if (alGetError() != AL_NO_ERROR) {
        alDeleteBuffers(1, &g_PitchToneBuffer);
        g_PitchToneBuffer = 0;
        return 0;
    }

    return 1;
}

static int PitchTone_Init(void)
{
    if (g_PitchToneInitialized) {
        return 1;
    }

    if (!PitchTone_GenerateBuffer()) {
        Accessibility_Log("Failed to generate pitch tone buffer\n");
        return 0;
    }

    alGenSources(1, &g_PitchToneSource);
    if (alGetError() != AL_NO_ERROR) {
        Accessibility_Log("Failed to create pitch tone source\n");
        return 0;
    }

    alSourcei(g_PitchToneSource, AL_BUFFER, g_PitchToneBuffer);
    alSourcef(g_PitchToneSource, AL_GAIN, 0.4f);
    alSourcei(g_PitchToneSource, AL_SOURCE_RELATIVE, AL_TRUE);  /* Plays at listener position */
    alSource3f(g_PitchToneSource, AL_POSITION, 0.0f, 0.0f, 0.0f);

    g_PitchToneInitialized = 1;
    Accessibility_Log("Pitch indicator tone initialized\n");
    return 1;
}

static void PitchTone_Shutdown(void)
{
    if (g_PitchToneSource != 0) {
        alSourceStop(g_PitchToneSource);
        alDeleteSources(1, &g_PitchToneSource);
        g_PitchToneSource = 0;
    }

    if (g_PitchToneBuffer != 0) {
        alDeleteBuffers(1, &g_PitchToneBuffer);
        g_PitchToneBuffer = 0;
    }

    g_PitchToneInitialized = 0;
}

/* Play pitch indicator tone
 * pitchAngle: player's view pitch in game units (typically -2048 to 2048 range)
 * Negative = looking down, Positive = looking up
 */
static void PitchTone_Play(int pitchAngle)
{
    if (!g_PitchIndicatorEnabled) return;

    if (!g_PitchToneInitialized) {
        if (!PitchTone_Init()) return;
    }

    /* Check if source is already playing - if so, let it finish (allows parallel sounds) */
    ALint state;
    alGetSourcei(g_PitchToneSource, AL_SOURCE_STATE, &state);
    if (state == AL_PLAYING) {
        return;  /* Let current sound finish, don't interrupt */
    }

    /* Use SOURCE_RELATIVE for UI sounds */
    alSourcei(g_PitchToneSource, AL_SOURCE_RELATIVE, AL_TRUE);

    /* Convert pitch angle to frequency multiplier
     * Looking up = higher pitch, looking down = lower pitch
     * Game angles are typically in 0-4096 range (or -2048 to 2048 for pitch)
     * Map to pitch multiplier: 0.5 (down) to 2.0 (up)
     */
    float normalizedPitch = (float)pitchAngle / 2048.0f;  /* -1 to 1 range */
    if (normalizedPitch < -1.0f) normalizedPitch = -1.0f;
    if (normalizedPitch > 1.0f) normalizedPitch = 1.0f;

    /* Map to pitch: looking down = 0.6, level = 1.0, looking up = 1.6 */
    float pitch = 1.0f + (normalizedPitch * 0.5f);

    alSourcef(g_PitchToneSource, AL_PITCH, pitch);
    alSourcef(g_PitchToneSource, AL_GAIN, 0.3f);  /* Lower gain */

    /* Play the tone */
    alSourceRewind(g_PitchToneSource);
    alSourcePlay(g_PitchToneSource);
}

/* Play a short "centered" tone - higher pitch, quick sound to indicate level view */
static void CenteringTone_Play(void)
{
    if (!g_PitchToneInitialized || !g_PitchToneSource) {
        return;
    }

    /* Play at higher pitch (1.5x) to distinguish from regular pitch indicator */
    alSourcef(g_PitchToneSource, AL_PITCH, 1.5f);
    alSourcef(g_PitchToneSource, AL_GAIN, 0.4f);

    /* Position at listener (center) */
    alSource3f(g_PitchToneSource, AL_POSITION, 0.0f, 0.0f, 0.0f);

    /* Play the tone */
    alSourceRewind(g_PitchToneSource);
    alSourcePlay(g_PitchToneSource);
}

/* ============================================
 * TTS Implementation (Tolk Screen Reader Library)
 * ============================================ */

#ifdef _WIN32

static int TTS_InitTolk(void)
{
    if (g_TolkInitialized) {
        return 1; /* Already initialized */
    }

    /* Enable SAPI as fallback when no screen reader is running */
    Tolk_TrySAPI(true);

    /* Initialize Tolk (also initializes COM) */
    Tolk_Load();

    if (!Tolk_IsLoaded()) {
        Accessibility_Log("Failed to initialize Tolk\n");
        return 0;
    }

    g_TolkInitialized = 1;

    /* Log which screen reader was detected */
    const wchar_t* screenReader = Tolk_DetectScreenReader();
    if (screenReader) {
        /* Convert wide string to narrow for logging */
        char screenReaderName[64];
        WideCharToMultiByte(CP_UTF8, 0, screenReader, -1, screenReaderName, sizeof(screenReaderName), NULL, NULL);
        Accessibility_Log("Tolk initialized with screen reader: %s\n", screenReaderName);
        LOG_INF("Screen reader detected: %s", screenReaderName);
    } else {
        Accessibility_Log("Tolk initialized (no screen reader detected, using SAPI fallback)\n");
        LOG_INF("No screen reader detected, using SAPI fallback");
    }

    /* Log Tolk capabilities */
    LOG_INF("Tolk speech support: %s", Tolk_HasSpeech() ? "yes" : "no");
    LOG_INF("Tolk braille support: %s", Tolk_HasBraille() ? "yes" : "no");

    return 1;
}

static void TTS_ShutdownTolk(void)
{
    if (g_TolkInitialized) {
        Tolk_Unload();
        g_TolkInitialized = 0;
    }
}

static void TTS_SpeakInternal(const char* text, int interrupt)
{
    if (!g_TolkInitialized || !text || !AccessibilitySettings.tts_enabled) {
        return;
    }

    /* Convert to wide string for Tolk */
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (len <= 0) return;

    WCHAR* wtext = (WCHAR*)malloc(len * sizeof(WCHAR));
    if (!wtext) return;

    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, len);

    /* Output through screen reader (speech + braille) */
    Tolk_Output(wtext, interrupt ? true : false);

    free(wtext);

    /* Store for repeat function */
    strncpy(g_LastSpokenText, text, sizeof(g_LastSpokenText) - 1);
    g_LastSpokenText[sizeof(g_LastSpokenText) - 1] = '\0';
}

#else
/* Non-Windows stub implementations */
static int TTS_InitTolk(void) { return 0; }
static void TTS_ShutdownTolk(void) {}
static void TTS_SpeakInternal(const char* text, int interrupt) {
    (void)text; (void)interrupt;
}
#endif

/* ============================================
 * Public TTS Functions
 * ============================================ */

extern "C" void TTS_Speak(const char* text)
{
    if (!AccessibilitySettings.enabled) return;

#ifdef _WIN32
    /* Interrupt previous speech if configured */
    int interrupt = AccessibilitySettings.tts_interrupt ? 1 : 0;
    TTS_SpeakInternal(text, interrupt);
#endif

    LOG_INF("TTS: %s", text);
}

extern "C" void TTS_SpeakQueued(const char* text)
{
    if (!AccessibilitySettings.enabled) return;

#ifdef _WIN32
    /* Don't interrupt - queue after current speech */
    TTS_SpeakInternal(text, 0);
#endif
}

extern "C" void TTS_SpeakPriority(const char* text)
{
    if (!AccessibilitySettings.enabled) return;

#ifdef _WIN32
    /* Always interrupt previous speech */
    TTS_SpeakInternal(text, 1);
#endif
}

extern "C" void TTS_Stop(void)
{
#ifdef _WIN32
    if (g_TolkInitialized) {
        Tolk_Silence();
    }
#endif
}

extern "C" int TTS_IsSpeaking(void)
{
#ifdef _WIN32
    if (!g_TolkInitialized) return 0;
    return Tolk_IsSpeaking() ? 1 : 0;
#else
    return 0;
#endif
}

extern "C" void TTS_SetRate(int rate)
{
    /* Rate adjustment not supported by Tolk - store setting for compatibility */
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    AccessibilitySettings.tts_rate = rate;
    /* Note: Tolk uses the screen reader's default speech settings */
}

extern "C" void TTS_SetVolume(int volume)
{
    /* Volume adjustment not supported by Tolk - store setting for compatibility */
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    AccessibilitySettings.tts_volume = volume;
    /* Note: Tolk uses the screen reader's default speech settings */
}

/* ============================================
 * Configuration File Support
 * ============================================ */

#ifdef _WIN32
static void LoadConfigFile(void)
{
    char iniPath[MAX_PATH];
    char buffer[64];

    /* Try to find accessibility.ini in current directory */
    GetCurrentDirectoryA(MAX_PATH, iniPath);
    strcat(iniPath, "\\accessibility.ini");

    /* Check if file exists */
    DWORD attrs = GetFileAttributesA(iniPath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        Accessibility_Log("No accessibility.ini found, using defaults\n");
        return;
    }

    Accessibility_Log("Loading config from %s\n", iniPath);

    /* General settings */
    AccessibilitySettings.enabled = GetPrivateProfileIntA("General", "Enabled", 1, iniPath);
    AccessibilitySettings.tts_enabled = GetPrivateProfileIntA("General", "TTSEnabled", 1, iniPath);
    AccessibilitySettings.audio_radar_enabled = GetPrivateProfileIntA("General", "RadarEnabled", 1, iniPath);
    AccessibilitySettings.navigation_cues_enabled = GetPrivateProfileIntA("General", "NavigationCues", 1, iniPath);
    AccessibilitySettings.state_announcements_enabled = GetPrivateProfileIntA("General", "StateAnnouncements", 1, iniPath);
    AccessibilitySettings.menu_narration_enabled = GetPrivateProfileIntA("General", "MenuNarration", 1, iniPath);

    /* TTS settings */
    AccessibilitySettings.tts_rate = GetPrivateProfileIntA("TTS", "Rate", 0, iniPath);
    if (AccessibilitySettings.tts_rate < -10) AccessibilitySettings.tts_rate = -10;
    if (AccessibilitySettings.tts_rate > 10) AccessibilitySettings.tts_rate = 10;

    AccessibilitySettings.tts_volume = GetPrivateProfileIntA("TTS", "Volume", 100, iniPath);
    if (AccessibilitySettings.tts_volume < 0) AccessibilitySettings.tts_volume = 0;
    if (AccessibilitySettings.tts_volume > 100) AccessibilitySettings.tts_volume = 100;

    AccessibilitySettings.tts_interrupt = GetPrivateProfileIntA("TTS", "Interrupt", 1, iniPath);

    /* Radar settings */
    AccessibilitySettings.radar_update_interval_ms = GetPrivateProfileIntA("Radar", "UpdateInterval", 500, iniPath);
    AccessibilitySettings.radar_max_enemies = GetPrivateProfileIntA("Radar", "MaxEnemies", 5, iniPath);
    AccessibilitySettings.radar_range = GetPrivateProfileIntA("Radar", "Range", 50000, iniPath);

    /* Warning thresholds */
    AccessibilitySettings.health_warning_threshold = GetPrivateProfileIntA("Warnings", "HealthThreshold", 25, iniPath);
    AccessibilitySettings.ammo_warning_threshold = GetPrivateProfileIntA("Warnings", "AmmoThreshold", 10, iniPath);

    /* Logging settings */
    g_LoggingEnabled = GetPrivateProfileIntA("Logging", "Enabled", 1, iniPath);

    char levelStr[32];
    GetPrivateProfileStringA("Logging", "Level", "DEBUG", levelStr, sizeof(levelStr), iniPath);
    if (_stricmp(levelStr, "DEBUG") == 0) g_LogLevel = LOG_DEBUG;
    else if (_stricmp(levelStr, "INFO") == 0) g_LogLevel = LOG_INFO;
    else if (_stricmp(levelStr, "WARNING") == 0) g_LogLevel = LOG_WARNING;
    else if (_stricmp(levelStr, "ERROR") == 0) g_LogLevel = LOG_ERROR;

    LOG_INF("Config loaded successfully");
}
#else
static void LoadConfigFile(void) { /* No-op on non-Windows */ }
#endif

/* ============================================
 * Initialization and Shutdown
 * ============================================ */

extern "C" int Accessibility_Init(void)
{
    if (g_AccessibilityInitialized) {
        return 1;
    }

    /* Initialize logging first */
    Log_Init();
    LOG_INF("=== Accessibility System Starting ===");

    /* Load configuration from INI file */
    LoadConfigFile();

    LOG_INF("Initializing accessibility system...");

    /* Initialize TTS (Tolk screen reader library) */
    if (!TTS_InitTolk()) {
        Accessibility_Log("Warning: TTS initialization failed\n");
        AccessibilitySettings.tts_enabled = 0;
    }

    g_AccessibilityInitialized = 1;

    /* Announce startup */
    TTS_Speak("Accessibility features enabled");

    return 1;
}

extern "C" void Accessibility_Shutdown(void)
{
    if (!g_AccessibilityInitialized) {
        return;
    }

    LOG_INF("=== Accessibility System Shutting Down ===");

    TTS_Stop();
    TTS_ShutdownTolk();
    RadarTone_Shutdown();
    PitchTone_Shutdown();

    g_AccessibilityInitialized = 0;

    LOG_INF("Accessibility system shutdown complete");
    Log_Shutdown();
}

extern "C" int Accessibility_IsAvailable(void)
{
    return g_AccessibilityInitialized && AccessibilitySettings.enabled;
}

/* ============================================
 * Direction and Distance Utilities
 * ============================================ */

extern "C" AUDIO_DIRECTION Accessibility_GetDirection(int px, int py, int pz,
                                                       int tx, int ty, int tz,
                                                       int player_yaw)
{
    /* Calculate relative position */
    int dx = tx - px;
    int dy = ty - py;
    int dz = tz - pz;

    /* Check vertical first */
    int horiz_dist = (int)sqrt((double)(dx*dx + dz*dz));
    if (abs(dy) > horiz_dist) {
        return (dy < 0) ? DIR_ABOVE : DIR_BELOW;
    }

    /* Calculate angle to target */
    double angle = atan2((double)dx, (double)dz);
    angle = angle * 180.0 / 3.14159265358979323846;

    /* Adjust for player's facing direction */
    /* player_yaw is typically in 0-4096 range (12-bit angle) */
    double player_angle = (player_yaw / 4096.0) * 360.0;
    angle = angle - player_angle;

    /* Normalize to -180 to 180 */
    while (angle > 180.0) angle -= 360.0;
    while (angle < -180.0) angle += 360.0;

    /* Convert to direction enum */
    if (angle >= -22.5 && angle < 22.5) return DIR_FRONT;
    if (angle >= 22.5 && angle < 67.5) return DIR_FRONT_RIGHT;
    if (angle >= 67.5 && angle < 112.5) return DIR_RIGHT;
    if (angle >= 112.5 && angle < 157.5) return DIR_BACK_RIGHT;
    if (angle >= 157.5 || angle < -157.5) return DIR_BACK;
    if (angle >= -157.5 && angle < -112.5) return DIR_BACK_LEFT;
    if (angle >= -112.5 && angle < -67.5) return DIR_LEFT;
    if (angle >= -67.5 && angle < -22.5) return DIR_FRONT_LEFT;

    return DIR_FRONT;
}

extern "C" int Accessibility_GetDistance(int x1, int y1, int z1,
                                         int x2, int y2, int z2)
{
    int dx = x2 - x1;
    int dy = y2 - y1;
    int dz = z2 - z1;
    return (int)sqrt((double)(dx*dx + dy*dy + dz*dz));
}

extern "C" const char* Accessibility_FormatDistance(int distance)
{
    static char buffer[64];

    /* Convert game units to approximate meters (rough estimate) */
    int meters = distance / 1000;

    if (meters < 2) {
        return "very close";
    } else if (meters < 5) {
        return "close";
    } else if (meters < 10) {
        snprintf(buffer, sizeof(buffer), "%d meters", meters);
    } else if (meters < 20) {
        return "medium range";
    } else if (meters < 50) {
        return "far";
    } else {
        return "very far";
    }

    return buffer;
}

extern "C" const char* AudioRadar_GetDirectionName(AUDIO_DIRECTION dir)
{
    switch (dir) {
        case DIR_FRONT: return "ahead";
        case DIR_FRONT_LEFT: return "front left";
        case DIR_LEFT: return "left";
        case DIR_BACK_LEFT: return "behind left";
        case DIR_BACK: return "behind";
        case DIR_BACK_RIGHT: return "behind right";
        case DIR_RIGHT: return "right";
        case DIR_FRONT_RIGHT: return "front right";
        case DIR_ABOVE: return "above";
        case DIR_BELOW: return "below";
        default: return "unknown";
    }
}

extern "C" const char* AudioRadar_GetEntityTypeName(RADAR_ENTITY_TYPE type)
{
    switch (type) {
        case RADAR_ENTITY_ALIEN: return "alien";
        case RADAR_ENTITY_PREDATOR: return "predator";
        case RADAR_ENTITY_MARINE: return "marine";
        case RADAR_ENTITY_FACEHUGGER: return "facehugger";
        case RADAR_ENTITY_XENOBORG: return "xenoborg";
        case RADAR_ENTITY_QUEEN: return "queen";
        case RADAR_ENTITY_ITEM_HEALTH: return "health pickup";
        case RADAR_ENTITY_ITEM_AMMO: return "ammo";
        case RADAR_ENTITY_ITEM_WEAPON: return "weapon";
        case RADAR_ENTITY_DOOR: return "door";
        case RADAR_ENTITY_LIFT: return "lift";
        case RADAR_ENTITY_SWITCH: return "switch";
        case RADAR_ENTITY_GENERATOR: return "generator";
        case RADAR_ENTITY_TERMINAL: return "terminal";
        default: return "unknown";
    }
}

/* ============================================
 * Audio Radar Implementation
 * ============================================ */

static RADAR_ENTITY_TYPE GetRadarEntityType(AVP_BEHAVIOUR_TYPE bhvr)
{
    switch (bhvr) {
        case I_BehaviourAlien:
            return RADAR_ENTITY_ALIEN;
        case I_BehaviourQueenAlien:
            return RADAR_ENTITY_QUEEN;
        case I_BehaviourFaceHugger:
            return RADAR_ENTITY_FACEHUGGER;
        case I_BehaviourPredator:
            return RADAR_ENTITY_PREDATOR;
        case I_BehaviourXenoborg:
            return RADAR_ENTITY_XENOBORG;
        case I_BehaviourMarine:
        case I_BehaviourSeal:
            return RADAR_ENTITY_MARINE;
        case I_BehaviourProximityDoor:
        case I_BehaviourLiftDoor:
        case I_BehaviourSwitchDoor:
            return RADAR_ENTITY_DOOR;
        case I_BehaviourLift:
            return RADAR_ENTITY_LIFT;
        case I_BehaviourBinarySwitch:
        case I_BehaviourLinkSwitch:
            return RADAR_ENTITY_SWITCH;
        case I_BehaviourGenerator:
            return RADAR_ENTITY_GENERATOR;
        case I_BehaviourDatabase:
            return RADAR_ENTITY_TERMINAL;
        default:
            return RADAR_ENTITY_UNKNOWN;
    }
}

static int IsEntityThreat(AVP_BEHAVIOUR_TYPE bhvr, I_PLAYER_TYPE playerType)
{
    switch (bhvr) {
        case I_BehaviourAlien:
        case I_BehaviourQueenAlien:
        case I_BehaviourFaceHugger:
            /* Aliens are threats to Marines and Predators */
            return (playerType == I_Marine || playerType == I_Predator);

        case I_BehaviourPredator:
            /* Predators are threats to Marines and Aliens */
            return (playerType == I_Marine || playerType == I_Alien);

        case I_BehaviourMarine:
        case I_BehaviourSeal:
            /* Marines are threats to Aliens and Predators */
            return (playerType == I_Alien || playerType == I_Predator);

        case I_BehaviourXenoborg:
            /* Xenoborgs are threats to everyone */
            return 1;

        default:
            return 0;
    }
}

extern "C" void AudioRadar_Update(void)
{
    if (!Accessibility_IsAvailable() || !AccessibilitySettings.audio_radar_enabled) {
        return;
    }

    /* Check if enough time has passed since last update */
    static int frameCount = 0;
    frameCount++;

    /* Update roughly every 30 frames (0.5 seconds at 60fps) */
    if (frameCount < 30) {
        return;
    }
    frameCount = 0;

    /* Check priority system - radar is LOW priority, skip during cooldowns */
    if (!Announcement_IsAllowed(ANNOUNCE_PRIORITY_LOW)) {
        return;
    }

    /* Scan for nearby threats */
    AudioRadar_AnnounceNearestThreat();
}

extern "C" void AudioRadar_ScanNow(void)
{
    AudioRadar_AnnounceAll();
}

extern "C" void AudioRadar_AnnounceNearestThreat(void)
{
    if (!Accessibility_IsAvailable() || !Player || !Player->ObStrategyBlock) {
        return;
    }

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    if (!playerDyn) return;

    int playerX = playerDyn->Position.vx;
    int playerY = playerDyn->Position.vy;
    int playerZ = playerDyn->Position.vz;

    /* Get player facing direction from view matrix */
    int playerYaw = 0;
    extern VIEWDESCRIPTORBLOCK* Global_VDB_Ptr;
    if (Global_VDB_Ptr) {
        /* Extract yaw from matrix - simplified */
        playerYaw = (int)(atan2((double)Global_VDB_Ptr->VDB_Mat.mat13,
                                (double)Global_VDB_Ptr->VDB_Mat.mat33) * 2048.0 / 3.14159265);
    }

    int nearestDist = AccessibilitySettings.radar_range;
    STRATEGYBLOCK* nearestSB = NULL;

    /* Scan all active strategy blocks */
    extern int NumActiveStBlocks;
    extern STRATEGYBLOCK* ActiveStBlockList[];

    for (int i = 0; i < NumActiveStBlocks; i++) {
        STRATEGYBLOCK* sb = ActiveStBlockList[i];
        if (!sb || !sb->DynPtr) continue;

        /* Skip non-threat entities */
        if (!IsEntityThreat(sb->I_SBtype, AvP.PlayerType)) continue;

        int dist = Accessibility_GetDistance(
            playerX, playerY, playerZ,
            sb->DynPtr->Position.vx,
            sb->DynPtr->Position.vy,
            sb->DynPtr->Position.vz
        );

        if (dist < nearestDist) {
            nearestDist = dist;
            nearestSB = sb;
        }
    }

    /* Play directional tone for nearest threat with enemy-specific pitch */
    if (nearestSB && nearestSB->DynPtr) {
        RadarTone_PlayDirectional(
            nearestSB->DynPtr->Position.vx,
            nearestSB->DynPtr->Position.vy,
            nearestSB->DynPtr->Position.vz,
            playerX, playerY, playerZ,
            playerYaw,
            nearestSB->I_SBtype  /* Pass entity type for distinct tone */
        );
    }
}

extern "C" void AudioRadar_AnnounceAll(void)
{
    if (!Accessibility_IsAvailable() || !Player || !Player->ObStrategyBlock) {
        return;
    }

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    if (!playerDyn) return;

    int playerX = playerDyn->Position.vx;
    int playerY = playerDyn->Position.vy;
    int playerZ = playerDyn->Position.vz;

    int playerYaw = 0;
    extern VIEWDESCRIPTORBLOCK* Global_VDB_Ptr;
    if (Global_VDB_Ptr) {
        playerYaw = (int)(atan2((double)Global_VDB_Ptr->VDB_Mat.mat13,
                                (double)Global_VDB_Ptr->VDB_Mat.mat33) * 2048.0 / 3.14159265);
    }

    extern int NumActiveStBlocks;
    extern STRATEGYBLOCK* ActiveStBlockList[];

    int enemyCount = 0;
    char fullAnnouncement[1024] = "Radar scan: ";
    char buffer[128];

    for (int i = 0; i < NumActiveStBlocks && enemyCount < AccessibilitySettings.radar_max_enemies; i++) {
        STRATEGYBLOCK* sb = ActiveStBlockList[i];
        if (!sb || !sb->DynPtr) continue;

        RADAR_ENTITY_TYPE type = GetRadarEntityType(sb->I_SBtype);
        if (type == RADAR_ENTITY_UNKNOWN) continue;

        /* Skip non-threats for this scan unless they're items */
        int isThreat = IsEntityThreat(sb->I_SBtype, AvP.PlayerType);
        if (!isThreat && type != RADAR_ENTITY_DOOR && type != RADAR_ENTITY_LIFT) continue;

        int dist = Accessibility_GetDistance(
            playerX, playerY, playerZ,
            sb->DynPtr->Position.vx,
            sb->DynPtr->Position.vy,
            sb->DynPtr->Position.vz
        );

        if (dist > AccessibilitySettings.radar_range) continue;

        AUDIO_DIRECTION dir = Accessibility_GetDirection(
            playerX, playerY, playerZ,
            sb->DynPtr->Position.vx,
            sb->DynPtr->Position.vy,
            sb->DynPtr->Position.vz,
            playerYaw
        );

        snprintf(buffer, sizeof(buffer), "%s %s. ",
                 AudioRadar_GetEntityTypeName(type),
                 AudioRadar_GetDirectionName(dir));

        strncat(fullAnnouncement, buffer, sizeof(fullAnnouncement) - strlen(fullAnnouncement) - 1);
        enemyCount++;
    }

    if (enemyCount == 0) {
        strcat(fullAnnouncement, "no contacts nearby.");
    }

    TTS_Speak(fullAnnouncement);
}

/* ============================================
 * Player State Implementation
 * ============================================ */

extern "C" void PlayerState_Update(void)
{
    if (!Accessibility_IsAvailable() || !AccessibilitySettings.state_announcements_enabled) {
        return;
    }

    if (!Player || !Player->ObStrategyBlock) return;

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    /* Convert from 16.16 fixed point to integer percentage */
    int currentHealth = (ps->Health >> 16);
    int currentArmor = (ps->Armour >> 16);
    int currentWeapon = (int)ps->SelectedWeaponSlot;

    /* Check for significant health change - lowered threshold from 10 to 5 */
    if (g_LastHealth >= 0 && currentHealth != g_LastHealth) {
        int healthLost = g_LastHealth - currentHealth;
        if (healthLost > 5) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "Taking damage! Health %d", currentHealth);
            TTS_SpeakPriority(buffer);
            Announcement_RecordTime(ANNOUNCE_PRIORITY_CRITICAL);  /* Triggers cooldown */
        }
    }

    /* Low health warning */
    if (currentHealth > 0 && currentHealth <= AccessibilitySettings.health_warning_threshold) {
        if (!g_HealthWarningGiven) {
            TTS_SpeakPriority("Warning! Health critical!");
            Announcement_RecordTime(ANNOUNCE_PRIORITY_CRITICAL);
            g_HealthWarningGiven = 1;
        }
    } else {
        g_HealthWarningGiven = 0;
    }

    /* Weapon change announcement - NORMAL priority, skip during damage cooldown */
    if (g_LastWeaponSlot >= 0 && currentWeapon != g_LastWeaponSlot &&
        Announcement_IsAllowed(ANNOUNCE_PRIORITY_NORMAL)) {
        PlayerState_AnnounceWeapon();
        Announcement_RecordTime(ANNOUNCE_PRIORITY_NORMAL);
    }

    /* ============================================
     * Predator Equipment Tracking
     * ============================================ */
    if (AvP.PlayerType == I_Predator) {
        /* Cloak state tracking */
        int currentCloak = ps->cloakOn;
        if (g_LastCloakOn >= 0 && currentCloak != g_LastCloakOn &&
            Announcement_IsAllowed(ANNOUNCE_PRIORITY_NORMAL)) {
            if (currentCloak) {
                TTS_SpeakQueued("Cloak on.");
            } else {
                TTS_SpeakQueued("Cloak off.");
            }
            Announcement_RecordTime(ANNOUNCE_PRIORITY_NORMAL);
        }
        g_LastCloakOn = currentCloak;

        /* Vision mode tracking */
        int currentVision = (int)CurrentVisionMode;
        if (g_LastVisionMode >= 0 && currentVision != g_LastVisionMode &&
            Announcement_IsAllowed(ANNOUNCE_PRIORITY_NORMAL)) {
            const char* modeName = "Normal vision";
            switch (CurrentVisionMode) {
                case VISION_MODE_NORMAL: modeName = "Normal vision"; break;
                case VISION_MODE_PRED_THERMAL: modeName = "Thermal vision"; break;
                case VISION_MODE_PRED_SEEALIENS: modeName = "Alien vision"; break;
                case VISION_MODE_PRED_SEEPREDTECH: modeName = "Tech vision"; break;
                default: modeName = "Vision mode changed"; break;
            }
            TTS_SpeakQueued(modeName);
            Announcement_RecordTime(ANNOUNCE_PRIORITY_NORMAL);
        }
        g_LastVisionMode = currentVision;

        /* Field charge (energy) tracking - announce on significant changes */
        int currentChargePercent = (ps->FieldCharge * 100) / PLAYERCLOAK_MAXENERGY;
        if (g_LastFieldChargePercent >= 0) {
            /* Announce when crossing 25%, 50%, 75% thresholds or hitting low */
            int lastQuarter = g_LastFieldChargePercent / 25;
            int currentQuarter = currentChargePercent / 25;

            if (currentQuarter != lastQuarter && currentChargePercent < g_LastFieldChargePercent &&
                Announcement_IsAllowed(ANNOUNCE_PRIORITY_NORMAL)) {
                char buffer[64];
                if (currentChargePercent <= 10) {
                    snprintf(buffer, sizeof(buffer), "Energy critical! %d percent.", currentChargePercent);
                    TTS_SpeakPriority(buffer);
                    Announcement_RecordTime(ANNOUNCE_PRIORITY_HIGH);
                } else if (currentChargePercent <= 25) {
                    snprintf(buffer, sizeof(buffer), "Energy low. %d percent.", currentChargePercent);
                    TTS_SpeakQueued(buffer);
                    Announcement_RecordTime(ANNOUNCE_PRIORITY_NORMAL);
                }
            }
        }
        g_LastFieldChargePercent = currentChargePercent;
    }

    /* Update tracked values */
    g_LastHealth = currentHealth;
    g_LastArmor = currentArmor;
    g_LastWeaponSlot = currentWeapon;
}

extern "C" void PlayerState_AnnounceHealth(void)
{
    if (!Player || !Player->ObStrategyBlock) return;

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    int health = (ps->Health >> 16);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Health: %d percent", health);
    TTS_Speak(buffer);
}

extern "C" void PlayerState_AnnounceArmor(void)
{
    if (!Player || !Player->ObStrategyBlock) return;

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    int armor = (ps->Armour >> 16);
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Armor: %d percent", armor);
    TTS_Speak(buffer);
}

/* Announce Predator energy cell / field charge status */
extern "C" void Accessibility_AnnounceEnergy(void)
{
    if (!Player || !Player->ObStrategyBlock) return;

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    char buffer[128];

    /* Only Predator has energy cells */
    if (AvP.PlayerType == I_Predator) {
        int energyPercent = (ps->FieldCharge * 100) / PLAYERCLOAK_MAXENERGY;
        snprintf(buffer, sizeof(buffer), "Energy: %d percent", energyPercent);
        TTS_Speak(buffer);
    } else if (AvP.PlayerType == I_Marine) {
        /* Marines don't have energy cells */
        TTS_Speak("Marines do not have energy cells.");
    } else if (AvP.PlayerType == I_Alien) {
        /* Aliens don't have energy cells */
        TTS_Speak("Aliens do not have energy cells.");
    }
}

extern "C" void PlayerState_AnnounceWeapon(void)
{
    if (!Player || !Player->ObStrategyBlock) return;

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    /* Weapon names based on player type and slot */
    const char* weaponName = "unknown weapon";

    /* This is simplified - would need full weapon name lookup */
    switch (AvP.PlayerType) {
        case I_Marine:
            switch (ps->SelectedWeaponSlot) {
                case 0: weaponName = "Pulse Rifle"; break;
                case 1: weaponName = "Smartgun"; break;
                case 2: weaponName = "Flamethrower"; break;
                case 3: weaponName = "Shotgun"; break;
                case 4: weaponName = "Grenade Launcher"; break;
                case 5: weaponName = "Minigun"; break;
                case 6: weaponName = "SADAR Rocket Launcher"; break;
                default: weaponName = "Unknown Marine Weapon"; break;
            }
            break;

        case I_Predator:
            switch (ps->SelectedWeaponSlot) {
                case 0: weaponName = "Wristblades"; break;
                case 1: weaponName = "Speargun"; break;
                case 2: weaponName = "Plasma Caster"; break;
                case 3: weaponName = "Disc"; break;
                case 4: weaponName = "Medicomp"; break;
                default: weaponName = "Unknown Predator Weapon"; break;
            }
            break;

        case I_Alien:
            switch (ps->SelectedWeaponSlot) {
                case 0: weaponName = "Claws"; break;
                case 1: weaponName = "Tail"; break;
                case 2: weaponName = "Jaw"; break;
                default: weaponName = "Unknown Alien Attack"; break;
            }
            break;
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Weapon: %s", weaponName);
    TTS_Speak(buffer);
}

extern "C" void PlayerState_AnnounceAmmo(void)
{
    if (!Player || !Player->ObStrategyBlock) return;

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    /* Aliens don't have ammo */
    if (AvP.PlayerType == I_Alien) {
        TTS_Speak("Alien attacks don't require ammunition");
        return;
    }

    int slot = (int)ps->SelectedWeaponSlot;
    if (slot >= 0 && slot < MAX_NO_OF_WEAPON_SLOTS) {
        int ammo = ps->WeaponSlot[slot].PrimaryRoundsRemaining +
                   ps->WeaponSlot[slot].SecondaryRoundsRemaining;

        char buffer[64];
        if (ammo > 0) {
            snprintf(buffer, sizeof(buffer), "Ammo: %d rounds", ammo);
        } else {
            snprintf(buffer, sizeof(buffer), "Out of ammo!");
        }
        TTS_Speak(buffer);
    }
}

extern "C" void PlayerState_AnnounceAll(void)
{
    PlayerState_AnnounceHealth();
    PlayerState_AnnounceArmor();
    PlayerState_AnnounceWeapon();
    PlayerState_AnnounceAmmo();
}

/* ============================================
 * Navigation Implementation
 * ============================================ */

extern "C" void Navigation_Update(void)
{
    if (!Accessibility_IsAvailable() || !AccessibilitySettings.navigation_cues_enabled) {
        return;
    }

    /* Navigation updates happen less frequently */
    static int frameCount = 0;
    frameCount++;
    if (frameCount < 60) return;  /* Every ~1 second */
    frameCount = 0;

    Navigation_CheckDoors();
}

extern "C" void Navigation_CheckWalls(void)
{
    /* Would require collision/raycast implementation */
    /* For now, this is a placeholder */
    TTS_Speak("Wall detection not yet implemented");
}

extern "C" void Navigation_CheckDoors(void)
{
    if (!Player || !Player->ObStrategyBlock) return;

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    if (!playerDyn) return;

    extern int NumActiveStBlocks;
    extern STRATEGYBLOCK* ActiveStBlockList[];

    for (int i = 0; i < NumActiveStBlocks; i++) {
        STRATEGYBLOCK* sb = ActiveStBlockList[i];
        if (!sb || !sb->DynPtr) continue;

        /* Check for doors */
        if (sb->I_SBtype == I_BehaviourProximityDoor ||
            sb->I_SBtype == I_BehaviourLiftDoor ||
            sb->I_SBtype == I_BehaviourSwitchDoor) {

            int dist = Accessibility_GetDistance(
                playerDyn->Position.vx, playerDyn->Position.vy, playerDyn->Position.vz,
                sb->DynPtr->Position.vx, sb->DynPtr->Position.vy, sb->DynPtr->Position.vz
            );

            /* Announce doors within close range */
            if (dist < 5000) {  /* ~5 meters */
                int playerYaw = 0;
                extern VIEWDESCRIPTORBLOCK* Global_VDB_Ptr;
                if (Global_VDB_Ptr) {
                    playerYaw = (int)(atan2((double)Global_VDB_Ptr->VDB_Mat.mat13,
                                           (double)Global_VDB_Ptr->VDB_Mat.mat33) * 2048.0 / 3.14159265);
                }

                AUDIO_DIRECTION dir = Accessibility_GetDirection(
                    playerDyn->Position.vx, playerDyn->Position.vy, playerDyn->Position.vz,
                    sb->DynPtr->Position.vx, sb->DynPtr->Position.vy, sb->DynPtr->Position.vz,
                    playerYaw
                );

                char buffer[128];
                snprintf(buffer, sizeof(buffer), "Door %s", AudioRadar_GetDirectionName(dir));
                TTS_SpeakQueued(buffer);
                return;  /* Only announce one door at a time */
            }
        }
    }
}

extern "C" void Navigation_AnnounceLocation(void)
{
    /* Announce current level - with redundancy prevention */
    const char* levelName = "Unknown Location";

    extern ELO* Env_List[];
    if (AvP.CurrentEnv >= 0 && AvP.CurrentEnv < I_Num_Environments && Env_List[AvP.CurrentEnv]) {
        levelName = Env_List[AvP.CurrentEnv]->main;
    }

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "Current location: %s", levelName);

    /* Only announce if different from last time */
    if (strcmp(buffer, g_LastLocationText) != 0) {
        TTS_Speak(buffer);
        strncpy(g_LastLocationText, buffer, sizeof(g_LastLocationText) - 1);
        g_LastLocationText[sizeof(g_LastLocationText) - 1] = '\0';
    }
}

/* ============================================
 * Pitch Indicator (View Angle Feedback)
 * ============================================ */

extern "C" void PitchIndicator_Update(void)
{
    if (!Accessibility_IsAvailable() || !g_PitchIndicatorEnabled) {
        return;
    }

    /* Only update every ~20 frames to avoid constant beeping */
    static int frameCount = 0;
    frameCount++;
    if (frameCount < 20) return;
    frameCount = 0;

    /* Get player's view pitch angle */
    extern VIEWDESCRIPTORBLOCK* Global_VDB_Ptr;
    if (!Global_VDB_Ptr) return;

    int pitchAngle = Global_VDB_Ptr->VDB_MatrixEuler.EulerX;

    /* Determine pitch zone: -1 = looking down, 0 = level, 1 = looking up */
    /* Threshold: ~15 degrees (about 170 in game units assuming 4096 = 360 degrees) */
    int pitchZone = 0;
    int threshold = 170;

    if (pitchAngle > threshold) {
        pitchZone = 1;  /* Looking up */
    } else if (pitchAngle < -threshold) {
        pitchZone = -1; /* Looking down */
    }

    /* Only play tone when pitch zone changes and is not level */
    if (pitchZone != g_LastPitchZone) {
        g_LastPitchZone = pitchZone;

        if (pitchZone != 0) {
            /* Play the pitch indicator tone */
            PitchTone_Play(pitchAngle);

            /* Optionally announce on first entry to extreme pitch */
            if (pitchZone == 1 && pitchAngle > 500) {
                /* Very steep up - don't spam TTS, just use tone */
            } else if (pitchZone == -1 && pitchAngle < -500) {
                /* Very steep down - don't spam TTS, just use tone */
            }
        }
    }
    /* If still in same non-level zone, play periodic tone */
    else if (pitchZone != 0) {
        static int toneCount = 0;
        toneCount++;
        if (toneCount >= 3) {  /* Play tone every ~60 frames when looking up/down */
            toneCount = 0;
            PitchTone_Play(pitchAngle);
        }
    }
}

/* ============================================
 * Menu Accessibility
 * ============================================ */

/* Track when menu was last announced to prevent render hook from double-announcing */
static unsigned int g_LastMenuAnnouncementTime = 0;
#define MENU_ANNOUNCEMENT_COOLDOWN_MS 2000  /* Don't re-announce within 2 seconds */

extern "C" void Menu_OnTextDisplayed(const char* text, int isSelected)
{
    if (!Accessibility_IsAvailable() || !AccessibilitySettings.menu_narration_enabled) {
        return;
    }

    if (!text || strlen(text) == 0) return;

    /* Check cooldown - if explicit announcement was made recently, skip render hook */
    unsigned int currentTime = GetTickCount();
    if ((currentTime - g_LastMenuAnnouncementTime) < MENU_ANNOUNCEMENT_COOLDOWN_MS) {
        return;  /* Recent explicit announcement, skip this render-triggered one */
    }

    /* Only announce if text changed and is selected */
    if (isSelected && strcmp(text, g_LastMenuText) != 0) {
        TTS_Speak(text);
        strncpy(g_LastMenuText, text, sizeof(g_LastMenuText) - 1);
        g_LastMenuText[sizeof(g_LastMenuText) - 1] = '\0';
        g_LastMenuAnnouncementTime = currentTime;
    }
}

/* Called by explicit menu announcement functions to set cooldown
 * This prevents render hooks from double-announcing after explicit TTS */
extern "C" void Menu_SetAnnouncementCooldown(void)
{
    g_LastMenuAnnouncementTime = GetTickCount();
}

extern "C" void Menu_AnnounceCurrentItem(void)
{
    if (strlen(g_LastMenuText) > 0) {
        TTS_Speak(g_LastMenuText);
    }
}

extern "C" void Menu_OnNavigate(int direction)
{
    if (!Accessibility_IsAvailable()) return;

    switch (direction) {
        case -1: /* Up */
            /* Item will be announced by Menu_OnTextDisplayed */
            break;
        case 1:  /* Down */
            /* Item will be announced by Menu_OnTextDisplayed */
            break;
        case 0:  /* Select */
            TTS_SpeakQueued("Selected");
            break;
    }
}

/* ============================================
 * Debug and Logging
 * ============================================ */

extern "C" void Accessibility_SetDebugMode(int enabled)
{
    g_DebugMode = enabled;
}

extern "C" void Accessibility_Log(const char* format, ...)
{
    if (!g_DebugMode) return;

    va_list args;
    va_start(args, format);

    fprintf(stderr, "[Accessibility] ");
    vfprintf(stderr, format, args);

    va_end(args);
}

/* ============================================
 * Input Processing
 * ============================================ */

extern "C" void Accessibility_ProcessInput(void)
{
    /* This would be called from the input handler */
    /* Check for accessibility hotkeys */

    extern unsigned char KeyboardInput[];
    extern unsigned char DebouncedKeyboardInput[];

    /* F1 - Toggle accessibility */
    if (DebouncedKeyboardInput[KEY_F1]) {
        AccessibilitySettings.enabled = !AccessibilitySettings.enabled;
        TTS_SpeakPriority(AccessibilitySettings.enabled ?
                          "Accessibility enabled" : "Accessibility disabled");
    }

    /* F2 - Toggle audio radar */
    if (DebouncedKeyboardInput[KEY_F2]) {
        AccessibilitySettings.audio_radar_enabled = !AccessibilitySettings.audio_radar_enabled;
        TTS_Speak(AccessibilitySettings.audio_radar_enabled ?
                  "Audio radar enabled" : "Audio radar disabled");
    }

    /* F3 - Announce health/armor */
    if (DebouncedKeyboardInput[KEY_F3]) {
        PlayerState_AnnounceHealth();
        PlayerState_AnnounceArmor();
    }

    /* F4 - Announce weapon/ammo */
    if (DebouncedKeyboardInput[KEY_F4]) {
        PlayerState_AnnounceWeapon();
        PlayerState_AnnounceAmmo();
    }

    /* F5 - Full radar scan */
    if (DebouncedKeyboardInput[KEY_F5]) {
        AudioRadar_ScanNow();
    }

    /* F6 - Announce location */
    if (DebouncedKeyboardInput[KEY_F6]) {
        Navigation_AnnounceLocation();
    }

    /* F7 - Repeat last announcement */
    if (DebouncedKeyboardInput[KEY_F7]) {
        if (strlen(g_LastSpokenText) > 0) {
            TTS_Speak(g_LastSpokenText);
        }
    }

    /* F8 - Toggle TTS */
    if (DebouncedKeyboardInput[KEY_F8]) {
        AccessibilitySettings.tts_enabled = !AccessibilitySettings.tts_enabled;
        /* Can't announce this if TTS is now disabled! */
        if (AccessibilitySettings.tts_enabled) {
            TTS_Speak("Text to speech enabled");
        }
    }

    /* F9 - Announce mission objectives */
    if (DebouncedKeyboardInput[KEY_F9]) {
        Mission_AnnounceObjectives();
    }

    /* F10 - Toggle pitch indicator */
    if (DebouncedKeyboardInput[KEY_F10]) {
        g_PitchIndicatorEnabled = !g_PitchIndicatorEnabled;
        TTS_Speak(g_PitchIndicatorEnabled ?
                  "Pitch indicator enabled" : "Pitch indicator disabled");
    }

    /* F11 - Scan for interactive elements (switches, doors, lifts) */
    if (DebouncedKeyboardInput[KEY_F11]) {
        Interactive_ScanAndAnnounce();
    }

    /* F12 - Full status announcement */
    if (DebouncedKeyboardInput[KEY_F12]) {
        Accessibility_AnnounceFullStatus();
    }

    /* ============================================
     * Autonavigation Controls (Insert/Home/End/PgUp/PgDown)
     * ============================================ */

    /* Insert - Toggle autonavigation on/off */
    if (DebouncedKeyboardInput[KEY_INS]) {
        AutoNav_Toggle();
    }

    /* Home - Toggle auto-rotation toward target */
    if (DebouncedKeyboardInput[KEY_HOME]) {
        AutoNav_ToggleRotation();
    }

    /* End - Toggle auto-movement toward target */
    if (DebouncedKeyboardInput[KEY_END]) {
        AutoNav_ToggleMovement();
    }

    /* PageUp - Cycle target type (interactive/NPC/exit/item) */
    if (DebouncedKeyboardInput[KEY_PAGEUP]) {
        AutoNav_CycleTargetType();
    }

    /* PageDown - Find nearest target and announce */
    if (DebouncedKeyboardInput[KEY_PAGEDOWN]) {
        AutoNav_FindTarget();
        AutoNav_AnnounceTarget();
    }

    /* ============================================
     * Obstruction Detection Controls (Del/Backslash/Grave)
     * ============================================ */

    /* Delete - Announce what's directly ahead */
    if (DebouncedKeyboardInput[KEY_DEL]) {
        Obstruction_AnnounceAhead();
    }

    /* Backslash - Announce surroundings (all directions) */
    if (DebouncedKeyboardInput[KEY_BACKSLASH]) {
        Obstruction_AnnounceSurroundings();
    }

    /* Grave/Tilde (~) - Toggle obstruction alerts */
    if (DebouncedKeyboardInput[KEY_GRAVE]) {
        Obstruction_Toggle();
    }

    /* Tab - Environment description */
    if (DebouncedKeyboardInput[KEY_TAB]) {
        Environment_Describe();
    }

    /* H - Health announcement (all characters) */
    if (DebouncedKeyboardInput[KEY_H]) {
        PlayerState_AnnounceHealth();
    }

    /* C - Energy cell status (Predator only) */
    if (DebouncedKeyboardInput[KEY_C]) {
        Accessibility_AnnounceEnergy();
    }

    /* M - Mission objectives */
    if (DebouncedKeyboardInput[KEY_M]) {
        Mission_AnnounceObjectives();
    }

    /* ============================================
     * IJKL / Numpad - Rotation and Vertical Look Control
     * J/Numpad4 = Rotate Left, L/Numpad6 = Rotate Right
     * I/Numpad8 = Look Up, K/Numpad2 = Look Down
     * ============================================ */

    /* Only process look keys when in-game (not in menus) */
    if (Player && Player->ObStrategyBlock && Player->ObStrategyBlock->DynPtr) {
        DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
        PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);

        if (ps) {
            /* J / Numpad4 - Rotate left */
            if (KeyboardInput[KEY_J] || KeyboardInput[KEY_NUMPAD4]) {
                playerDyn->AngVelocity.EulerY = -ARROW_ROTATION_SPEED;
            }
            /* L / Numpad6 - Rotate right */
            else if (KeyboardInput[KEY_L] || KeyboardInput[KEY_NUMPAD6]) {
                playerDyn->AngVelocity.EulerY = ARROW_ROTATION_SPEED;
            }

            /* I / Numpad8 - Look up */
            if (KeyboardInput[KEY_I] || KeyboardInput[KEY_NUMPAD8]) {
                ps->ViewPanX -= ARROW_PITCH_SPEED;
                /* Clamp pitch to prevent extreme angles */
                if (ps->ViewPanX < -1536) ps->ViewPanX = -1536;  /* Max look up */
            }
            /* K / Numpad2 - Look down */
            else if (KeyboardInput[KEY_K] || KeyboardInput[KEY_NUMPAD2]) {
                ps->ViewPanX += ARROW_PITCH_SPEED;
                /* Clamp pitch to prevent extreme angles */
                if (ps->ViewPanX > 1536) ps->ViewPanX = 1536;  /* Max look down */
            }

            /* Centering tone detection - play tone when pitch returns to center */
            int currentPitchCentered = (ps->ViewPanX >= -PITCH_CENTER_THRESHOLD &&
                                        ps->ViewPanX <= PITCH_CENTER_THRESHOLD);
            unsigned int currentTime = GetTickCount();

            /* If pitch was off-center and now it's centered, play centering tone */
            if (g_WasPitchOffCenter && currentPitchCentered) {
                /* Debounce: only play if 300ms since last centering tone */
                if ((currentTime - g_LastCenteringToneTime) > 300) {
                    CenteringTone_Play();
                    g_LastCenteringToneTime = currentTime;
                }
            }

            /* Update off-center tracking */
            g_WasPitchOffCenter = !currentPitchCentered;
        }
    }
}

/* ============================================
 * Interactive Element Scanning
 * ============================================ */

/* Check if an entity type is an interactive element */
static int IsInteractiveElement(AVP_BEHAVIOUR_TYPE bhvr)
{
    switch (bhvr) {
        case I_BehaviourProximityDoor:
        case I_BehaviourLiftDoor:
        case I_BehaviourSwitchDoor:
        case I_BehaviourLift:
        case I_BehaviourBinarySwitch:
        case I_BehaviourLinkSwitch:
        case I_BehaviourGenerator:
        case I_BehaviourDatabase:
            return 1;
        default:
            return 0;
    }
}

/* Structure to hold interactive element info for sorting */
typedef struct {
    RADAR_ENTITY_TYPE type;
    AUDIO_DIRECTION direction;
    int distance;
    int posX, posY, posZ;
} INTERACTIVE_ELEMENT_INFO;

/* Compare function for sorting by distance (nearest first) */
static int CompareByDistance(const void* a, const void* b)
{
    const INTERACTIVE_ELEMENT_INFO* elemA = (const INTERACTIVE_ELEMENT_INFO*)a;
    const INTERACTIVE_ELEMENT_INFO* elemB = (const INTERACTIVE_ELEMENT_INFO*)b;
    return elemA->distance - elemB->distance;
}

extern "C" void Interactive_ScanAndAnnounce(void)
{
    if (!Accessibility_IsAvailable() || !Player || !Player->ObStrategyBlock) {
        TTS_Speak("Cannot scan: player not available");
        return;
    }

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    if (!playerDyn) {
        TTS_Speak("Cannot scan: player position unavailable");
        return;
    }

    int playerX = playerDyn->Position.vx;
    int playerY = playerDyn->Position.vy;
    int playerZ = playerDyn->Position.vz;

    /* Get player facing direction */
    int playerYaw = 0;
    extern VIEWDESCRIPTORBLOCK* Global_VDB_Ptr;
    if (Global_VDB_Ptr) {
        playerYaw = (int)(atan2((double)Global_VDB_Ptr->VDB_Mat.mat13,
                                (double)Global_VDB_Ptr->VDB_Mat.mat33) * 2048.0 / 3.14159265);
    }

    extern int NumActiveStBlocks;
    extern STRATEGYBLOCK* ActiveStBlockList[];

    /* Collect interactive elements (max 20) */
    #define MAX_INTERACTIVE_ELEMENTS 20
    INTERACTIVE_ELEMENT_INFO elements[MAX_INTERACTIVE_ELEMENTS];
    int elementCount = 0;

    /* Extended range for interactive elements (mission objectives might be far) */
    int scanRange = AccessibilitySettings.radar_range * 2;

    for (int i = 0; i < NumActiveStBlocks && elementCount < MAX_INTERACTIVE_ELEMENTS; i++) {
        STRATEGYBLOCK* sb = ActiveStBlockList[i];
        if (!sb || !sb->DynPtr) continue;

        /* Only include interactive elements */
        if (!IsInteractiveElement(sb->I_SBtype)) continue;

        int dist = Accessibility_GetDistance(
            playerX, playerY, playerZ,
            sb->DynPtr->Position.vx,
            sb->DynPtr->Position.vy,
            sb->DynPtr->Position.vz
        );

        if (dist > scanRange) continue;

        /* Store element info */
        elements[elementCount].type = GetRadarEntityType(sb->I_SBtype);
        elements[elementCount].distance = dist;
        elements[elementCount].posX = sb->DynPtr->Position.vx;
        elements[elementCount].posY = sb->DynPtr->Position.vy;
        elements[elementCount].posZ = sb->DynPtr->Position.vz;
        elements[elementCount].direction = Accessibility_GetDirection(
            playerX, playerY, playerZ,
            sb->DynPtr->Position.vx,
            sb->DynPtr->Position.vy,
            sb->DynPtr->Position.vz,
            playerYaw
        );
        elementCount++;
    }

    if (elementCount == 0) {
        TTS_Speak("No interactive elements detected nearby.");
        return;
    }

    /* Sort by distance (nearest first) */
    qsort(elements, elementCount, sizeof(INTERACTIVE_ELEMENT_INFO), CompareByDistance);

    /* Build announcement */
    char announcement[1024] = "Interactive elements: ";
    char buffer[128];

    /* Announce up to 8 nearest elements */
    int announceCount = (elementCount > 8) ? 8 : elementCount;

    for (int i = 0; i < announceCount; i++) {
        const char* typeName = AudioRadar_GetEntityTypeName(elements[i].type);
        const char* dirName = AudioRadar_GetDirectionName(elements[i].direction);
        const char* distName = Accessibility_FormatDistance(elements[i].distance);

        snprintf(buffer, sizeof(buffer), "%s %s, %s. ",
                 typeName, dirName, distName);

        strncat(announcement, buffer, sizeof(announcement) - strlen(announcement) - 1);
    }

    if (elementCount > announceCount) {
        snprintf(buffer, sizeof(buffer), "And %d more.", elementCount - announceCount);
        strncat(announcement, buffer, sizeof(announcement) - strlen(announcement) - 1);
    }

    TTS_Speak(announcement);
}

/* ============================================
 * Mission Objectives
 * ============================================ */

extern "C" void Mission_AnnounceObjectives(void)
{
    if (!Accessibility_IsAvailable()) return;

    static char objectivesBuffer[2048];
    int count = GetMissionObjectivesText(objectivesBuffer, sizeof(objectivesBuffer));

    if (count > 0) {
        char announcement[2100];
        snprintf(announcement, sizeof(announcement), "Mission objectives: %s", objectivesBuffer);
        TTS_Speak(announcement);

        /* After announcing objectives, also announce nearby interactive elements
         * This provides directional guidance even though objectives don't have positions */
        TTS_SpeakQueued("Scanning for interactive elements...");

        /* Brief delay for TTS queue, then announce interactives */
        Interactive_ScanAndAnnounce();
    } else {
        TTS_Speak(objectivesBuffer);  /* Will say "No active objectives." */

        /* Still scan for interactive elements as guidance */
        TTS_SpeakQueued("Scanning for interactive elements...");
        Interactive_ScanAndAnnounce();
    }
}

/* ============================================
 * On-Screen Message Hook
 * ============================================ */

/* Called when a message is displayed on screen - announce via TTS */
extern "C" void Accessibility_OnScreenMessage(const char* message)
{
    if (!Accessibility_IsAvailable() || !message || strlen(message) == 0) {
        return;
    }

    /* Prevent duplicate announcements of the same message */
    if (strcmp(message, g_LastOnScreenMessage) == 0) {
        return;
    }

    /* Store for duplicate prevention */
    strncpy(g_LastOnScreenMessage, message, sizeof(g_LastOnScreenMessage) - 1);
    g_LastOnScreenMessage[sizeof(g_LastOnScreenMessage) - 1] = '\0';

    /* Announce the message */
    TTS_SpeakQueued(message);
}

/* ============================================
 * Interaction Detection System
 * ============================================ */

/* Activation ranges from triggers.c */
#define ACTIVATION_Z_RANGE 3000
#define ACTIVATION_X_RANGE 1000
#define ACTIVATION_Y_RANGE 1000

/* Check if an object is an operable/interactive type */
static int IsOperableObject(AVP_BEHAVIOUR_TYPE behaviour)
{
    return (behaviour == I_BehaviourBinarySwitch ||
            behaviour == I_BehaviourLinkSwitch ||
            behaviour == I_BehaviourAutoGun ||
            behaviour == I_BehaviourDatabase);
}

/* Get a friendly name for the interactive object type */
static const char* GetInteractiveTypeName(AVP_BEHAVIOUR_TYPE behaviour)
{
    switch (behaviour) {
        case I_BehaviourBinarySwitch: return "switch";
        case I_BehaviourLinkSwitch: return "switch";
        case I_BehaviourAutoGun: return "turret control";
        case I_BehaviourDatabase: return "terminal";
        default: return "object";
    }
}

/* Check for nearby interactive objects and announce "Press SPACE" */
extern "C" void Accessibility_CheckInteraction(void)
{
    if (!Accessibility_IsAvailable() || !AccessibilitySettings.navigation_cues_enabled) {
        return;
    }

    /* Only check every 15 frames to avoid spam */
    g_InteractionCheckFrames++;
    if (g_InteractionCheckFrames < 15) {
        return;
    }
    g_InteractionCheckFrames = 0;

    /* Check if player exists */
    if (!Player || !Player->ObStrategyBlock) {
        return;
    }

    int numberOfObjects = NumOnScreenBlocks;
    DISPLAYBLOCK* nearestObjectPtr = NULL;
    int nearestMagnitude = ACTIVATION_X_RANGE * ACTIVATION_X_RANGE + ACTIVATION_Y_RANGE * ACTIVATION_Y_RANGE;
    AVP_BEHAVIOUR_TYPE nearestBehaviour = I_BehaviourNull;

    while (numberOfObjects) {
        DISPLAYBLOCK* objectPtr = OnScreenBlockList[--numberOfObjects];
        if (!objectPtr) continue;

        /* Does object have a strategy block? */
        if (objectPtr->ObStrategyBlock) {
            AVP_BEHAVIOUR_TYPE behaviour = objectPtr->ObStrategyBlock->I_SBtype;

            /* Is it operable? */
            if (IsOperableObject(behaviour)) {
                /* Is it in range? */
                if (objectPtr->ObView.vz > 0 && objectPtr->ObView.vz < ACTIVATION_Z_RANGE) {
                    int absX = objectPtr->ObView.vx;
                    int absY = objectPtr->ObView.vy;

                    if (absX < 0) absX = -absX;
                    if (absY < 0) absY = -absY;

                    if (absX < ACTIVATION_X_RANGE && absY < ACTIVATION_Y_RANGE) {
                        int magnitude = (absX * absX + absY * absY);

                        if (nearestMagnitude > magnitude) {
                            nearestMagnitude = magnitude;
                            nearestObjectPtr = objectPtr;
                            nearestBehaviour = behaviour;
                        }
                    }
                }
            }
        }
    }

    /* Check if we found something */
    if (nearestObjectPtr) {
        /* Verify line of sight */
        if (IsThisObjectVisibleFromThisPosition_WithIgnore(Player, nearestObjectPtr,
                &nearestObjectPtr->ObWorld, 10000)) {
            const char* typeName = GetInteractiveTypeName(nearestBehaviour);

            /* Only announce if this is a new interactive or type changed, AND priority allows */
            if ((!g_LastInteractiveNearby || strcmp(typeName, g_LastInteractiveType) != 0) &&
                Announcement_IsAllowed(ANNOUNCE_PRIORITY_HIGH)) {
                char announcement[128];
                snprintf(announcement, sizeof(announcement), "%s nearby. Press SPACE to interact.", typeName);
                TTS_Speak(announcement);
                Announcement_RecordTime(ANNOUNCE_PRIORITY_HIGH);

                strncpy(g_LastInteractiveType, typeName, sizeof(g_LastInteractiveType) - 1);
                g_LastInteractiveType[sizeof(g_LastInteractiveType) - 1] = '\0';
            }
            g_LastInteractiveNearby = 1;
            return;
        }
    }

    /* No interactive nearby - reset state */
    if (g_LastInteractiveNearby) {
        g_LastInteractiveNearby = 0;
        g_LastInteractiveType[0] = '\0';
    }
}

/* ============================================
 * Weapon State Tracking
 * ============================================ */

/* Get weapon name from weapon ID */
static const char* GetWeaponNameFromID(int weaponID, I_PLAYER_TYPE playerType)
{
    switch (playerType) {
        case I_Marine:
            switch (weaponID) {
                case WEAPON_PULSERIFLE: return "Pulse Rifle";
                case WEAPON_AUTOSHOTGUN: return "Shotgun";
                case WEAPON_SMARTGUN: return "Smartgun";
                case WEAPON_FLAMETHROWER: return "Flamethrower";
                case WEAPON_PLASMAGUN: return "Plasma Gun";
                case WEAPON_SADAR: return "SADAR Rocket Launcher";
                case WEAPON_GRENADELAUNCHER: return "Grenade Launcher";
                case WEAPON_MINIGUN: return "Minigun";
                case WEAPON_MARINE_PISTOL: return "Pistol";
                case WEAPON_TWO_PISTOLS: return "Dual Pistols";
                case WEAPON_CUDGEL: return "Cudgel";
                default: return "Unknown Weapon";
            }
        case I_Predator:
            switch (weaponID) {
                case WEAPON_PRED_WRISTBLADE: return "Wristblades";
                case WEAPON_PRED_PISTOL: return "Speargun";
                case WEAPON_PRED_RIFLE: return "Speargun Rifle";
                case WEAPON_PRED_SHOULDERCANNON: return "Plasma Caster";
                case WEAPON_PRED_DISC: return "Smart Disc";
                case WEAPON_PRED_MEDICOMP: return "Medicomp";
                case WEAPON_PRED_STAFF: return "Combi-Stick";
                default: return "Unknown Weapon";
            }
        case I_Alien:
            switch (weaponID) {
                case WEAPON_ALIEN_CLAW: return "Claws";
                case WEAPON_ALIEN_GRAB: return "Grab Attack";
                case WEAPON_ALIEN_SPIT: return "Acid Spit";
                default: return "Claws";
            }
        default:
            return "Unknown";
    }
}

/* Track weapon state changes and announce */
extern "C" void Accessibility_WeaponStateUpdate(void)
{
    if (!Accessibility_IsAvailable() || !AccessibilitySettings.state_announcements_enabled) {
        return;
    }

    if (!Player || !Player->ObStrategyBlock) return;

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    int currentSlot = (int)ps->SelectedWeaponSlot;
    if (currentSlot < 0 || currentSlot >= MAX_NO_OF_WEAPON_SLOTS) return;

    PLAYER_WEAPON_DATA* weaponPtr = &ps->WeaponSlot[currentSlot];
    if (!weaponPtr) return;

    int weaponID = weaponPtr->WeaponIDNumber;
    int weaponState = weaponPtr->CurrentState;
    int primaryRounds = weaponPtr->PrimaryRoundsRemaining >> 16;
    int secondaryRounds = weaponPtr->SecondaryRoundsRemaining >> 16;

    /* Check for weapon switch */
    if (g_LastWeaponSlot != currentSlot && g_LastWeaponSlot >= 0) {
        const char* weaponName = GetWeaponNameFromID(weaponID, AvP.PlayerType);
        char announcement[128];

        /* Include ammo info for Marines and Predators */
        if (AvP.PlayerType != I_Alien && primaryRounds >= 0) {
            snprintf(announcement, sizeof(announcement), "%s. %d rounds.", weaponName, primaryRounds);
        } else {
            snprintf(announcement, sizeof(announcement), "%s.", weaponName);
        }
        TTS_Speak(announcement);
    }

    /* Check for reload start */
    if (g_LastWeaponState != WEAPONSTATE_RELOAD_PRIMARY &&
        weaponState == WEAPONSTATE_RELOAD_PRIMARY) {
        TTS_SpeakQueued("Reloading.");
    }

    /* Check for out of ammo (when trying to fire with no ammo) */
    if (g_LastPrimaryRounds > 0 && primaryRounds == 0 &&
        weaponState != WEAPONSTATE_RELOAD_PRIMARY) {
        if (AvP.PlayerType != I_Alien) {
            TTS_SpeakPriority("Out of ammo!");
        }
    }

    /* Check for weapon jammed */
    if (g_LastWeaponState != WEAPONSTATE_JAMMED &&
        weaponState == WEAPONSTATE_JAMMED) {
        TTS_SpeakPriority("Weapon jammed!");
    }

    /* Update tracking variables */
    g_LastWeaponSlot = currentSlot;
    g_LastWeaponState = weaponState;
    g_LastPrimaryRounds = primaryRounds;
    g_LastSecondaryRounds = secondaryRounds;
}

/* ============================================
 * Comprehensive Status Announcement (F12)
 * ============================================ */

extern "C" void Accessibility_AnnounceFullStatus(void)
{
    if (!Accessibility_IsAvailable()) return;

    if (!Player || !Player->ObStrategyBlock) {
        TTS_Speak("Status unavailable.");
        return;
    }

    PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
    if (!ps) return;

    char announcement[512];
    int health = (ps->Health >> 16);
    int armor = (ps->Armour >> 16);

    int currentSlot = (int)ps->SelectedWeaponSlot;
    const char* weaponName = "Unknown";
    int ammo = 0;
    int magazines = 0;

    if (currentSlot >= 0 && currentSlot < MAX_NO_OF_WEAPON_SLOTS) {
        PLAYER_WEAPON_DATA* weaponPtr = &ps->WeaponSlot[currentSlot];
        weaponName = GetWeaponNameFromID(weaponPtr->WeaponIDNumber, AvP.PlayerType);
        ammo = weaponPtr->PrimaryRoundsRemaining >> 16;
        magazines = weaponPtr->PrimaryMagazinesRemaining;
    }

    if (AvP.PlayerType == I_Alien) {
        snprintf(announcement, sizeof(announcement),
            "Status: Health %d percent. Weapon: %s.",
            health, weaponName);
    } else {
        snprintf(announcement, sizeof(announcement),
            "Status: Health %d percent. Armor %d percent. Weapon: %s. Ammo: %d rounds, %d magazines.",
            health, armor, weaponName, ammo, magazines);
    }

    TTS_Speak(announcement);
}

/* ============================================
 * Auto-Navigation System Implementation
 * ============================================ */

/* Global autonavigation state */
AUTONAV_STATE AutoNavState = {
    0,      /* enabled */
    0,      /* auto_rotate */
    0,      /* auto_move */
    NAV_TARGET_INTERACTIVE,  /* default target type */
    0, 0, 0,  /* target position */
    0,      /* target_distance */
    NULL    /* target_name */
};

/* Navigation tone system */
#define NAV_TONE_SAMPLE_RATE 44100
#define NAV_TONE_DURATION_MS 80
#define NAV_TONE_SAMPLES (NAV_TONE_SAMPLE_RATE * NAV_TONE_DURATION_MS / 1000)
#define NAV_BASE_FREQUENCY 660.0f  /* E5 note - distinct from radar */

static ALuint g_NavToneBuffer = 0;
static ALuint g_NavToneSource = 0;
static int g_NavToneInitialized = 0;
static int g_NavToneFrameCounter = 0;

/* Generate navigation guidance tone buffer */
static int NavTone_GenerateBuffer(void)
{
    if (g_NavToneBuffer != 0) {
        return 1;
    }

    short* samples = (short*)malloc(NAV_TONE_SAMPLES * sizeof(short));
    if (!samples) return 0;

    /* Generate a pleasant sine wave with quick attack/decay */
    for (int i = 0; i < NAV_TONE_SAMPLES; i++) {
        float t = (float)i / NAV_TONE_SAMPLE_RATE;
        float phase = 2.0f * 3.14159265f * NAV_BASE_FREQUENCY * t;
        float sample = sinf(phase);

        /* Quick envelope */
        float envelope = 1.0f;
        float fadeLen = NAV_TONE_SAMPLES * 0.1f;
        if (i < (int)fadeLen) {
            envelope = (float)i / fadeLen;
        } else if (i > NAV_TONE_SAMPLES - (int)fadeLen) {
            envelope = (float)(NAV_TONE_SAMPLES - i) / fadeLen;
        }

        samples[i] = (short)(sample * envelope * 20000.0f);
    }

    alGenBuffers(1, &g_NavToneBuffer);
    if (alGetError() != AL_NO_ERROR) {
        free(samples);
        return 0;
    }

    alBufferData(g_NavToneBuffer, AL_FORMAT_MONO16, samples,
                 NAV_TONE_SAMPLES * sizeof(short), NAV_TONE_SAMPLE_RATE);

    free(samples);

    if (alGetError() != AL_NO_ERROR) {
        alDeleteBuffers(1, &g_NavToneBuffer);
        g_NavToneBuffer = 0;
        return 0;
    }

    return 1;
}

static int NavTone_Init(void)
{
    if (g_NavToneInitialized) {
        return 1;
    }

    if (!NavTone_GenerateBuffer()) {
        return 0;
    }

    alGenSources(1, &g_NavToneSource);
    if (alGetError() != AL_NO_ERROR) {
        return 0;
    }

    alSourcei(g_NavToneSource, AL_BUFFER, g_NavToneBuffer);
    alSourcef(g_NavToneSource, AL_GAIN, 0.5f);
    alSourcei(g_NavToneSource, AL_SOURCE_RELATIVE, AL_FALSE);

    g_NavToneInitialized = 1;
    return 1;
}

/* Play navigation tone with stereo panning and vertical pitch variation
 * angleOffset: -1.0 = hard left, 0 = center, 1.0 = hard right
 * verticalRatio: -1.0 = below, 0 = same level, 1.0 = above
 */
static void NavTone_PlayDirectional(float angleOffset, float verticalRatio)
{
    if (!g_NavToneInitialized) {
        if (!NavTone_Init()) return;
    }

    /* Check if source is already playing - if so, let it finish (allows parallel sounds) */
    ALint state;
    alGetSourcei(g_NavToneSource, AL_SOURCE_STATE, &state);
    if (state == AL_PLAYING) {
        return;  /* Let current sound finish, don't interrupt */
    }

    /* Use SOURCE_RELATIVE for consistent UI audio that doesn't interfere with game 3D sounds */
    alSourcei(g_NavToneSource, AL_SOURCE_RELATIVE, AL_TRUE);

    /* Map to 3D position for stereo panning */
    float posX = angleOffset * 2.0f;
    float posZ = -1.0f;  /* In front */

    alSource3f(g_NavToneSource, AL_POSITION, posX, 0.0f, posZ);

    /* Calculate pitch based on both horizontal alignment AND vertical offset:
     * - Base pitch 1.0
     * - Higher when on target horizontally (+0.15 to +0.3)
     * - Vertical variation: above = higher pitch, below = lower pitch (+/- 0.5)
     */
    float pitch = 1.0f;

    /* Horizontal alignment bonus */
    float absAngle = (angleOffset < 0) ? -angleOffset : angleOffset;
    if (absAngle < 0.1f) {
        pitch += 0.3f;  /* Higher pitch when on target */
    } else if (absAngle < 0.3f) {
        pitch += 0.15f;
    }

    /* Vertical variation: above = higher pitch, below = lower pitch */
    pitch += verticalRatio * 0.5f;

    /* Clamp pitch to reasonable range */
    if (pitch < 0.5f) pitch = 0.5f;
    if (pitch > 2.0f) pitch = 2.0f;

    alSourcef(g_NavToneSource, AL_PITCH, pitch);

    /* Set a moderate gain that won't overpower game sounds */
    alSourcef(g_NavToneSource, AL_GAIN, 0.4f);

    alSourceRewind(g_NavToneSource);
    alSourcePlay(g_NavToneSource);
}

extern "C" void AutoNav_Init(void)
{
    AutoNavState.enabled = 0;
    AutoNavState.auto_rotate = 0;
    AutoNavState.auto_move = 0;
    AutoNavState.target_type = NAV_TARGET_INTERACTIVE;
    AutoNavState.target_x = 0;
    AutoNavState.target_y = 0;
    AutoNavState.target_z = 0;
    AutoNavState.target_distance = 0;
    AutoNavState.target_name = NULL;

    /* Initialize position history */
    AutoNavState.history_index = 0;
    AutoNavState.history_count = 0;
    memset(AutoNavState.position_history, 0, sizeof(AutoNavState.position_history));

    /* Initialize progress tracking */
    AutoNavState.last_announced_distance = 0;
    AutoNavState.closest_achieved = 999999;
    AutoNavState.last_progress_time = 0;

    /* Initialize strategy management */
    AutoNavState.current_strategy = NAV_STRATEGY_DIRECT;
    AutoNavState.strategy_frames = 0;
    AutoNavState.strategy_failures = 0;

    /* Initialize door/lift tracking */
    AutoNavState.door_wait.doorSB = NULL;
    AutoNavState.door_wait.lastState = 0;
    AutoNavState.door_wait.waitStartTime = 0;
    AutoNavState.door_wait.announced = 0;
    AutoNavState.lift_track.liftSB = NULL;
    AutoNavState.lift_track.lastState = 0;
    AutoNavState.lift_track.playerOnLift = 0;
    AutoNavState.lift_track.rideStartTime = 0;

    /* Initialize arrival handling */
    AutoNavState.arrival_announced = 0;
    AutoNavState.target_reached = 0;
}

/* ============================================
 * Spatial Awareness State
 * ============================================ */

SPATIAL_AWARENESS_STATE SpatialState = {0};

/* ============================================
 * Pathfinding Helper Functions
 * ============================================ */

/* Record current position in history (call every 10 frames) */
extern "C" void PathFind_RecordPosition(int x, int y, int z)
{
    POSITION_RECORD* rec = &AutoNavState.position_history[AutoNavState.history_index];
    rec->x = x;
    rec->y = y;
    rec->z = z;
    rec->timestamp = GetTickCount();

    AutoNavState.history_index = (AutoNavState.history_index + 1) % NAV_POSITION_HISTORY_SIZE;
    if (AutoNavState.history_count < NAV_POSITION_HISTORY_SIZE) {
        AutoNavState.history_count++;
    }
}

/* Detect if player is oscillating (moving back and forth) */
extern "C" int PathFind_DetectOscillation(void)
{
    if (AutoNavState.history_count < 6) return 0;

    /* Look at last 6 positions (~1 second) */
    int sumX = 0, sumZ = 0;
    int sumX2 = 0, sumZ2 = 0;
    int count = 0;

    for (int i = 0; i < 6; i++) {
        int idx = (AutoNavState.history_index - 1 - i + NAV_POSITION_HISTORY_SIZE) % NAV_POSITION_HISTORY_SIZE;
        POSITION_RECORD* rec = &AutoNavState.position_history[idx];
        sumX += rec->x;
        sumZ += rec->z;
        sumX2 += rec->x * rec->x;
        sumZ2 += rec->z * rec->z;
        count++;
    }

    /* Calculate variance */
    int meanX = sumX / count;
    int meanZ = sumZ / count;
    int varX = (sumX2 / count) - (meanX * meanX);
    int varZ = (sumZ2 / count) - (meanZ * meanZ);

    /* Low variance in position but movement detected = oscillating */
    /* Check if total movement is significant but variance is low */
    int oldest = (AutoNavState.history_index - 6 + NAV_POSITION_HISTORY_SIZE) % NAV_POSITION_HISTORY_SIZE;
    int newest = (AutoNavState.history_index - 1 + NAV_POSITION_HISTORY_SIZE) % NAV_POSITION_HISTORY_SIZE;
    int dx = AutoNavState.position_history[newest].x - AutoNavState.position_history[oldest].x;
    int dz = AutoNavState.position_history[newest].z - AutoNavState.position_history[oldest].z;
    int totalMovement = dx * dx + dz * dz;

    /* Oscillating if variance is high (lots of back-and-forth) but net movement is low */
    if ((varX + varZ) > 500000 && totalMovement < 1000000) {
        return 1;
    }

    return 0;
}

/* Detect if player has returned to a previous position (loop) */
extern "C" int PathFind_DetectLoop(int* loopSize)
{
    if (AutoNavState.history_count < 18) return 0;  /* Need 3+ seconds of history */

    int currentIdx = (AutoNavState.history_index - 1 + NAV_POSITION_HISTORY_SIZE) % NAV_POSITION_HISTORY_SIZE;
    POSITION_RECORD* current = &AutoNavState.position_history[currentIdx];

    /* Check positions from 3+ seconds ago */
    for (int i = 18; i < AutoNavState.history_count; i++) {
        int oldIdx = (AutoNavState.history_index - 1 - i + NAV_POSITION_HISTORY_SIZE) % NAV_POSITION_HISTORY_SIZE;
        POSITION_RECORD* old = &AutoNavState.position_history[oldIdx];

        int dx = current->x - old->x;
        int dz = current->z - old->z;
        int distSq = dx * dx + dz * dz;

        /* Within 1000 units = same position (loop detected) */
        if (distSq < 1000000) {
            if (loopSize) *loopSize = i;
            return 1;
        }
    }

    return 0;
}

/* Escalate to next navigation strategy */
extern "C" void PathFind_EscalateStrategy(void)
{
    AutoNavState.strategy_frames = 0;

    switch (AutoNavState.current_strategy) {
        case NAV_STRATEGY_DIRECT:
            AutoNavState.current_strategy = NAV_STRATEGY_WALL_FOLLOW_LEFT;
            TTS_SpeakQueued("Trying wall follow left.");
            LOG_INF("Strategy: Wall follow left");
            break;

        case NAV_STRATEGY_WALL_FOLLOW_LEFT:
            AutoNavState.current_strategy = NAV_STRATEGY_WALL_FOLLOW_RIGHT;
            TTS_SpeakQueued("Trying wall follow right.");
            LOG_INF("Strategy: Wall follow right");
            break;

        case NAV_STRATEGY_WALL_FOLLOW_RIGHT:
            AutoNavState.current_strategy = NAV_STRATEGY_BACKTRACK;
            TTS_SpeakQueued("Backing up.");
            LOG_INF("Strategy: Backtrack");
            break;

        case NAV_STRATEGY_BACKTRACK:
            /* After backtrack, try wide around */
            AutoNavState.current_strategy = NAV_STRATEGY_WIDE_AROUND_LEFT;
            TTS_SpeakQueued("Trying wide path left.");
            LOG_INF("Strategy: Wide around left");
            break;

        case NAV_STRATEGY_WIDE_AROUND_LEFT:
            AutoNavState.current_strategy = NAV_STRATEGY_WIDE_AROUND_RIGHT;
            TTS_SpeakQueued("Trying wide path right.");
            LOG_INF("Strategy: Wide around right");
            break;

        case NAV_STRATEGY_WIDE_AROUND_RIGHT:
            /* All strategies exhausted */
            AutoNavState.strategy_failures++;
            if (AutoNavState.strategy_failures >= 2) {
                TTS_Speak("Cannot reach target. Try manual navigation.");
                AutoNavState.auto_move = 0;
                AutoNavState.current_strategy = NAV_STRATEGY_DIRECT;
                LOG_WRN("All navigation strategies failed");
            } else {
                /* Reset and try again */
                AutoNavState.current_strategy = NAV_STRATEGY_DIRECT;
                TTS_SpeakQueued("Retrying direct path.");
                LOG_INF("Strategy: Reset to direct");
            }
            break;
    }
}

/* ============================================
 * Progress and Arrival Functions
 * ============================================ */

#define PROGRESS_ANNOUNCE_DISTANCE 5000   /* 5 meters */
#define PROGRESS_ANNOUNCE_TIME_MS 10000   /* 10 seconds */
#define PROGRESS_MOVING_AWAY_THRESHOLD 3000  /* 3 meters */
#define ARRIVAL_DISTANCE 2500  /* 2.5 meters */

extern "C" void AutoNav_CheckProgress(void)
{
    if (!AutoNavState.enabled || !AutoNavState.auto_move) return;

    int currentDist = AutoNavState.target_distance;
    int lastDist = AutoNavState.last_announced_distance;
    unsigned int now = GetTickCount();

    /* Update closest achieved */
    if (currentDist < AutoNavState.closest_achieved) {
        AutoNavState.closest_achieved = currentDist;
    }

    /* Check if making progress */
    int distanceChange = lastDist - currentDist;  /* Positive = getting closer */
    unsigned int timeSince = now - AutoNavState.last_progress_time;

    if (distanceChange >= PROGRESS_ANNOUNCE_DISTANCE) {
        /* Announce distance milestone */
        char msg[64];
        int meters = currentDist / 1000;
        if (meters > 0) {
            snprintf(msg, sizeof(msg), "%d meters.", meters);
            TTS_SpeakQueued(msg);
        }
        AutoNavState.last_announced_distance = currentDist;
        AutoNavState.last_progress_time = now;
        LOG_INF("Progress: %d meters to target", meters);
    }
    else if (distanceChange <= -PROGRESS_MOVING_AWAY_THRESHOLD && timeSince > 5000) {
        /* Moving away from target */
        TTS_SpeakQueued("Moving away from target.");
        AutoNavState.last_announced_distance = currentDist;
        AutoNavState.last_progress_time = now;
        LOG_INF("Moving away from target");
    }
    else if (timeSince > PROGRESS_ANNOUNCE_TIME_MS && abs(distanceChange) < 2000) {
        /* No significant progress in 10 seconds */
        TTS_SpeakQueued("Navigation stalled.");
        AutoNavState.last_progress_time = now;
        LOG_WRN("Navigation stalled");
    }
}

extern "C" void AutoNav_CheckArrival(void)
{
    if (!AutoNavState.enabled) return;
    if (AutoNavState.arrival_announced) return;
    if (AutoNavState.target_distance >= ARRIVAL_DISTANCE) return;

    /* Build contextual announcement */
    char msg[128];

    if (AutoNavState.target_type == NAV_TARGET_INTERACTIVE) {
        snprintf(msg, sizeof(msg), "Arrived at %s. Press SPACE to interact.",
                 AutoNavState.target_name ? AutoNavState.target_name : "target");
    }
    else if (AutoNavState.target_type == NAV_TARGET_ITEM) {
        snprintf(msg, sizeof(msg), "Item nearby. Walk forward to collect.");
    }
    else {
        snprintf(msg, sizeof(msg), "Target reached.");
    }

    TTS_Speak(msg);
    AutoNavState.arrival_announced = 1;
    AutoNavState.target_reached = 1;

    /* Disable auto-move on arrival */
    AutoNavState.auto_move = 0;

    LOG_INF("Arrived at target: %s", AutoNavState.target_name ? AutoNavState.target_name : "unknown");
}

/* Get name for target type */
static const char* GetTargetTypeName(NAV_TARGET_TYPE type)
{
    switch (type) {
        case NAV_TARGET_INTERACTIVE: return "interactive";
        case NAV_TARGET_NPC: return "enemy";
        case NAV_TARGET_EXIT: return "exit";
        case NAV_TARGET_ITEM: return "item";
        default: return "unknown";
    }
}

/* Check if a behavior type is a valid navigation target */
static int IsNavTarget(AVP_BEHAVIOUR_TYPE bhvr, NAV_TARGET_TYPE targetType)
{
    switch (targetType) {
        case NAV_TARGET_INTERACTIVE:
            return (bhvr == I_BehaviourBinarySwitch ||
                    bhvr == I_BehaviourLinkSwitch ||
                    bhvr == I_BehaviourDatabase ||
                    bhvr == I_BehaviourLift ||
                    bhvr == I_BehaviourProximityDoor ||
                    bhvr == I_BehaviourLiftDoor ||
                    bhvr == I_BehaviourSwitchDoor ||
                    bhvr == I_BehaviourGenerator);

        case NAV_TARGET_NPC:
            return (bhvr == I_BehaviourAlien ||
                    bhvr == I_BehaviourQueenAlien ||
                    bhvr == I_BehaviourFaceHugger ||
                    bhvr == I_BehaviourPredator ||
                    bhvr == I_BehaviourXenoborg ||
                    bhvr == I_BehaviourMarine ||
                    bhvr == I_BehaviourSeal);

        case NAV_TARGET_EXIT:
            return (bhvr == I_BehaviourProximityDoor ||
                    bhvr == I_BehaviourLiftDoor ||
                    bhvr == I_BehaviourSwitchDoor ||
                    bhvr == I_BehaviourLift);

        case NAV_TARGET_ITEM:
            return (bhvr == I_BehaviourInanimateObject);  /* Pickups */

        default:
            return 0;
    }
}

/* Get name for a specific target */
static const char* GetNavTargetName(AVP_BEHAVIOUR_TYPE bhvr)
{
    switch (bhvr) {
        case I_BehaviourBinarySwitch: return "switch";
        case I_BehaviourLinkSwitch: return "switch";
        case I_BehaviourDatabase: return "terminal";
        case I_BehaviourLift: return "lift";
        case I_BehaviourAlien: return "alien";
        case I_BehaviourQueenAlien: return "queen";
        case I_BehaviourFaceHugger: return "facehugger";
        case I_BehaviourPredator: return "predator";
        case I_BehaviourXenoborg: return "xenoborg";
        case I_BehaviourMarine: return "marine";
        case I_BehaviourSeal: return "marine";
        case I_BehaviourProximityDoor: return "door";
        case I_BehaviourLiftDoor: return "lift door";
        case I_BehaviourSwitchDoor: return "door";
        case I_BehaviourInanimateObject: return "item";
        default: return "target";
    }
}

extern "C" void AutoNav_FindTarget(void)
{
    if (!Player || !Player->ObStrategyBlock) {
        AutoNavState.target_name = NULL;
        return;
    }

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    if (!playerDyn) {
        AutoNavState.target_name = NULL;
        return;
    }

    int playerX = playerDyn->Position.vx;
    int playerY = playerDyn->Position.vy;
    int playerZ = playerDyn->Position.vz;

    extern int NumActiveStBlocks;
    extern STRATEGYBLOCK* ActiveStBlockList[];

    int bestScore = 999999999;
    int nearestDist = 999999999;
    STRATEGYBLOCK* nearestSB = NULL;

    for (int i = 0; i < NumActiveStBlocks; i++) {
        STRATEGYBLOCK* sb = ActiveStBlockList[i];
        if (!sb || !sb->DynPtr) continue;

        if (!IsNavTarget(sb->I_SBtype, AutoNavState.target_type)) continue;

        int dist = Accessibility_GetDistance(
            playerX, playerY, playerZ,
            sb->DynPtr->Position.vx,
            sb->DynPtr->Position.vy,
            sb->DynPtr->Position.vz
        );

        /* Score-based prioritization (lower is better):
         * - Threats get -5000 priority bonus
         * - Interactive elements get -2000 bonus
         * This makes closer threats/interactives preferred over distant ones */
        int score = dist;
        if (IsEntityThreat(sb->I_SBtype, AvP.PlayerType)) {
            score -= 5000;  /* Prioritize threats */
        }
        if (sb->I_SBtype == I_BehaviourBinarySwitch ||
            sb->I_SBtype == I_BehaviourLinkSwitch ||
            sb->I_SBtype == I_BehaviourDatabase ||
            sb->I_SBtype == I_BehaviourGenerator) {
            score -= 2000;  /* Prioritize mission-relevant interactives */
        }
        if (sb->I_SBtype == I_BehaviourProximityDoor ||
            sb->I_SBtype == I_BehaviourLiftDoor ||
            sb->I_SBtype == I_BehaviourSwitchDoor) {
            score -= 1500;  /* Prioritize doors slightly less than switches */
        }

        if (score < bestScore) {
            bestScore = score;
            nearestDist = dist;
            nearestSB = sb;
        }
    }

    if (nearestSB && nearestSB->DynPtr) {
        AutoNavState.target_x = nearestSB->DynPtr->Position.vx;
        AutoNavState.target_y = nearestSB->DynPtr->Position.vy;
        AutoNavState.target_z = nearestSB->DynPtr->Position.vz;
        AutoNavState.target_distance = nearestDist;
        AutoNavState.target_name = GetNavTargetName(nearestSB->I_SBtype);
        LOG_DBG("AutoNav: Found target '%s' at dist=%d pos=(%d,%d,%d)",
                AutoNavState.target_name, nearestDist,
                AutoNavState.target_x, AutoNavState.target_y, AutoNavState.target_z);
    } else {
        AutoNavState.target_name = NULL;
        AutoNavState.target_distance = 0;
        LOG_DBG("AutoNav: No target found for type %d", AutoNavState.target_type);
    }
}

extern "C" void AutoNav_AnnounceTarget(void)
{
    if (!AutoNavState.target_name) {
        char msg[128];
        snprintf(msg, sizeof(msg), "No %s found nearby.", GetTargetTypeName(AutoNavState.target_type));
        TTS_Speak(msg);
        return;
    }

    if (!Player || !Player->ObStrategyBlock || !Player->ObStrategyBlock->DynPtr) {
        return;
    }

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    int playerX = playerDyn->Position.vx;
    int playerY = playerDyn->Position.vy;
    int playerZ = playerDyn->Position.vz;

    /* Get player facing direction */
    int playerYaw = 0;
    extern VIEWDESCRIPTORBLOCK* Global_VDB_Ptr;
    if (Global_VDB_Ptr) {
        playerYaw = (int)(atan2((double)Global_VDB_Ptr->VDB_Mat.mat13,
                                (double)Global_VDB_Ptr->VDB_Mat.mat33) * 2048.0 / 3.14159265);
    }

    AUDIO_DIRECTION dir = Accessibility_GetDirection(
        playerX, playerY, playerZ,
        AutoNavState.target_x, AutoNavState.target_y, AutoNavState.target_z,
        playerYaw
    );

    const char* dirName = AudioRadar_GetDirectionName(dir);
    const char* distName = Accessibility_FormatDistance(AutoNavState.target_distance);

    char announcement[256];
    snprintf(announcement, sizeof(announcement), "Navigating to %s. %s, %s.",
             AutoNavState.target_name, dirName, distName);
    TTS_Speak(announcement);
}

extern "C" void AutoNav_Toggle(void)
{
    AutoNavState.enabled = !AutoNavState.enabled;

    if (AutoNavState.enabled) {
        AutoNav_FindTarget();
        if (AutoNavState.target_name) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Auto-navigation enabled. Targeting %s.",
                     GetTargetTypeName(AutoNavState.target_type));
            TTS_Speak(msg);
            AutoNav_AnnounceTarget();
        } else {
            TTS_Speak("Auto-navigation enabled. No target found.");
        }
    } else {
        TTS_Speak("Auto-navigation disabled.");
    }
}

extern "C" void AutoNav_ToggleRotation(void)
{
    AutoNavState.auto_rotate = !AutoNavState.auto_rotate;
    TTS_Speak(AutoNavState.auto_rotate ?
              "Auto-rotation enabled." : "Auto-rotation disabled.");
}

extern "C" void AutoNav_ToggleMovement(void)
{
    AutoNavState.auto_move = !AutoNavState.auto_move;
    TTS_Speak(AutoNavState.auto_move ?
              "Auto-movement enabled." : "Auto-movement disabled.");
}

extern "C" void AutoNav_CycleTargetType(void)
{
    /* Cycle through target types */
    switch (AutoNavState.target_type) {
        case NAV_TARGET_INTERACTIVE:
            AutoNavState.target_type = NAV_TARGET_NPC;
            break;
        case NAV_TARGET_NPC:
            AutoNavState.target_type = NAV_TARGET_EXIT;
            break;
        case NAV_TARGET_EXIT:
            AutoNavState.target_type = NAV_TARGET_ITEM;
            break;
        case NAV_TARGET_ITEM:
        default:
            AutoNavState.target_type = NAV_TARGET_INTERACTIVE;
            break;
    }

    AutoNav_FindTarget();

    char msg[128];
    snprintf(msg, sizeof(msg), "Now targeting: %s.", GetTargetTypeName(AutoNavState.target_type));
    TTS_Speak(msg);

    if (AutoNavState.target_name) {
        AutoNav_AnnounceTarget();
    }
}

/* Forward declarations for structure identification system (defined later) */
typedef struct {
    int distance;           /* Distance to hit (0 if no hit) */
    DISPLAYBLOCK* hitObj;   /* Object that was hit (NULL for world geometry) */
    const char* typeName;   /* Friendly name for what was hit */
} RAY_RESULT;

static RAY_RESULT CastObstructionRayEx(VECTORCH* origin, VECTORCH* direction, int maxRange);
static const char* GetObstacleTypeName(DISPLAYBLOCK* obj);

extern "C" void AutoNav_Update(void)
{
    if (!AutoNavState.enabled || !Accessibility_IsAvailable()) {
        return;
    }

    if (!Player || !Player->ObStrategyBlock || !Player->ObStrategyBlock->DynPtr) {
        return;
    }

    /* Refresh target periodically */
    g_NavToneFrameCounter++;
    if (g_NavToneFrameCounter >= 30) {  /* Every ~0.5 seconds */
        g_NavToneFrameCounter = 0;
        AutoNav_FindTarget();
    }

    if (!AutoNavState.target_name) {
        return;
    }

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    int playerX = playerDyn->Position.vx;
    int playerY = playerDyn->Position.vy;
    int playerZ = playerDyn->Position.vz;

    /* Record position every 10 frames for loop detection */
    static int positionRecordCounter = 0;
    positionRecordCounter++;
    if (positionRecordCounter >= 10) {
        positionRecordCounter = 0;
        PathFind_RecordPosition(playerX, playerY, playerZ);
    }

    /* Increment strategy frame counter */
    AutoNavState.strategy_frames++;

    /* Check progress toward target (every 60 frames) */
    static int progressCheckCounter = 0;
    progressCheckCounter++;
    if (progressCheckCounter >= 60) {
        progressCheckCounter = 0;
        AutoNav_CheckProgress();
    }

    /* Calculate direction to target (including vertical) */
    float dx = (float)(AutoNavState.target_x - playerX);
    float dy = (float)(AutoNavState.target_y - playerY);
    float dz = (float)(AutoNavState.target_z - playerZ);

    /* Get player's forward direction from orientation matrix */
    float forwardX = (float)playerDyn->OrientMat.mat31 / 65536.0f;
    float forwardZ = (float)playerDyn->OrientMat.mat33 / 65536.0f;

    /* Normalize direction to target (horizontal) */
    float targetDist = sqrtf(dx*dx + dz*dz);
    if (targetDist < 1.0f) targetDist = 1.0f;
    float targetDirX = dx / targetDist;
    float targetDirZ = dz / targetDist;

    /* Calculate 3D distance for vertical ratio */
    float dist3D = sqrtf(dx*dx + dy*dy + dz*dz);
    float verticalRatio = 0.0f;
    if (dist3D > 1.0f) {
        verticalRatio = dy / dist3D;  /* -1 to 1: negative = below, positive = above */
        if (verticalRatio > 1.0f) verticalRatio = 1.0f;
        if (verticalRatio < -1.0f) verticalRatio = -1.0f;
    }

    /* Calculate cross product to determine left/right */
    float cross = forwardX * targetDirZ - forwardZ * targetDirX;

    /* Calculate dot product to determine if in front or behind */
    float dot = forwardX * targetDirX + forwardZ * targetDirZ;

    /* Convert to angle offset (-1 to 1 range for stereo) */
    float angleOffset = cross;
    if (angleOffset > 1.0f) angleOffset = 1.0f;
    if (angleOffset < -1.0f) angleOffset = -1.0f;

    /* If target is behind, indicate strongly to one side */
    if (dot < 0) {
        angleOffset = (cross >= 0) ? 1.0f : -1.0f;
    }

    /* Play navigation tone every 20 frames with vertical pitch variation */
    static int toneCounter = 0;
    toneCounter++;
    if (toneCounter >= 20) {
        toneCounter = 0;
        NavTone_PlayDirectional(angleOffset, verticalRatio);
    }

    /* ============================================
     * Obstacle Avoidance for Autonavigation
     * ============================================ */

    /* Cast a ray toward target to check for obstacles */
    static int avoidanceState = 0;  /* 0=none, 1=avoiding left, 2=avoiding right, 3=waiting at door */
    static int avoidanceTimer = 0;
    static int stuckCounter = 0;
    static int lastPlayerX = 0, lastPlayerZ = 0;
    static int doorAnnouncedThisStop = 0;

    VECTORCH rayOrigin = playerDyn->Position;
    rayOrigin.vy -= 800;  /* Chest height */

    VECTORCH targetDir;
    targetDir.vx = (int)(targetDirX * ONE_FIXED);
    targetDir.vy = 0;
    targetDir.vz = (int)(targetDirZ * ONE_FIXED);

    /* Use extended ray cast for structure identification */
    RAY_RESULT obstacleResult = CastObstructionRayEx(&rayOrigin, &targetDir, 8000);
    int obstacleDistance = obstacleResult.distance;
    const char* obstacleType = obstacleResult.typeName;

    /* Check if obstacle is a door - STOP and announce */
    int isDoor = (strcmp(obstacleType, "door") == 0 ||
                  strcmp(obstacleType, "proximity door") == 0 ||
                  strcmp(obstacleType, "lift door") == 0);

    int isEnemy = (strcmp(obstacleType, "alien") == 0 ||
                   strcmp(obstacleType, "queen alien") == 0 ||
                   strcmp(obstacleType, "facehugger") == 0 ||
                   strcmp(obstacleType, "predator") == 0 ||
                   strcmp(obstacleType, "xenoborg") == 0 ||
                   strcmp(obstacleType, "marine") == 0);

    /* Enhanced stuck detection using position history */
    int movedX = playerX - lastPlayerX;
    int movedZ = playerZ - lastPlayerZ;
    int distMoved = (movedX * movedX + movedZ * movedZ);

    if (AutoNavState.auto_move && !AutoNavState.target_reached) {
        /* Check for oscillation (back-and-forth movement) */
        int loopSize = 0;
        int isOscillating = PathFind_DetectOscillation();
        int isLooping = PathFind_DetectLoop(&loopSize);

        /* Simple stuck check (barely moving) */
        if (distMoved < 10000) {
            stuckCounter++;
        } else {
            stuckCounter = 0;
        }

        /* Escalate strategy if stuck, oscillating, or looping */
        if (stuckCounter > 90) {  /* Stuck for ~1.5 seconds */
            LOG_INF("AutoNav: Stuck detected (stuckCounter=%d), escalating strategy", stuckCounter);
            PathFind_EscalateStrategy();
            stuckCounter = 0;

            /* Apply strategy-specific behavior to avoidance state */
            switch (AutoNavState.current_strategy) {
                case NAV_STRATEGY_WALL_FOLLOW_LEFT:
                    avoidanceState = 1;  /* Force left */
                    avoidanceTimer = 180;  /* 3 seconds */
                    break;
                case NAV_STRATEGY_WALL_FOLLOW_RIGHT:
                    avoidanceState = 2;  /* Force right */
                    avoidanceTimer = 180;
                    break;
                case NAV_STRATEGY_BACKTRACK:
                    avoidanceState = 0;  /* Will be handled separately */
                    break;
                case NAV_STRATEGY_WIDE_AROUND_LEFT:
                    avoidanceState = 1;
                    avoidanceTimer = 300;  /* 5 seconds */
                    break;
                case NAV_STRATEGY_WIDE_AROUND_RIGHT:
                    avoidanceState = 2;
                    avoidanceTimer = 300;
                    break;
                default:
                    break;
            }
        } else if (isOscillating) {
            LOG_INF("AutoNav: Oscillation detected, escalating strategy");
            PathFind_EscalateStrategy();
        } else if (isLooping && loopSize > 0) {
            LOG_INF("AutoNav: Loop detected (size=%d), escalating strategy", loopSize);
            PathFind_EscalateStrategy();
        }
    } else {
        stuckCounter = 0;
    }
    lastPlayerX = playerX;
    lastPlayerZ = playerZ;

    /* Determine movement behavior */
    int shouldMoveForward = 0;
    int shouldStrafeLeft = 0;
    int shouldStrafeRight = 0;
    int adjustedTurnAmount = 0;

    /* Handle doors specially - STOP and announce */
    if (isDoor && obstacleDistance > 0 && obstacleDistance < 3000) {
        /* Door ahead - stop and announce */
        avoidanceState = 3;  /* Waiting at door */
        shouldMoveForward = 0;

        if (!doorAnnouncedThisStop) {
            char doorMsg[128];
            snprintf(doorMsg, sizeof(doorMsg), "%s ahead. Press SPACE to open.",
                    obstacleType);
            if (doorMsg[0] >= 'a' && doorMsg[0] <= 'z') doorMsg[0] -= 32;
            TTS_SpeakQueued(doorMsg);
            LOG_INF("AutoNav: Stopped at %s (dist=%d)", obstacleType, obstacleDistance);
            doorAnnouncedThisStop = 1;
        }
    } else if (obstacleDistance > 0 && obstacleDistance < 4000) {
        /* Reset door announcement flag when not near a door */
        doorAnnouncedThisStop = 0;

        /* Obstacle in the way - need to avoid */
        /* Enemies get tighter avoidance threshold */
        int avoidThreshold = isEnemy ? 5000 : 4000;

        if (avoidanceState == 0 || avoidanceState == 3) {  /* Not currently avoiding, or was waiting at door */
            /* Start avoidance - check which way has more room */
            VECTORCH leftDir, rightDir;
            leftDir.vx = -playerDyn->OrientMat.mat11;
            leftDir.vy = 0;
            leftDir.vz = -playerDyn->OrientMat.mat13;
            rightDir.vx = playerDyn->OrientMat.mat11;
            rightDir.vy = 0;
            rightDir.vz = playerDyn->OrientMat.mat13;

            LOS_Lambda = 8000;
            FindPolygonInLineOfSight(&leftDir, &rayOrigin, 0, Player);
            int leftClear = LOS_Lambda;

            LOS_Lambda = 8000;
            FindPolygonInLineOfSight(&rightDir, &rayOrigin, 0, Player);
            int rightClear = LOS_Lambda;

            /* Also consider which direction is closer to target */
            if (cross > 0.1f) {
                /* Target is to the right, prefer going right if clear */
                avoidanceState = (rightClear > 2000) ? 2 : 1;
            } else if (cross < -0.1f) {
                /* Target is to the left, prefer going left if clear */
                avoidanceState = (leftClear > 2000) ? 1 : 2;
            } else {
                /* Go toward the side with more room */
                avoidanceState = (leftClear > rightClear) ? 1 : 2;
            }
            avoidanceTimer = 30;  /* Avoid for ~0.5 seconds */
            LOG_DBG("AutoNav: Obstacle at %d, avoiding %s (L=%d R=%d)",
                    obstacleDistance, avoidanceState == 1 ? "LEFT" : "RIGHT", leftClear, rightClear);
        }

        /* Execute avoidance */
        if (avoidanceState == 1) {
            /* Turn and strafe left */
            adjustedTurnAmount = -30;
            shouldStrafeLeft = 1;
            shouldMoveForward = 1;
        } else if (avoidanceState == 2) {
            /* Turn and strafe right */
            adjustedTurnAmount = 30;
            shouldStrafeRight = 1;
            shouldMoveForward = 1;
        }
    }

    /* Decrement avoidance timer outside obstacle condition - so it always counts down */
    if (avoidanceTimer > 0) {
        avoidanceTimer--;
        if (avoidanceTimer <= 0) {
            avoidanceState = 0;  /* End avoidance, reassess */
        }
    } else if (obstacleDistance == 0 || obstacleDistance >= 4000) {
        /* No obstacle - clear avoidance state */
        avoidanceState = 0;
        doorAnnouncedThisStop = 0;  /* Reset door announcement flag */

        /* Auto-rotation: gradually turn toward target */
        if (AutoNavState.auto_rotate && targetDist > 2000) {
            adjustedTurnAmount = (int)(cross * 100.0f);
            if (adjustedTurnAmount > 50) adjustedTurnAmount = 50;
            if (adjustedTurnAmount < -50) adjustedTurnAmount = -50;
        }

        /* Auto-movement: move forward when facing target */
        if (dot > 0.5f) {
            shouldMoveForward = 1;
        } else if (dot < -0.3f) {
            /* Target is behind - turn faster */
            adjustedTurnAmount = (cross >= 0) ? 70 : -70;
        }
    }

    /* Apply rotation with smoothing (lerp toward target turn rate) */
    if (AutoNavState.auto_rotate) {
        static int smoothedTurnAmount = 0;
        /* Lerp factor: 0.15 = smooth, responsive turning */
        int diff = adjustedTurnAmount - smoothedTurnAmount;
        smoothedTurnAmount += (int)(diff * 0.15f);
        playerDyn->AngVelocity.EulerY = smoothedTurnAmount;
    }

    /* Apply movement - use SLOWER speed for better control during autonavigation */
    /* Normal speed is 32768, we use ~40% speed (13000) for more precise navigation */
    #define AUTONAV_FORWARD_SPEED 13000   /* Slower forward movement */
    #define AUTONAV_STRAFE_SPEED 8000     /* Slower strafe movement */
    #define AUTONAV_BACKTRACK_SPEED -8000 /* Backward movement for backtrack strategy */
    #define BACKTRACK_DURATION_FRAMES 90  /* ~1.5 seconds of backing up */

    /* Handle BACKTRACK strategy specially - move backward */
    static int backtrackFrames = 0;
    if (AutoNavState.current_strategy == NAV_STRATEGY_BACKTRACK) {
        backtrackFrames++;
        PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
        if (ps) {
            ps->Mvt_MotionIncrement = AUTONAV_BACKTRACK_SPEED;  /* Move backward */
            ps->Mvt_SideStepIncrement = 0;
        }
        /* After backing up enough, reset to direct strategy */
        if (backtrackFrames > BACKTRACK_DURATION_FRAMES) {
            AutoNavState.current_strategy = NAV_STRATEGY_DIRECT;
            AutoNavState.strategy_frames = 0;
            backtrackFrames = 0;
            LOG_INF("AutoNav: Backtrack complete, returning to direct strategy");
        }
    } else if (AutoNavState.auto_move && targetDist > 3000) {
        backtrackFrames = 0;  /* Reset backtrack counter */
        PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
        if (ps) {
            if (shouldMoveForward) {
                ps->Mvt_MotionIncrement = AUTONAV_FORWARD_SPEED;
            } else {
                ps->Mvt_MotionIncrement = 0;  /* Stop forward movement */
            }
            if (shouldStrafeLeft) {
                ps->Mvt_SideStepIncrement = -AUTONAV_STRAFE_SPEED;
            } else if (shouldStrafeRight) {
                ps->Mvt_SideStepIncrement = AUTONAV_STRAFE_SPEED;
            } else {
                ps->Mvt_SideStepIncrement = 0;  /* Stop strafing */
            }

            /* Auto-jump: If obstruction detection found a jumpable obstacle ahead, jump! */
            if (g_ObstructionState.forward_blocked &&
                g_ObstructionState.forward_distance < 2500 &&  /* Within 2.5m */
                g_ObstructionState.forward_is_clearable &&     /* Can be jumped */
                !g_ObstructionState.forward_is_jumpable) {     /* Not a step (needs actual jump) */
                /* Trigger jump by setting the jump request flag */
                ps->Mvt_InputRequests.Flags.Rqst_Jump = 1;
            }
        }
    } else if (AutoNavState.auto_move) {
        /* Near target - stop all auto-movement */
        PLAYER_STATUS* ps = (PLAYER_STATUS*)(Player->ObStrategyBlock->SBdataptr);
        if (ps) {
            ps->Mvt_MotionIncrement = 0;
            ps->Mvt_SideStepIncrement = 0;
        }
    }

    /* Check arrival using the enhanced arrival detection */
    AutoNav_CheckArrival();
}

/* ============================================
 * Obstruction Detection System
 * ============================================ */

/* Constants for obstruction detection */
#define OBSTRUCTION_CHECK_INTERVAL 10    /* Check every N frames */
#define OBSTRUCTION_CLOSE_DIST 1500      /* 1.5m - very close warning */
#define OBSTRUCTION_NEAR_DIST 3000       /* 3m - near warning */
#define OBSTRUCTION_FAR_DIST 6000        /* 6m - far warning */
#define MAXIMUM_STEP_HEIGHT 450          /* From dynamics.h - auto-climb threshold */
#define JUMP_CLEARANCE_HEIGHT 1200       /* Approximate jump height */

/* Note: OBSTRUCTION_STATE struct and g_ObstructionState are defined earlier in the file
 * near the other global state variables, so AutoNav can access them */

/* ============================================
 * Structure Identification System
 * ============================================ */

/* Get a friendly name for an obstacle based on its behavior type or geometry */
static const char* GetObstacleTypeName(DISPLAYBLOCK* obj)
{
    if (!obj) return "wall";

    /* If the object has a strategy block, identify by behavior type */
    if (obj->ObStrategyBlock) {
        AVP_BEHAVIOUR_TYPE bhvr = obj->ObStrategyBlock->I_SBtype;

        switch (bhvr) {
            /* Enemies */
            case I_BehaviourAlien:
            case I_BehaviourPredatorAlien:
                return "alien";
            case I_BehaviourQueenAlien:
                return "queen alien";
            case I_BehaviourFaceHugger:
                return "facehugger";
            case I_BehaviourPredator:
            case I_BehaviourDormantPredator:
                return "predator";
            case I_BehaviourXenoborg:
                return "xenoborg";
            case I_BehaviourMarine:
            case I_BehaviourSeal:
                return "marine";
            case I_BehaviourAutoGun:
                return "autogun";

            /* Doors */
            case I_BehaviourProximityDoor:
                return "proximity door";
            case I_BehaviourLiftDoor:
                return "lift door";
            case I_BehaviourSwitchDoor:
                return "door";

            /* Interactive */
            case I_BehaviourBinarySwitch:
            case I_BehaviourLinkSwitch:
                return "switch";
            case I_BehaviourLift:
            case I_BehaviourPlatform:
                return "lift";
            case I_BehaviourGenerator:
                return "generator";
            case I_BehaviourDatabase:
                return "terminal";
            case I_BehaviourPowerCable:
                return "power cable";
            case I_BehaviourFan:
                return "fan";
            case I_BehaviourDeathVolume:
                return "hazard";
            case I_BehaviourSelfDestruct:
                return "self-destruct console";

            /* Projectiles (shouldn't hit these usually) */
            case I_BehaviourGrenade:
            case I_BehaviourPulseGrenade:
            case I_BehaviourFlareGrenade:
            case I_BehaviourFragmentationGrenade:
            case I_BehaviourProximityGrenade:
            case I_BehaviourClusterGrenade:
                return "grenade";
            case I_BehaviourRocket:
                return "rocket";

            /* Objects */
            case I_BehaviourInanimateObject:
                return "object";
            case I_BehaviourFragment:
            case I_BehaviourHierarchicalFragment:
            case I_BehaviourAlienFragment:
                return "debris";
            case I_BehaviourNetCorpse:
                return "corpse";

            /* Placed items */
            case I_BehaviourPlacedHierarchy:
            case I_BehaviourPlacedLight:
                return "structure";
            case I_BehaviourVideoScreen:
                return "screen";
            case I_BehaviourTrackObject:
                return "track";

            default:
                break;
        }
    }

    /* No strategy block or unknown type - analyze geometry if we have shape data */
    if (obj->ObShapeData) {
        /* Get shape extents */
        int minX = obj->ObShapeData->shaperadius;  /* Use radius as approximation */
        int maxExtent = minX * 2;  /* Diameter */

        /* Very rough classification by size */
        if (maxExtent < 1000) {
            return "small object";
        } else if (maxExtent < 3000) {
            return "crate";
        } else if (maxExtent < 6000) {
            return "pillar";
        }
    }

    /* Default to wall for static geometry */
    return "wall";
}

/* Note: RAY_RESULT typedef is declared earlier (before AutoNav_Update) for forward reference */

/* Cast a ray in a direction and return distance to hit (0 if no hit) */
static int CastObstructionRay(VECTORCH* origin, VECTORCH* direction, int maxRange)
{
    /* Initialize LOS globals */
    LOS_ObjectHitPtr = NULL;
    LOS_Lambda = maxRange;

    /* Cast the ray */
    FindPolygonInLineOfSight(direction, origin, 0, Player);

    if (LOS_ObjectHitPtr != NULL || LOS_Lambda < maxRange) {
        LOG_DBG("Ray hit at distance %d (obj=%p)", LOS_Lambda, (void*)LOS_ObjectHitPtr);
        return LOS_Lambda;
    }
    return 0;  /* No obstruction within range */
}

/* Cast a ray and return detailed result including what was hit */
static RAY_RESULT CastObstructionRayEx(VECTORCH* origin, VECTORCH* direction, int maxRange)
{
    RAY_RESULT result = {0, NULL, "clear"};

    /* Initialize LOS globals */
    LOS_ObjectHitPtr = NULL;
    LOS_Lambda = maxRange;

    /* Cast the ray */
    FindPolygonInLineOfSight(direction, origin, 0, Player);

    if (LOS_ObjectHitPtr != NULL || LOS_Lambda < maxRange) {
        result.distance = LOS_Lambda;
        result.hitObj = LOS_ObjectHitPtr;
        result.typeName = GetObstacleTypeName(LOS_ObjectHitPtr);
        LOG_DBG("RayEx hit '%s' at distance %d", result.typeName, result.distance);
    }

    return result;
}

/* Analyze an obstruction to determine if it's jumpable */
static void AnalyzeObstruction(VECTORCH* playerPos, VECTORCH* hitPoint,
                               int* isJumpable, int* isClearable)
{
    *isJumpable = 0;
    *isClearable = 0;

    /* The hit point Y compared to player Y tells us the height difference */
    /* In AVP, Y is typically up (negative is up in some cases) */
    int heightDiff = playerPos->vy - hitPoint->vy;

    /* If the obstruction is below player's waist level, it's potentially steppable */
    if (heightDiff > -MAXIMUM_STEP_HEIGHT && heightDiff < MAXIMUM_STEP_HEIGHT) {
        *isJumpable = 1;
    }

    /* If it's below jump height, it might be clearable with a jump */
    if (heightDiff > -JUMP_CLEARANCE_HEIGHT) {
        *isClearable = 1;
    }
}

/* Get distance description */
static const char* GetDistanceDescription(int distance)
{
    if (distance < OBSTRUCTION_CLOSE_DIST) return "very close";
    if (distance < OBSTRUCTION_NEAR_DIST) return "close";
    if (distance < OBSTRUCTION_FAR_DIST) return "ahead";
    return "far";
}

/* Main obstruction update - call each frame */
extern "C" void Obstruction_Update(void)
{
    if (!g_ObstructionState.enabled || !Accessibility_IsAvailable()) return;
    if (!Player || !Player->ObStrategyBlock || !Player->ObStrategyBlock->DynPtr) return;

    /* Only check every N frames for performance */
    static int frameCounter = 0;
    frameCounter++;
    if (frameCounter < OBSTRUCTION_CHECK_INTERVAL) return;
    frameCounter = 0;

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    VECTORCH playerPos = playerDyn->Position;

    /* Raise the ray origin to chest height */
    playerPos.vy -= 800;  /* Roughly chest height */

    /* Get player's orientation vectors */
    VECTORCH forward, left, up;

    /* Forward direction from orientation matrix (mat31, mat32, mat33 is forward) */
    forward.vx = playerDyn->OrientMat.mat31;
    forward.vy = playerDyn->OrientMat.mat32;
    forward.vz = playerDyn->OrientMat.mat33;
    Normalise(&forward);

    /* Left direction (mat11, mat12, mat13) */
    left.vx = -playerDyn->OrientMat.mat11;
    left.vy = -playerDyn->OrientMat.mat12;
    left.vz = -playerDyn->OrientMat.mat13;
    Normalise(&left);

    /* Up direction */
    up.vx = 0;
    up.vy = -ONE_FIXED;  /* Up in AVP coordinate system */
    up.vz = 0;

    /* Cast rays in each direction */
    int maxRange = OBSTRUCTION_FAR_DIST * 2;

    /* Forward ray */
    int forwardDist = CastObstructionRay(&playerPos, &forward, maxRange);
    if (forwardDist > 0) {
        g_ObstructionState.forward_blocked = 1;
        g_ObstructionState.forward_distance = forwardDist;

        /* Analyze if jumpable */
        AnalyzeObstruction(&playerPos, &LOS_Point,
                          &g_ObstructionState.forward_is_jumpable,
                          &g_ObstructionState.forward_is_clearable);
    } else {
        g_ObstructionState.forward_blocked = 0;
        g_ObstructionState.forward_distance = 0;
    }

    /* Left ray */
    g_ObstructionState.left_distance = CastObstructionRay(&playerPos, &left, maxRange);

    /* Right ray */
    VECTORCH right;
    right.vx = -left.vx;
    right.vy = -left.vy;
    right.vz = -left.vz;
    g_ObstructionState.right_distance = CastObstructionRay(&playerPos, &right, maxRange);

    /* Automatic alerts for very close obstructions */
    if (AccessibilitySettings.navigation_cues_enabled) {
        static unsigned int lastAutoAlertTime = 0;
        static char lastAutoAlertType[32] = {0};
        static int debounceCounter = 0;  /* Require consistent detection before announcing */
        static char pendingAlertType[32] = {0};  /* Type waiting to be announced */
        unsigned int currentTime = GetTickCount();

        /* Forward wall alert - with debouncing and time-based cooldown to prevent spam */
        if (g_ObstructionState.forward_blocked &&
            g_ObstructionState.forward_distance < OBSTRUCTION_CLOSE_DIST) {

            const char* alertType = NULL;
            if (g_ObstructionState.forward_is_jumpable) {
                alertType = "step";
            } else if (g_ObstructionState.forward_is_clearable) {
                alertType = "obstacle";
            } else {
                alertType = "wall";
            }

            /* Debouncing: require 3 consecutive frames with same alert type before announcing */
            if (strcmp(alertType, pendingAlertType) == 0) {
                debounceCounter++;
            } else {
                /* New alert type detected - reset debounce */
                strncpy(pendingAlertType, alertType, sizeof(pendingAlertType) - 1);
                debounceCounter = 1;
            }

            /* Only announce after debounce threshold AND if type changed OR enough time passed (3 seconds)
             * AND if priority system allows it (not during damage cooldown) */
            if (debounceCounter >= 3 &&
                Announcement_IsAllowed(ANNOUNCE_PRIORITY_HIGH) &&
                (strcmp(alertType, lastAutoAlertType) != 0 ||
                 (currentTime - lastAutoAlertTime) > 3000)) {

                if (g_ObstructionState.forward_is_jumpable) {
                    TTS_SpeakQueued("Step ahead.");
                } else if (g_ObstructionState.forward_is_clearable) {
                    TTS_SpeakQueued("Low obstacle. Jump.");
                } else {
                    TTS_SpeakQueued("Wall ahead.");
                }

                Announcement_RecordTime(ANNOUNCE_PRIORITY_HIGH);
                strncpy(lastAutoAlertType, alertType, sizeof(lastAutoAlertType) - 1);
                lastAutoAlertTime = currentTime;
            }
            g_ObstructionState.last_announced_forward = g_ObstructionState.forward_distance;
        } else {
            /* Not close to obstruction - reset debounce counter */
            debounceCounter = 0;
            pendingAlertType[0] = '\0';
        }

        /* Only clear last announced type when player has moved very far away (>5m)
         * This prevents spam when walking along a wall or weaving near obstacles */
        if (!g_ObstructionState.forward_blocked ||
            g_ObstructionState.forward_distance > 5000) {
            g_ObstructionState.last_announced_forward = 0;
            /* Don't clear lastAutoAlertType immediately - let time-based cooldown handle it */
            /* This prevents re-announcement when briefly moving away then back */
        }
    }
}

/* Check if a type is an interactive object that can be operated */
static int IsInteractiveType(const char* typeName)
{
    return (strcmp(typeName, "door") == 0 ||
            strcmp(typeName, "proximity door") == 0 ||
            strcmp(typeName, "lift door") == 0 ||
            strcmp(typeName, "switch") == 0 ||
            strcmp(typeName, "lift") == 0 ||
            strcmp(typeName, "terminal") == 0 ||
            strcmp(typeName, "generator") == 0 ||
            strcmp(typeName, "self-destruct console") == 0);
}

/* Get navigation guidance string based on left/right clearance */
static const char* GetNavigationGuidance(VECTORCH* playerPos, DYNAMICSBLOCK* playerDyn, const char* typeName)
{
    /* Cast rays left and right to check clearance */
    VECTORCH left, right;
    left.vx = -playerDyn->OrientMat.mat11;
    left.vy = 0;
    left.vz = -playerDyn->OrientMat.mat13;
    Normalise(&left);

    right.vx = playerDyn->OrientMat.mat11;
    right.vy = 0;
    right.vz = playerDyn->OrientMat.mat13;
    Normalise(&right);

    int maxRange = 8000;  /* 8 meters */
    int leftClear = CastObstructionRay(playerPos, &left, maxRange);
    int rightClear = CastObstructionRay(playerPos, &right, maxRange);

    /* Convert 0 (no hit) to max range for comparison */
    if (leftClear == 0) leftClear = maxRange;
    if (rightClear == 0) rightClear = maxRange;

    LOG_DBG("Navigation guidance: L=%d R=%d", leftClear, rightClear);

    /* Interactive objects have special guidance */
    if (IsInteractiveType(typeName)) {
        if (strcmp(typeName, "door") == 0 ||
            strcmp(typeName, "proximity door") == 0 ||
            strcmp(typeName, "lift door") == 0 ||
            strcmp(typeName, "switch") == 0) {
            return "Press SPACE to operate.";
        }
        if (strcmp(typeName, "lift") == 0) {
            return "Step on to ride.";
        }
        if (strcmp(typeName, "terminal") == 0 ||
            strcmp(typeName, "generator") == 0) {
            return "Press SPACE to interact.";
        }
    }

    /* For non-interactive obstacles, suggest direction */
    int threshold = 2000;  /* Need at least 2m clearance to suggest */

    if (leftClear > threshold && rightClear > threshold) {
        /* Both sides clear - suggest the one with more room */
        if (leftClear > rightClear + 1000) {
            return "Go left to continue.";
        } else if (rightClear > leftClear + 1000) {
            return "Go right to continue.";
        } else {
            return "Clear paths left and right.";
        }
    } else if (leftClear > threshold) {
        return "Go left to continue.";
    } else if (rightClear > threshold) {
        return "Go right to continue.";
    }

    /* Both sides blocked */
    return "Path blocked. Try turning around.";
}

/* Announce current obstruction status (on-demand via hotkey) with navigation guidance */
extern "C" void Obstruction_AnnounceAhead(void)
{
    if (!Accessibility_IsAvailable()) {
        TTS_Speak("Obstruction detection unavailable.");
        return;
    }

    if (!Player || !Player->ObStrategyBlock || !Player->ObStrategyBlock->DynPtr) {
        TTS_Speak("Cannot detect: player unavailable.");
        return;
    }

    /* Force an immediate check */
    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    VECTORCH playerPos = playerDyn->Position;
    playerPos.vy -= 800;

    VECTORCH forward;
    forward.vx = playerDyn->OrientMat.mat31;
    forward.vy = playerDyn->OrientMat.mat32;
    forward.vz = playerDyn->OrientMat.mat33;
    Normalise(&forward);

    /* Use extended ray cast to get type information */
    RAY_RESULT result = CastObstructionRayEx(&playerPos, &forward, OBSTRUCTION_FAR_DIST * 2);

    char announcement[384];  /* Larger buffer for guidance text */

    if (result.distance > 0) {
        int isJumpable, isClearable;
        AnalyzeObstruction(&playerPos, &LOS_Point, &isJumpable, &isClearable);

        const char* distDesc = GetDistanceDescription(result.distance);
        const char* typeName = result.typeName;
        const char* guidance = "";

        /* Log what was detected */
        LOG_INF("Ahead check: %s at %d mm (jumpable=%d, clearable=%d)",
                typeName, result.distance, isJumpable, isClearable);

        /* Get navigation guidance for obstacles that can't be traversed */
        if (!isJumpable && !isClearable) {
            guidance = GetNavigationGuidance(&playerPos, playerDyn, typeName);
        }

        if (isJumpable) {
            snprintf(announcement, sizeof(announcement),
                     "%s %s, %d millimeters. Can walk over.",
                     typeName, distDesc, result.distance);
        } else if (isClearable) {
            snprintf(announcement, sizeof(announcement),
                     "%s %s, %d millimeters. Can jump over.",
                     typeName, distDesc, result.distance);
        } else {
            /* Include navigation guidance */
            snprintf(announcement, sizeof(announcement),
                     "%s %s, %d millimeters. %s",
                     typeName, distDesc, result.distance, guidance);
        }

        /* Capitalize first letter */
        if (announcement[0] >= 'a' && announcement[0] <= 'z') {
            announcement[0] -= 32;
        }

        /* Play directional tone - centered since it's directly ahead */
        NavTone_PlayDirectional(0.0f, 0.0f);
    } else {
        snprintf(announcement, sizeof(announcement), "Clear ahead.");
    }

    /* Redundancy check - don't repeat same announcement within short time */
    static unsigned int lastAnnounceTime = 0;
    unsigned int currentTime = GetTickCount();

    if (strcmp(announcement, g_LastObstructionText) != 0 ||
        (currentTime - lastAnnounceTime) > 3000) {  /* Allow repeat after 3 seconds */

        TTS_Speak(announcement);
        strncpy(g_LastObstructionText, announcement, sizeof(g_LastObstructionText) - 1);
        g_LastObstructionText[sizeof(g_LastObstructionText) - 1] = '\0';
        lastAnnounceTime = currentTime;
    } else {
        /* Same announcement recently - just play tone without TTS */
        if (result.distance > 0) {
            NavTone_PlayDirectional(0.0f, 0.0f);
        }
    }
}

/* Announce surroundings (walls on all sides) with structure types */
extern "C" void Obstruction_AnnounceSurroundings(void)
{
    if (!Accessibility_IsAvailable()) return;
    if (!Player || !Player->ObStrategyBlock || !Player->ObStrategyBlock->DynPtr) return;

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    VECTORCH playerPos = playerDyn->Position;
    playerPos.vy -= 800;

    /* Get direction vectors */
    VECTORCH forward, left, right, back;

    forward.vx = playerDyn->OrientMat.mat31;
    forward.vy = playerDyn->OrientMat.mat32;
    forward.vz = playerDyn->OrientMat.mat33;
    Normalise(&forward);

    left.vx = -playerDyn->OrientMat.mat11;
    left.vy = -playerDyn->OrientMat.mat12;
    left.vz = -playerDyn->OrientMat.mat13;
    Normalise(&left);

    right.vx = playerDyn->OrientMat.mat11;
    right.vy = playerDyn->OrientMat.mat12;
    right.vz = playerDyn->OrientMat.mat13;
    Normalise(&right);

    back.vx = -forward.vx;
    back.vy = -forward.vy;
    back.vz = -forward.vz;

    int maxRange = OBSTRUCTION_FAR_DIST * 2;

    /* Use extended ray casts to get type information */
    RAY_RESULT frontResult = CastObstructionRayEx(&playerPos, &forward, maxRange);
    RAY_RESULT leftResult = CastObstructionRayEx(&playerPos, &left, maxRange);
    RAY_RESULT rightResult = CastObstructionRayEx(&playerPos, &right, maxRange);
    RAY_RESULT backResult = CastObstructionRayEx(&playerPos, &back, maxRange);

    char announcement[512];
    char* ptr = announcement;
    int remaining = sizeof(announcement);
    int written;

    /* Front */
    if (frontResult.distance > 0 && frontResult.distance < maxRange) {
        written = snprintf(ptr, remaining, "Front: %s, %d meters. ",
                          frontResult.typeName, frontResult.distance / 1000);
        ptr += written;
        remaining -= written;
    } else {
        written = snprintf(ptr, remaining, "Front clear. ");
        ptr += written;
        remaining -= written;
    }

    /* Left */
    if (leftResult.distance > 0 && leftResult.distance < maxRange) {
        written = snprintf(ptr, remaining, "Left: %s, %d meters. ",
                          leftResult.typeName, leftResult.distance / 1000);
        ptr += written;
        remaining -= written;
    } else {
        written = snprintf(ptr, remaining, "Left clear. ");
        ptr += written;
        remaining -= written;
    }

    /* Right */
    if (rightResult.distance > 0 && rightResult.distance < maxRange) {
        written = snprintf(ptr, remaining, "Right: %s, %d meters. ",
                          rightResult.typeName, rightResult.distance / 1000);
        ptr += written;
        remaining -= written;
    } else {
        written = snprintf(ptr, remaining, "Right clear. ");
        ptr += written;
        remaining -= written;
    }

    /* Back */
    if (backResult.distance > 0 && backResult.distance < maxRange) {
        written = snprintf(ptr, remaining, "Back: %s, %d meters.",
                          backResult.typeName, backResult.distance / 1000);
    } else {
        written = snprintf(ptr, remaining, "Back clear.");
    }

    LOG_INF("Surroundings: F=%s@%d L=%s@%d R=%s@%d B=%s@%d",
            frontResult.typeName, frontResult.distance,
            leftResult.typeName, leftResult.distance,
            rightResult.typeName, rightResult.distance,
            backResult.typeName, backResult.distance);

    TTS_Speak(announcement);
}

/* Toggle obstruction detection */
extern "C" void Obstruction_Toggle(void)
{
    g_ObstructionState.enabled = !g_ObstructionState.enabled;
    TTS_Speak(g_ObstructionState.enabled ?
              "Obstruction alerts enabled" : "Obstruction alerts disabled");
}

/* ============================================
 * Environment Description System
 * ============================================ */

/* Get spatial description based on distance */
static const char* GetSpatialDescription(int distance)
{
    if (distance < 2000) return "immediately";      /* < 2m */
    if (distance < 5000) return "nearby";           /* 2-5m */
    if (distance < 10000) return "";                /* 5-10m - just "to your left" */
    return "in the distance";                       /* > 10m */
}

/* Priority for sorting - lower = more important (announced first) */
static int GetFeaturePriority(const char* typeName)
{
    /* Enemies - highest priority */
    if (strcmp(typeName, "alien") == 0 || strcmp(typeName, "queen alien") == 0 ||
        strcmp(typeName, "facehugger") == 0 || strcmp(typeName, "predator") == 0 ||
        strcmp(typeName, "xenoborg") == 0 || strcmp(typeName, "marine") == 0 ||
        strcmp(typeName, "autogun") == 0) {
        return 0;
    }
    /* Interactive elements - high priority */
    if (strcmp(typeName, "door") == 0 || strcmp(typeName, "proximity door") == 0 ||
        strcmp(typeName, "lift door") == 0 || strcmp(typeName, "switch") == 0 ||
        strcmp(typeName, "lift") == 0 || strcmp(typeName, "terminal") == 0 ||
        strcmp(typeName, "generator") == 0 || strcmp(typeName, "self-destruct console") == 0) {
        return 1;
    }
    /* Notable objects */
    if (strcmp(typeName, "crate") == 0 || strcmp(typeName, "pillar") == 0 ||
        strcmp(typeName, "object") == 0 || strcmp(typeName, "structure") == 0) {
        return 2;
    }
    /* Walls - lowest priority */
    return 3;
}

/* Environment scan result */
typedef struct {
    const char* direction;  /* "ahead", "to your left", etc. */
    const char* typeName;   /* What's there */
    int distance;           /* Distance in game units */
    int priority;           /* For sorting */
} ENV_SCAN_ENTRY;

/* Compare function for sorting environment entries */
static int CompareEnvEntries(const void* a, const void* b)
{
    const ENV_SCAN_ENTRY* entryA = (const ENV_SCAN_ENTRY*)a;
    const ENV_SCAN_ENTRY* entryB = (const ENV_SCAN_ENTRY*)b;

    /* First by priority (important features first) */
    if (entryA->priority != entryB->priority) {
        return entryA->priority - entryB->priority;
    }
    /* Then by distance (closer first) */
    return entryA->distance - entryB->distance;
}

/* Describe the environment in all directions */
extern "C" void Environment_Describe(void)
{
    if (!Accessibility_IsAvailable()) {
        TTS_Speak("Environment scan unavailable.");
        return;
    }

    if (!Player || !Player->ObStrategyBlock || !Player->ObStrategyBlock->DynPtr) {
        TTS_Speak("Cannot scan: player unavailable.");
        return;
    }

    DYNAMICSBLOCK* playerDyn = Player->ObStrategyBlock->DynPtr;
    VECTORCH playerPos = playerDyn->Position;
    playerPos.vy -= 800;  /* Chest height */

    /* Get player's forward and right vectors */
    VECTORCH forward, right;
    forward.vx = playerDyn->OrientMat.mat31;
    forward.vy = 0;  /* Horizontal only for direction calculations */
    forward.vz = playerDyn->OrientMat.mat33;
    Normalise(&forward);

    right.vx = playerDyn->OrientMat.mat11;
    right.vy = 0;
    right.vz = playerDyn->OrientMat.mat13;
    Normalise(&right);

    /* Define 8 horizontal directions + up + down */
    VECTORCH dirs[10];
    const char* dirNames[10] = {
        "ahead",
        "ahead to your right",
        "to your right",
        "behind to your right",
        "behind you",
        "behind to your left",
        "to your left",
        "ahead to your left",
        "above",
        "below"
    };

    /* Calculate direction vectors */
    /* Forward (0 degrees) */
    dirs[0] = forward;

    /* Forward-right (45 degrees) */
    dirs[1].vx = (forward.vx + right.vx) / 2;
    dirs[1].vy = 0;
    dirs[1].vz = (forward.vz + right.vz) / 2;
    Normalise(&dirs[1]);

    /* Right (90 degrees) */
    dirs[2] = right;

    /* Back-right (135 degrees) */
    dirs[3].vx = (-forward.vx + right.vx) / 2;
    dirs[3].vy = 0;
    dirs[3].vz = (-forward.vz + right.vz) / 2;
    Normalise(&dirs[3]);

    /* Back (180 degrees) */
    dirs[4].vx = -forward.vx;
    dirs[4].vy = 0;
    dirs[4].vz = -forward.vz;

    /* Back-left (225 degrees) */
    dirs[5].vx = (-forward.vx - right.vx) / 2;
    dirs[5].vy = 0;
    dirs[5].vz = (-forward.vz - right.vz) / 2;
    Normalise(&dirs[5]);

    /* Left (270 degrees) */
    dirs[6].vx = -right.vx;
    dirs[6].vy = 0;
    dirs[6].vz = -right.vz;

    /* Forward-left (315 degrees) */
    dirs[7].vx = (forward.vx - right.vx) / 2;
    dirs[7].vy = 0;
    dirs[7].vz = (forward.vz - right.vz) / 2;
    Normalise(&dirs[7]);

    /* Up */
    dirs[8].vx = 0;
    dirs[8].vy = -ONE_FIXED;  /* Up in AVP coordinate system */
    dirs[8].vz = 0;

    /* Down */
    dirs[9].vx = 0;
    dirs[9].vy = ONE_FIXED;
    dirs[9].vz = 0;

    /* Scan all directions */
    int maxRange = OBSTRUCTION_FAR_DIST * 3;  /* Extended range for environment scan */
    ENV_SCAN_ENTRY entries[10];
    int numEntries = 0;
    int numClearDirs = 0;
    const char* clearDirections[10];

    for (int i = 0; i < 10; i++) {
        RAY_RESULT result = CastObstructionRayEx(&playerPos, &dirs[i], maxRange);

        if (result.distance > 0 && result.distance < maxRange) {
            entries[numEntries].direction = dirNames[i];
            entries[numEntries].typeName = result.typeName;
            entries[numEntries].distance = result.distance;
            entries[numEntries].priority = GetFeaturePriority(result.typeName);
            numEntries++;
        } else {
            /* Track clear directions */
            clearDirections[numClearDirs++] = dirNames[i];
        }
    }

    /* Sort entries by priority (important first) then distance */
    if (numEntries > 1) {
        qsort(entries, numEntries, sizeof(ENV_SCAN_ENTRY), CompareEnvEntries);
    }

    /* Build announcement */
    char announcement[1024];
    char* ptr = announcement;
    int remaining = sizeof(announcement);
    int written;

    /* Start with clear paths (if any ahead) */
    int hasOpenPath = 0;
    for (int i = 0; i < numClearDirs; i++) {
        if (strcmp(clearDirections[i], "ahead") == 0) {
            written = snprintf(ptr, remaining, "Open path ahead. ");
            ptr += written;
            remaining -= written;
            hasOpenPath = 1;
            break;
        }
    }

    /* Announce detected features (limit to 6 most important) */
    int announced = 0;
    for (int i = 0; i < numEntries && announced < 6; i++) {
        const char* spatial = GetSpatialDescription(entries[i].distance);
        int distMeters = entries[i].distance / 1000;

        if (distMeters < 1) distMeters = 1;

        if (strlen(spatial) > 0) {
            written = snprintf(ptr, remaining, "%s %s %s, %d meters. ",
                              entries[i].typeName, spatial, entries[i].direction, distMeters);
        } else {
            written = snprintf(ptr, remaining, "%s %s, %d meters. ",
                              entries[i].typeName, entries[i].direction, distMeters);
        }

        /* Capitalize first letter of each sentence */
        if (ptr[0] >= 'a' && ptr[0] <= 'z') {
            ptr[0] -= 32;
        }

        ptr += written;
        remaining -= written;
        announced++;
    }

    /* If nothing detected, mention all clear */
    if (numEntries == 0) {
        snprintf(ptr, remaining, "Area is clear in all directions.");
    }

    LOG_INF("Environment scan: %d features detected, %d clear directions", numEntries, numClearDirs);

    TTS_Speak(announcement);
}
