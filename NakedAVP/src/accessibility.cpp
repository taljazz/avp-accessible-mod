/*
 * AVP Accessibility Module - Implementation
 *
 * Uses Windows SAPI (Speech API) for text-to-speech.
 * Uses OpenAL for directional audio radar tones.
 * SAPI is the underlying API that System.Speech wraps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#include <sphelper.h>
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "ole32.lib")
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
/* SAPI COM interface */
static ISpVoice* g_pVoice = NULL;
static int g_COMInitialized = 0;
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

/* Play a directional radar tone for an entity
 * - Position determines stereo panning (left/right/front/back)
 * - Vertical offset determines pitch (above = higher, below = lower)
 * - Distance affects volume
 */
static void RadarTone_PlayDirectional(int targetX, int targetY, int targetZ,
                                       int playerX, int playerY, int playerZ,
                                       int playerYaw)
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

    /* Calculate pitch based on vertical offset
     * Above player = higher pitch, below = lower pitch
     * Range: 0.7 (below) to 1.5 (above), 1.0 at same level
     */
    float distance = sqrtf(dx*dx + dy*dy + dz*dz);
    float verticalRatio = 0.0f;
    if (distance > 0.0f) {
        verticalRatio = dy / distance;  /* -1 to 1 range */
    }

    /* Map vertical ratio to pitch: -1 (below) -> 0.7, 0 (level) -> 1.0, 1 (above) -> 1.5 */
    float pitch = 1.0f + (verticalRatio * 0.4f);
    if (pitch < 0.6f) pitch = 0.6f;
    if (pitch > 1.6f) pitch = 1.6f;

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

/* ============================================
 * TTS Implementation (Windows SAPI)
 * ============================================ */

#ifdef _WIN32

static int TTS_InitSAPI(void)
{
    HRESULT hr;

    if (g_pVoice != NULL) {
        return 1; /* Already initialized */
    }

    /* Initialize COM */
    if (!g_COMInitialized) {
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            Accessibility_Log("Failed to initialize COM: 0x%08X\n", hr);
            return 0;
        }
        g_COMInitialized = 1;
    }

    /* Create SAPI voice */
    hr = CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL,
                          IID_ISpVoice, (void**)&g_pVoice);
    if (FAILED(hr)) {
        Accessibility_Log("Failed to create SAPI voice: 0x%08X\n", hr);
        return 0;
    }

    /* Set initial rate and volume */
    g_pVoice->SetRate(AccessibilitySettings.tts_rate);
    g_pVoice->SetVolume((USHORT)AccessibilitySettings.tts_volume);

    Accessibility_Log("SAPI TTS initialized successfully\n");
    return 1;
}

static void TTS_ShutdownSAPI(void)
{
    if (g_pVoice != NULL) {
        g_pVoice->Release();
        g_pVoice = NULL;
    }

    if (g_COMInitialized) {
        CoUninitialize();
        g_COMInitialized = 0;
    }
}

static void TTS_SpeakInternal(const char* text, DWORD flags)
{
    if (!g_pVoice || !text || !AccessibilitySettings.tts_enabled) {
        return;
    }

    /* Convert to wide string for SAPI */
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (len <= 0) return;

    WCHAR* wtext = (WCHAR*)malloc(len * sizeof(WCHAR));
    if (!wtext) return;

    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, len);

    /* Speak the text */
    g_pVoice->Speak(wtext, flags, NULL);

    free(wtext);

    /* Store for repeat function */
    strncpy(g_LastSpokenText, text, sizeof(g_LastSpokenText) - 1);
    g_LastSpokenText[sizeof(g_LastSpokenText) - 1] = '\0';
}

#else
/* Non-Windows stub implementations */
static int TTS_InitSAPI(void) { return 0; }
static void TTS_ShutdownSAPI(void) {}
static void TTS_SpeakInternal(const char* text, unsigned int flags) {
    (void)text; (void)flags;
}
#endif

/* ============================================
 * Public TTS Functions
 * ============================================ */

extern "C" void TTS_Speak(const char* text)
{
    if (!AccessibilitySettings.enabled) return;

#ifdef _WIN32
    DWORD flags = SPF_ASYNC;
    if (AccessibilitySettings.tts_interrupt) {
        flags |= SPF_PURGEBEFORESPEAK;
    }
    TTS_SpeakInternal(text, flags);
#endif

    Accessibility_Log("TTS: %s\n", text);
}

extern "C" void TTS_SpeakQueued(const char* text)
{
    if (!AccessibilitySettings.enabled) return;

#ifdef _WIN32
    TTS_SpeakInternal(text, SPF_ASYNC);
#endif
}

extern "C" void TTS_SpeakPriority(const char* text)
{
    if (!AccessibilitySettings.enabled) return;

#ifdef _WIN32
    TTS_SpeakInternal(text, SPF_ASYNC | SPF_PURGEBEFORESPEAK);
#endif
}

extern "C" void TTS_Stop(void)
{
#ifdef _WIN32
    if (g_pVoice) {
        g_pVoice->Speak(NULL, SPF_PURGEBEFORESPEAK, NULL);
    }
#endif
}

extern "C" int TTS_IsSpeaking(void)
{
#ifdef _WIN32
    if (!g_pVoice) return 0;

    SPVOICESTATUS status;
    g_pVoice->GetStatus(&status, NULL);
    return (status.dwRunningState == SPRS_IS_SPEAKING);
#else
    return 0;
#endif
}

extern "C" void TTS_SetRate(int rate)
{
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    AccessibilitySettings.tts_rate = rate;

#ifdef _WIN32
    if (g_pVoice) {
        g_pVoice->SetRate(rate);
    }
#endif
}

extern "C" void TTS_SetVolume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    AccessibilitySettings.tts_volume = volume;

#ifdef _WIN32
    if (g_pVoice) {
        g_pVoice->SetVolume((USHORT)volume);
    }
#endif
}

/* ============================================
 * Initialization and Shutdown
 * ============================================ */

extern "C" int Accessibility_Init(void)
{
    if (g_AccessibilityInitialized) {
        return 1;
    }

    Accessibility_Log("Initializing accessibility system...\n");

    /* Initialize TTS */
    if (!TTS_InitSAPI()) {
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

    TTS_Stop();
    TTS_ShutdownSAPI();
    RadarTone_Shutdown();
    PitchTone_Shutdown();

    g_AccessibilityInitialized = 0;
    Accessibility_Log("Accessibility system shutdown\n");
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

    /* Play directional tone for nearest threat */
    if (nearestSB && nearestSB->DynPtr) {
        RadarTone_PlayDirectional(
            nearestSB->DynPtr->Position.vx,
            nearestSB->DynPtr->Position.vy,
            nearestSB->DynPtr->Position.vz,
            playerX, playerY, playerZ,
            playerYaw
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

extern "C" void Menu_OnTextDisplayed(const char* text, int isSelected)
{
    if (!Accessibility_IsAvailable() || !AccessibilitySettings.menu_narration_enabled) {
        return;
    }

    if (!text || strlen(text) == 0) return;

    /* Only announce if text changed and is selected */
    if (isSelected && strcmp(text, g_LastMenuText) != 0) {
        TTS_Speak(text);
        strncpy(g_LastMenuText, text, sizeof(g_LastMenuText) - 1);
        g_LastMenuText[sizeof(g_LastMenuText) - 1] = '\0';
    }
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

/* Play navigation tone with stereo panning based on direction */
static void NavTone_PlayDirectional(float angleOffset)
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

    /* angleOffset: -1.0 = hard left, 0 = center, 1.0 = hard right */
    /* Use SOURCE_RELATIVE for consistent UI audio that doesn't interfere with game 3D sounds */
    alSourcei(g_NavToneSource, AL_SOURCE_RELATIVE, AL_TRUE);

    /* Map to 3D position for stereo panning */
    float posX = angleOffset * 2.0f;
    float posZ = -1.0f;  /* In front */

    alSource3f(g_NavToneSource, AL_POSITION, posX, 0.0f, posZ);

    /* Higher pitch when closer to center (on target) */
    float pitch = 1.0f;
    float absAngle = (angleOffset < 0) ? -angleOffset : angleOffset;
    if (absAngle < 0.1f) {
        pitch = 1.3f;  /* Higher pitch when on target */
    } else if (absAngle < 0.3f) {
        pitch = 1.15f;
    }
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
                    bhvr == I_BehaviourLift);

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

        if (dist < nearestDist) {
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
    } else {
        AutoNavState.target_name = NULL;
        AutoNavState.target_distance = 0;
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

    /* Calculate direction to target */
    float dx = (float)(AutoNavState.target_x - playerX);
    float dz = (float)(AutoNavState.target_z - playerZ);

    /* Get player's forward direction from orientation matrix */
    float forwardX = (float)playerDyn->OrientMat.mat31 / 65536.0f;
    float forwardZ = (float)playerDyn->OrientMat.mat33 / 65536.0f;

    /* Normalize direction to target */
    float targetDist = sqrtf(dx*dx + dz*dz);
    if (targetDist < 1.0f) targetDist = 1.0f;
    float targetDirX = dx / targetDist;
    float targetDirZ = dz / targetDist;

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

    /* Play navigation tone every 20 frames */
    static int toneCounter = 0;
    toneCounter++;
    if (toneCounter >= 20) {
        toneCounter = 0;
        NavTone_PlayDirectional(angleOffset);
    }

    /* ============================================
     * Obstacle Avoidance for Autonavigation
     * ============================================ */

    /* Cast a ray toward target to check for obstacles */
    static int avoidanceState = 0;  /* 0=none, 1=avoiding left, 2=avoiding right */
    static int avoidanceTimer = 0;
    static int stuckCounter = 0;
    static int lastPlayerX = 0, lastPlayerZ = 0;

    VECTORCH rayOrigin = playerDyn->Position;
    rayOrigin.vy -= 800;  /* Chest height */

    VECTORCH targetDir;
    targetDir.vx = (int)(targetDirX * ONE_FIXED);
    targetDir.vy = 0;
    targetDir.vz = (int)(targetDirZ * ONE_FIXED);

    int obstacleDistance = 0;
    LOS_ObjectHitPtr = NULL;
    LOS_Lambda = 8000;  /* Check 8 meters ahead */
    FindPolygonInLineOfSight(&targetDir, &rayOrigin, 0, Player);

    if (LOS_ObjectHitPtr != NULL || LOS_Lambda < 8000) {
        obstacleDistance = LOS_Lambda;
    }

    /* Stuck detection - if we haven't moved much in a while */
    int movedX = playerX - lastPlayerX;
    int movedZ = playerZ - lastPlayerZ;
    int distMoved = (movedX * movedX + movedZ * movedZ);

    if (AutoNavState.auto_move && distMoved < 10000) {  /* Barely moved - threshold increased to reduce false positives */
        stuckCounter++;
        if (stuckCounter > 90) {  /* Stuck for ~1.5 seconds */
            /* Try reversing avoidance direction */
            avoidanceState = (avoidanceState == 1) ? 2 : 1;
            stuckCounter = 0;
            if (Announcement_IsAllowed(ANNOUNCE_PRIORITY_NORMAL)) {
                TTS_SpeakQueued("Rerouting.");
                Announcement_RecordTime(ANNOUNCE_PRIORITY_NORMAL);
            }
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

    if (obstacleDistance > 0 && obstacleDistance < 4000) {
        /* Obstacle in the way - need to avoid */
        if (avoidanceState == 0) {
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

    /* Apply rotation */
    if (AutoNavState.auto_rotate) {
        playerDyn->AngVelocity.EulerY = adjustedTurnAmount;
    }

    /* Apply movement - use SLOWER speed for better control during autonavigation */
    /* Normal speed is 32768, we use ~40% speed (13000) for more precise navigation */
    #define AUTONAV_FORWARD_SPEED 13000   /* Slower forward movement */
    #define AUTONAV_STRAFE_SPEED 8000     /* Slower strafe movement */

    if (AutoNavState.auto_move && targetDist > 3000) {
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

    /* Check if reached target */
    static int arrivedAnnounced = 0;
    if (targetDist < 3000) {
        if (!arrivedAnnounced && Announcement_IsAllowed(ANNOUNCE_PRIORITY_NORMAL)) {
            TTS_SpeakQueued("Target reached.");
            Announcement_RecordTime(ANNOUNCE_PRIORITY_NORMAL);
            arrivedAnnounced = 1;
        }
    } else {
        arrivedAnnounced = 0;
    }
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

/* Cast a ray in a direction and return distance to hit (0 if no hit) */
static int CastObstructionRay(VECTORCH* origin, VECTORCH* direction, int maxRange)
{
    /* Initialize LOS globals */
    LOS_ObjectHitPtr = NULL;
    LOS_Lambda = maxRange;

    /* Cast the ray */
    FindPolygonInLineOfSight(direction, origin, 0, Player);

    if (LOS_ObjectHitPtr != NULL || LOS_Lambda < maxRange) {
        return LOS_Lambda;
    }
    return 0;  /* No obstruction within range */
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

/* Announce current obstruction status (on-demand via hotkey) */
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

    int forwardDist = CastObstructionRay(&playerPos, &forward, OBSTRUCTION_FAR_DIST * 2);

    char announcement[256];

    if (forwardDist > 0) {
        int isJumpable, isClearable;
        AnalyzeObstruction(&playerPos, &LOS_Point, &isJumpable, &isClearable);

        const char* distDesc = GetDistanceDescription(forwardDist);

        if (isJumpable) {
            snprintf(announcement, sizeof(announcement),
                     "Step %s, %d millimeters. Can walk over.",
                     distDesc, forwardDist);
        } else if (isClearable) {
            snprintf(announcement, sizeof(announcement),
                     "Obstacle %s, %d millimeters. Can jump over.",
                     distDesc, forwardDist);
        } else {
            snprintf(announcement, sizeof(announcement),
                     "Wall %s, %d millimeters.",
                     distDesc, forwardDist);
        }

        /* Play directional tone - centered since it's directly ahead */
        NavTone_PlayDirectional(0.0f);
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
        if (forwardDist > 0) {
            NavTone_PlayDirectional(0.0f);
        }
    }
}

/* Announce surroundings (walls on all sides) */
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

    int frontDist = CastObstructionRay(&playerPos, &forward, maxRange);
    int leftDist = CastObstructionRay(&playerPos, &left, maxRange);
    int rightDist = CastObstructionRay(&playerPos, &right, maxRange);
    int backDist = CastObstructionRay(&playerPos, &back, maxRange);

    char announcement[512];
    char* ptr = announcement;
    int remaining = sizeof(announcement);
    int written;

    /* Front */
    if (frontDist > 0 && frontDist < maxRange) {
        written = snprintf(ptr, remaining, "Front %d. ", frontDist / 1000);
        ptr += written;
        remaining -= written;
    } else {
        written = snprintf(ptr, remaining, "Front clear. ");
        ptr += written;
        remaining -= written;
    }

    /* Left */
    if (leftDist > 0 && leftDist < maxRange) {
        written = snprintf(ptr, remaining, "Left %d. ", leftDist / 1000);
        ptr += written;
        remaining -= written;
    } else {
        written = snprintf(ptr, remaining, "Left clear. ");
        ptr += written;
        remaining -= written;
    }

    /* Right */
    if (rightDist > 0 && rightDist < maxRange) {
        written = snprintf(ptr, remaining, "Right %d. ", rightDist / 1000);
        ptr += written;
        remaining -= written;
    } else {
        written = snprintf(ptr, remaining, "Right clear. ");
        ptr += written;
        remaining -= written;
    }

    /* Back */
    if (backDist > 0 && backDist < maxRange) {
        written = snprintf(ptr, remaining, "Back %d.", backDist / 1000);
    } else {
        written = snprintf(ptr, remaining, "Back clear.");
    }

    TTS_Speak(announcement);
}

/* Toggle obstruction detection */
extern "C" void Obstruction_Toggle(void)
{
    g_ObstructionState.enabled = !g_ObstructionState.enabled;
    TTS_Speak(g_ObstructionState.enabled ?
              "Obstruction alerts enabled" : "Obstruction alerts disabled");
}
