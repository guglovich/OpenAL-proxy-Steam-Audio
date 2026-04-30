/**
* OpenAL32.dll — proxy для Lineage 2 ElmoreX3 (UE2.5) v11.0-SA-HRTF (LA 2.7)
* База: v10.6-SA-EAX (LA 2.6)
*
* LA 2.7:
* - SA Direct Sound + HRTF binaural pipeline (прототип: 1 источник с 440Hz тоном)
* - PCM → float32 → DirectEffect → BinauralEffect → interleave → OpenAL stereo
* - DirectEffect: distance attenuation + air absorption (БЕЗ геометрии)
* - BinauralEffect: HRTF spatialization, mono→stereo
* - Тестовый 440Hz тон генерируется внутри прокси
* - EAX3.0 поддержка из LA 2.6 сохранена
*
* LA 2.6:
* - EAX3.0 extension support: alIsExtensionPresent("EAX3.0")=AL_TRUE
* - EAXSet/EAXGet function pointers returned via alGetProcAddress
* - EAXSet(GUID_SOURCE, prop=0x0B, src, &val, 4) → EAXSOURCE_OCCLUSIONDIRECTFACTOR
* Маппинг: EAX occlusion (-10000..0) → SA occlusion (0..1) → gain modulation
* - EAXSet(GUID_SOURCE, prop=0x05, src, &val, 4) → EAXSOURCE_OUTSIDEVOLUMEHF (no-op, logged)
* - EAXSet(GUID_LISTENER, prop=0x01, src=0, &buf, 112) → EAXLISTENER_ALLPARAMETERS (no-op, logged)
* - EAXGet: returns AL_SUCCESS for all queries (ALAudio.dll only loads but never calls EAXGet)
* - Это активирует встроенный EAX путь в ALAudio.dll — SoundOcclusion работает через EAX
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>

// Forward declarations (определения ниже, после AL-констант)
static FILE*   g_log  = NULL;
static HMODULE g_orig = NULL;
static char g_my_dir[MAX_PATH] = {0};

static HMODULE load_original_openal(void)
{
    // Freya клиент: нет OpenAL32_orig.dll
    // Цепочка поиска: DefOpenAL32.dll → wrap_oal.dll → system32
    const char* candidates[] = {
        "DefOpenAL32.dll",
        "wrap_oal.dll",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        HMODULE mod = LoadLibraryA(candidates[i]);
        if (mod) {
            if (g_log) {
                fprintf(g_log, "[BOOT] loaded original OpenAL: %s @ %p\n", candidates[i], (void*)mod);
                fflush(g_log);
            }
            return mod;
        }
    }

    // Fallback: системный OpenAL32.dll
    char sys_dir[MAX_PATH] = {0};
    UINT n = GetSystemDirectoryA(sys_dir, (UINT)sizeof(sys_dir));
    if (n > 0 && n < (UINT)sizeof(sys_dir)) {
        char path[MAX_PATH] = {0};
        wsprintfA(path, "%s\\OpenAL32.dll", sys_dir);
        HMODULE mod = LoadLibraryA(path);
        if (mod) {
            if (g_log) {
                fprintf(g_log, "[BOOT] loaded system OpenAL: %s @ %p\n", path, (void*)mod);
                fflush(g_log);
            }
            return mod;
        }
    }
    return NULL;
}

// ─── Steam Audio C API (минимум, без заголовков SDK) ─────────────────────────
typedef int   IPLint32;
typedef float IPLfloat32;
typedef int   IPLbool;
typedef unsigned int IPLuint32;
typedef unsigned char IPLuint8;
#define IPLCALL __stdcall
#define IPL_TRUE  1
#define IPL_FALSE 0

typedef enum { IPL_STATUS_SUCCESS=0, IPL_STATUS_FAILURE=1 } IPLerror;

// Точный layout из phonon.h
typedef struct { IPLint32 samplingRate; IPLint32 frameSize; } IPLAudioSettings;

typedef struct {
    IPLuint32 version;          // 0x040400 для v4.4.0
    void*     logCallback;      // NULL
    void*     allocateCallback; // NULL
    void*     freeCallback;     // NULL
    int       simdLevel;        // 0 = SSE2
    // flags отсутствует в v4.4 (добавлено только в v4.8)
} IPLContextSettings;

typedef struct { IPLfloat32 x,y,z; } IPLVector3;
typedef struct { IPLVector3 right,up,ahead,origin; } IPLCoordinateSpace3;
typedef struct { IPLint32 numChannels; IPLint32 numSamples; IPLfloat32** data; } IPLAudioBuffer;

typedef void* IPLContext;
typedef void* IPLHRTF;
typedef void* IPLBinauralEffect;

#define IPL_NUM_BANDS 3

typedef enum { IPL_HRTFTYPE_DEFAULT=0 } IPLHRTFType;
typedef enum { IPL_HRTFINTERP_BILINEAR=1 } IPLHRTFInterpolation;
typedef enum { IPL_EFFECTSTATE_TAILCOMPLETE=0, IPL_AUDIOEFFECTSTATE_TAILREMAINING=1 } IPLAudioEffectState;

// ВАЖНО: sofaData/sofaDataSize были пропущены — из-за этого volume попадал
// на неверное смещение и HRTFCreate возвращал FAILURE
typedef struct {
    int             type;         // IPLHRTFType: 0 = DEFAULT
    const char*     sofaFileName; // NULL
    const IPLuint8* sofaData;     // NULL
    int             sofaDataSize; // 0
    float           volume;       // 1.0
    int             normType;     // 0 = NONE
} IPLHRTFSettings;

typedef struct { IPLHRTF hrtf; } IPLBinauralEffectSettings;
typedef struct {
    IPLVector3           direction;
    IPLHRTFInterpolation interpolation;
    IPLfloat32           spatialBlend;
    IPLHRTF              hrtf;
    IPLAudioBuffer*      peakDelays;
} IPLBinauralEffectParams;

typedef void* IPLDirectEffect;

typedef enum {
    IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION = 1 << 0,
    IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION = 1 << 1,
    IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY = 1 << 2,
    IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION = 1 << 3,
    IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION = 1 << 4
} IPLDirectEffectFlags;

typedef enum { IPL_TRANSMISSIONTYPE_FREQINDEPENDENT=0, IPL_TRANSMISSIONTYPE_FREQDEPENDENT=1 } IPLTransmissionType;

typedef struct { IPLint32 numChannels; } IPLDirectEffectSettings;

typedef struct {
    IPLDirectEffectFlags flags;
    IPLTransmissionType transmissionType;
    IPLfloat32 distanceAttenuation;
    IPLfloat32 airAbsorption[IPL_NUM_BANDS];
    IPLfloat32 directivity;
    IPLfloat32 occlusion;
    IPLfloat32 transmission[IPL_NUM_BANDS];
} IPLDirectEffectParams;

typedef IPLVector3 (__stdcall *pfn_iplCalculateRelativeDirection)(IPLContext, IPLVector3, IPLVector3, IPLVector3, IPLVector3);

// Function pointers — ОБЯЗАТЕЛЬНО __stdcall (phonon.dll экспортирует как IPLCALL=__stdcall)
typedef IPLerror (__stdcall *pfn_iplContextCreate)(IPLContextSettings*, IPLContext*);
typedef void     (__stdcall *pfn_iplContextRelease)(IPLContext*);
typedef IPLerror (__stdcall *pfn_iplHRTFCreate)(IPLContext, IPLAudioSettings*, IPLHRTFSettings*, IPLHRTF*);
typedef void     (__stdcall *pfn_iplHRTFRelease)(IPLHRTF*);
typedef IPLerror (__stdcall *pfn_iplBinauralEffectCreate)(IPLContext, IPLAudioSettings*, IPLBinauralEffectSettings*, IPLBinauralEffect*);
typedef void (__stdcall *pfn_iplBinauralEffectRelease)(IPLBinauralEffect*);
typedef IPLAudioEffectState (__stdcall *pfn_iplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*);
typedef void (__stdcall *pfn_iplBinauralEffectReset)(IPLBinauralEffect);
typedef IPLerror (__stdcall *pfn_iplDirectEffectCreate)(IPLContext, IPLAudioSettings*, IPLDirectEffectSettings*, IPLDirectEffect*);
typedef void (__stdcall *pfn_iplDirectEffectRelease)(IPLDirectEffect*);
typedef IPLAudioEffectState (__stdcall *pfn_iplDirectEffectApply)(IPLDirectEffect, IPLDirectEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*);
typedef void (__stdcall *pfn_iplDirectEffectReset)(IPLDirectEffect);
typedef void (__stdcall *pfn_iplAudioBufferAllocate)(IPLContext, IPLint32, IPLint32, IPLAudioBuffer*);
typedef void (__stdcall *pfn_iplAudioBufferFree)(IPLContext, IPLAudioBuffer*);
typedef void (__stdcall *pfn_iplAudioBufferInterleave)(IPLContext, IPLAudioBuffer*, IPLfloat32*);
typedef void (__stdcall *pfn_iplAudioBufferDeinterleave)(IPLContext, IPLfloat32*, IPLAudioBuffer*);
typedef void (__stdcall *pfn_iplAudioBufferMix)(IPLContext, IPLAudioBuffer*, IPLAudioBuffer*);

// ─── Steam Audio Scene / Simulator types ─────────────────────────────────────
// Координаты OBJ и AL — одна система (UE2 unreal units), масштаб = 1.0
// OBJ пишется скриптом as-is из instances.json, движок передаёт те же единицы в AL

typedef void* IPLScene;
typedef void* IPLStaticMesh;
typedef void* IPLSimulator;
typedef void* IPLSource;

typedef struct { int indices[3]; } IPLTriangle;

typedef struct {
    IPLfloat32 absorption[3];  // low/mid/high
    IPLfloat32 scattering;
    IPLfloat32 transmission[3];
} IPLMaterial;

typedef struct {
    int  type;  // 0 = IPL_SCENETYPE_DEFAULT
    void* closestHitCallback; void* anyHitCallback;
    void* batchedClosestHitCallback; void* batchedAnyHitCallback;
    void* userData; void* embreeDevice; void* radeonRaysDevice;
} IPLSceneSettings_t;

typedef struct {
    IPLint32     numVertices;
    IPLint32     numTriangles;
    IPLint32     numMaterials;
    IPLVector3*  vertices;
    IPLTriangle* triangles;
    IPLint32*    materialIndices;
    IPLMaterial* materials;
} IPLStaticMeshSettings_t;

typedef struct {
    IPLint32   flags;        // IPL_SIMULATIONFLAGS_DIRECT = 1
    IPLint32   sceneType;    // 0 = DEFAULT
    IPLint32   reflectionType; // 1 = PARAMETRIC
    IPLint32   maxNumOcclusionSamples;
    IPLint32   maxNumRays;
    IPLint32   numDiffuseSamples;
    IPLfloat32 maxDuration;
    IPLint32   maxOrder;
    IPLint32   maxNumSources;
    IPLint32   numThreads;
    IPLint32   rayBatchSize;
    IPLint32   numVisSamples;
    IPLint32   samplingRate;
    IPLint32   frameSize;
    void*      openCLDevice; void* radeonRaysDevice; void* tanDevice;
} IPLSimulationSettings_t;

typedef struct { IPLint32 flags; } IPLSourceSettings_t;

typedef struct {
    IPLCoordinateSpace3 listener;
    IPLint32   numRays; IPLint32 numBounces;
    IPLfloat32 duration; IPLint32 order;
    IPLfloat32 irradianceMinDistance;
    void* pathingVisCallback; void* pathingUserData;
} IPLSimulationSharedInputs_t;

typedef struct {
    IPLint32   type;
    IPLfloat32 minDistance;
    void* callback; void* userData;
} IPLDistAttModel_t;

typedef struct {
    IPLint32   type;
    IPLfloat32 coefficients[3];
    void* callback; void* userData;
} IPLAirAbsModel_t;

typedef struct {
    IPLfloat32 dipoleWeight; IPLfloat32 dipolePower;
} IPLDirectivity_t;

typedef struct {
    IPLint32   flags;       // IPL_SIMULATIONFLAGS_DIRECT = 1
    IPLint32   directFlags; // IPL_DIRECTSIMULATIONFLAGS_OCCLUSION = 1<<3
    IPLCoordinateSpace3  source;
    IPLDistAttModel_t    distanceAttenuationModel;
    IPLAirAbsModel_t     airAbsorptionModel;
    IPLDirectivity_t     directivity;
    IPLint32   occlusionType;    // 1 = RAYCAST (было 0)
    IPLfloat32 occlusionRadius;  // 2.0f (было 0)
    IPLint32   numOcclusionSamples; // 8 (было 1)
    // reverb/reflection (zeroed, не используются для occlusion-only)
    IPLfloat32 reverbScale[3];
    IPLfloat32 hybridReverbTransitionTime;
    IPLfloat32 hybridReverbOverlapPercent;
    IPLint32   baked;
    IPLint32   bakedDataIdentifier[2];  // type + index (IPLBakedDataIdentifier)
    // pathing (zeroed, не используются)
    void*      pathingProbes;
    IPLfloat32 visRadius; IPLfloat32 visThreshold; IPLfloat32 visRange;
    IPLint32   pathingOrder;
    IPLint32   enableValidation; IPLint32 findAlternatePaths;
    IPLint32   numTransmissionRays;
    void*      deviationModel;
} IPLSimulationInputs_t;

// Нам нужно только поле direct.occlusion — оно первое в структуре
typedef struct {
    IPLint32   flags;
    IPLint32   transmissionType;
    IPLfloat32 distanceAttenuation;
    IPLfloat32 airAbsorption[3];
    IPLfloat32 directivity;
    IPLfloat32 occlusion;
    IPLfloat32 transmission[3];
} IPLDirectEffectParams_t;

typedef struct {
    IPLDirectEffectParams_t direct;
    unsigned char _padding[512];  // reflections + pathing, нам не нужны
} IPLSimulationOutputs_t;

// Function pointers — сцена и симулятор
typedef IPLerror (__stdcall *pfn_iplSceneCreate)(IPLContext, IPLSceneSettings_t*, IPLScene*);
typedef void     (__stdcall *pfn_iplSceneRelease)(IPLScene*);
typedef void     (__stdcall *pfn_iplSceneCommit)(IPLScene);
typedef IPLerror (__stdcall *pfn_iplStaticMeshCreate)(IPLScene, IPLStaticMeshSettings_t*, IPLStaticMesh*);
typedef void     (__stdcall *pfn_iplStaticMeshRelease)(IPLStaticMesh*);
typedef void     (__stdcall *pfn_iplStaticMeshAdd)(IPLStaticMesh, IPLScene);
typedef IPLerror (__stdcall *pfn_iplSimulatorCreate)(IPLContext, IPLSimulationSettings_t*, IPLSimulator*);
typedef void     (__stdcall *pfn_iplSimulatorRelease)(IPLSimulator*);
typedef void     (__stdcall *pfn_iplSimulatorSetScene)(IPLSimulator, IPLScene);
typedef void     (__stdcall *pfn_iplSimulatorCommit)(IPLSimulator);
typedef void     (__stdcall *pfn_iplSimulatorRunDirect)(IPLSimulator);
typedef void     (__stdcall *pfn_iplSimulatorSetSharedInputs)(IPLSimulator, int, IPLSimulationSharedInputs_t*);
typedef IPLerror (__stdcall *pfn_iplSourceCreate)(IPLSimulator, IPLSourceSettings_t*, IPLSource*);
typedef void     (__stdcall *pfn_iplSourceRelease)(IPLSource*);
typedef void     (__stdcall *pfn_iplSourceAdd)(IPLSource, IPLSimulator);
typedef void     (__stdcall *pfn_iplSourceRemove)(IPLSource, IPLSimulator);
typedef void     (__stdcall *pfn_iplSourceSetInputs)(IPLSource, int, IPLSimulationInputs_t*);
typedef void     (__stdcall *pfn_iplSourceGetOutputs)(IPLSource, int, IPLSimulationOutputs_t*);

// ─── SA globals ──────────────────────────────────────────────────────────────
#define SA_SAMPLE_RATE 44100
#define SA_FRAME_SIZE 512
#define SA_LISTENER_INTERVAL_MS 16 // ~60 Hz

// ─── Config (loaded from LineageAudio.ini) ─────────────────────────────────
static float cfg_hrtf_blend = 1.0f;       // spatialBlend (0=dry, 1=full HRTF)
static float cfg_gain_master = 1.0f;      // master gain multiplier after HRTF
static float cfg_dist_atten = 1.0f;       // distance attenuation multiplier (0=disable, 1=1/dist)
static float cfg_air_absorb = 0.0f;       // air absorption amount (0=off, 1=full)
static float cfg_occlusion = 1.0f;        // occlusion amount (0=off, 1=full)
static int   cfg_hrtf_enabled = 1;        // 0=pass through, 1=HRTF processing
static int   cfg_direct_effect = 1;       // 0=skip DirectEffect, 1=apply
static float cfg_min_distance = 1.0f;     // minimum distance for attenuation
static float cfg_volume_boost = 1.0f;     // output volume boost (1.0=normal, 2.0=2x)
static int   cfg_hud = 1;                 // 0=no HUD, 1=window title HUD

static HMODULE           g_phonon        = NULL;
static IPLContext        g_sa_ctx        = NULL;
static IPLHRTF           g_sa_hrtf       = NULL;
static int               g_sa_ok         = 0;  // 1 = SA инициализирован
static int               g_sa_enabled    = 1;  // RightAlt toggle

#define SA_MAX_SRC 64

// ─── Scene / Simulator globals ───────────────────────────────────────────────
static IPLScene      g_ipl_scene   = NULL;
static IPLStaticMesh g_ipl_mesh    = NULL;
static IPLSimulator  g_ipl_sim     = NULL;
static IPLSource     g_ipl_src[SA_MAX_SRC] = {0};
static int           g_scene_ok    = 0;

// Occlusion результаты (пишет поток ~10Hz, читает audio-поток)
// float на x86 читается атомарно — мьютекс не нужен
static float g_occlusion[SA_MAX_SRC];  // 0=заглушено, 1=открыто
// ВАЖНО: инициализируем 1.0 чтобы не заглушить звук до первого расчёта
static float g_user_gain[SA_MAX_SRC] = {
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
};

// Поток симулятора
static HANDLE        g_sim_thread  = NULL;
static volatile int  g_sim_run     = 0;

// SA function pointers — сцена и симулятор
static pfn_iplSceneCreate             sa_SceneCreate            = NULL;
static pfn_iplSceneRelease            sa_SceneRelease           = NULL;
static pfn_iplSceneCommit             sa_SceneCommit            = NULL;
static pfn_iplStaticMeshCreate        sa_StaticMeshCreate       = NULL;
static pfn_iplStaticMeshRelease       sa_StaticMeshRelease      = NULL;
static pfn_iplStaticMeshAdd           sa_StaticMeshAdd          = NULL;
static pfn_iplSimulatorCreate         sa_SimulatorCreate        = NULL;
static pfn_iplSimulatorRelease        sa_SimulatorRelease       = NULL;
static pfn_iplSimulatorSetScene       sa_SimulatorSetScene      = NULL;
static pfn_iplSimulatorCommit         sa_SimulatorCommit        = NULL;
static pfn_iplSimulatorRunDirect      sa_SimulatorRunDirect     = NULL;
static pfn_iplSimulatorSetSharedInputs sa_SimulatorSetSharedInputs = NULL;
static pfn_iplSourceCreate            sa_SourceCreate           = NULL;
static pfn_iplSourceRelease           sa_SourceRelease          = NULL;
static pfn_iplSourceAdd               sa_SourceAdd              = NULL;
static pfn_iplSourceRemove            sa_SourceRemove           = NULL;
static pfn_iplSourceSetInputs         sa_SourceSetInputs        = NULL;
static pfn_iplSourceGetOutputs        sa_SourceGetOutputs       = NULL;

// Геометрия сцены (живёт между sa_scene_init и sa_scene_shutdown)
static IPLVector3*  g_mesh_verts   = NULL;
static IPLTriangle* g_mesh_tris    = NULL;
static IPLint32*    g_mesh_matidx  = NULL;
static IPLMaterial* g_mesh_mats    = NULL;
static int          g_mesh_nverts  = 0;
static int          g_mesh_ntris   = 0;

// Listener
static IPLCoordinateSpace3 g_sa_listener = {{0,0,0},{0,0,-1},{0,1,0},{1,0,0}};
static DWORD               g_sa_listener_ts = 0;
static float               g_sa_listener_prev_x = 0, g_sa_listener_prev_y = 0, g_sa_listener_prev_z = 0;
static int                 g_sa_listener_static_ticks = 0;  // счётчик статичных тиков

// Трекинг неподвижных источников — для поиска статуи
// Источник считается статичным если не двигался N тиков подряд
#define STATIC_SRC_TICKS 10  // 1 секунда при 10Hz
static int g_src_still_ticks[SA_MAX_SRC] = {0};  // счётчик тиков без движения
static float g_src_prev_x[SA_MAX_SRC] = {0};
static float g_src_prev_y[SA_MAX_SRC] = {0};
static float g_src_prev_z[SA_MAX_SRC] = {0};

// SA function pointers — только Context/HRTF (нужны для IPLSimulator)
static pfn_iplContextCreate sa_ContextCreate = NULL;
static pfn_iplContextRelease sa_ContextRelease = NULL;
static pfn_iplHRTFCreate sa_HRTFCreate = NULL;
static pfn_iplHRTFRelease sa_HRTFRelease = NULL;

// SA function pointers — Direct/Binaural effects + AudioBuffer
static pfn_iplDirectEffectCreate sa_DirectEffectCreate = NULL;
static pfn_iplDirectEffectRelease sa_DirectEffectRelease = NULL;
static pfn_iplDirectEffectApply sa_DirectEffectApply = NULL;
static pfn_iplDirectEffectReset sa_DirectEffectReset = NULL;
static pfn_iplBinauralEffectCreate sa_BinauralEffectCreate = NULL;
static pfn_iplBinauralEffectRelease sa_BinauralEffectRelease = NULL;
static pfn_iplBinauralEffectApply sa_BinauralEffectApply = NULL;
static pfn_iplBinauralEffectReset sa_BinauralEffectReset = NULL;
static pfn_iplAudioBufferAllocate sa_AudioBufferAllocate = NULL;
static pfn_iplAudioBufferFree sa_AudioBufferFree = NULL;
static pfn_iplAudioBufferInterleave sa_AudioBufferInterleave = NULL;
static pfn_iplAudioBufferDeinterleave sa_AudioBufferDeinterleave = NULL;
static pfn_iplAudioBufferMix sa_AudioBufferMix = NULL;
static pfn_iplCalculateRelativeDirection sa_CalculateRelativeDirection = NULL;

// SA effects — прототип с 1 источником (440Hz тестовый тон)
static IPLDirectEffect g_direct_effect = NULL;
static IPLBinauralEffect g_binaural_effect = NULL;
static IPLAudioBuffer g_buf_in_mono;       // 1ch, mono input
static IPLAudioBuffer g_buf_direct;        // 1ch, after DirectEffect
static IPLAudioBuffer g_buf_binaural;      // 2ch, after BinauralEffect
static IPLfloat32* g_buf_interleaved = NULL; // 2ch interleaved float32
static short* g_buf_pcm16 = NULL;            // 2ch interleaved int16 (for OpenAL)
static int g_effects_ok = 0;              // 1 = effects initialized

// SA test tone output source
static unsigned int g_test_al_src = 0;     // OpenAL stereo source
static unsigned int g_test_al_buf[3];      // triple-buffered AL buffers
static int g_test_al_bufs_generated = 0;   // сколько буферов уже сгенерировано
static int g_test_audio_active = 0;        // 1 = audio thread running
static HANDLE g_test_audio_thread = NULL;  // audio processing thread
static volatile int g_test_audio_run = 0;  // stop flag

// Test tone state
static float g_test_tone_phase = 0.0f;    // 440Hz oscillator phase

// phonon.dll на Windows 32-bit экспортирует в stdcall-декорированном виде:
// _iplContextCreate@8, _iplHRTFCreate@12, etc.
// GetProcAddress не находит их по "iplContextCreate" — нужно пробовать оба варианта.
// Вспомогательная функция перебирает суффиксы @0..@64 (с шагом 4).
static void* sa_getproc(const char* name)
{
    // Только stdcall-декорированный вариант _name@N — именно так экспортирует phonon.dll
    // Неукрашенный вариант пропускаем — он может резолвиться в неверный адрес
    char decorated[128];
    for (int n = 0; n <= 128; n += 4) {
        _snprintf(decorated, sizeof(decorated), "_%s@%d", name, n);
        void* p = (void*)GetProcAddress(g_phonon, decorated);
        if (p) {
            if (g_log) { fprintf(g_log,"[SA] resolved %s -> %s @ %p\n", name, decorated, p); fflush(g_log); }
            return p;
        }
    }
    if (g_log) { fprintf(g_log,"[SA] not found: %s\n", name); fflush(g_log); }
    return NULL;
}

#define SA_LOAD(name) \
    sa_##name = (pfn_ipl##name)sa_getproc("ipl"#name); \
    if (!sa_##name) { fprintf(g_log,"[SA] missing ipl"#name"\n"); fflush(g_log); goto sa_fail; }

// Callback для логов самого Steam Audio SDK
static void IPLCALL sa_log_callback(int /*level*/, const char* message)
{
    if (g_log) { fprintf(g_log, "[SA-SDK] %s\n", message); fflush(g_log); }
}

// Позиция/gain источников — для симулятора
#define MAX_SOURCES 64
#define MAX_BUFFERS 512

struct BufInfo {
    unsigned int id;
    int format;
    int frequency;
    int channels;
    int sample_size;
    int data_size;
    int active;
    short* pcm16;
    int pcm16_samples;
};

struct SrcState {
    unsigned int id;
    float x,y,z;
    int active;
    int relative;
    int diag_log_count;
    unsigned int current_buf;
};

static BufInfo g_buf[MAX_BUFFERS];
static int g_buf_count = 0;
static SrcState g_src[MAX_SOURCES];
static int sa_src_slot(unsigned int id);

// Флаг: alSourcef вызван изнутри прокси (не перезаписывать g_user_gain)
static volatile int g_internal_gain_set = 0;

// Per-source override position (set in alSourcePlay, used in alSourcefv)
struct SrcOverridePos { unsigned int src_id; float x, y, z; int active; };
static SrcOverridePos g_override_pos[MAX_SOURCES];
static int g_override_pos_inited = 0;
static void ensure_override_init() {
    if (!g_override_pos_inited) { memset(g_override_pos, 0, sizeof(g_override_pos)); g_override_pos_inited = 1; }
}
static SrcOverridePos* find_override(unsigned int s) {
    for (int i = 0; i < MAX_SOURCES; i++) if (g_override_pos[i].active && g_override_pos[i].src_id == s) return &g_override_pos[i];
    return NULL;
}
static SrcOverridePos* alloc_override(unsigned int s) {
    for (int i = 0; i < MAX_SOURCES; i++) if (!g_override_pos[i].active) { g_override_pos[i].src_id = s; g_override_pos[i].active = 1; return &g_override_pos[i]; }
    return NULL;
}

// ─── OBJ loader + Scene/Simulator init ───────────────────────────────────────

// Материал "камень" для всей геометрии DEV
static const IPLMaterial k_stone = {
    { 0.05f, 0.07f, 0.08f }, 0.05f, { 0.015f, 0.002f, 0.001f }
};

static int load_obj_geometry(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) {
        if (g_log) { fprintf(g_log,"[SCENE] cannot open: %s\n", path); fflush(g_log); }
        return 0;
    }
    int nv_alloc = 0, nf_alloc = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='v' && line[1]==' ') nv_alloc++;
        else if (line[0]=='f' && line[1]==' ') nf_alloc++;
    }
    rewind(f);
    if (!nv_alloc || !nf_alloc) {
        if (g_log) { fprintf(g_log,"[SCENE] OBJ empty v=%d f=%d\n",nv_alloc,nf_alloc); fflush(g_log); }
        fclose(f); return 0;
    }
    float* raw_x = (float*)malloc(nv_alloc * sizeof(float));
    float* raw_y = (float*)malloc(nv_alloc * sizeof(float));
    float* raw_z = (float*)malloc(nv_alloc * sizeof(float));
    int* remap = (int*)malloc(nv_alloc * sizeof(int));
    IPLTriangle* tris = (IPLTriangle*)malloc(nf_alloc * sizeof(IPLTriangle));
    IPLint32* midx = (IPLint32*)malloc(nf_alloc * sizeof(IPLint32));
    IPLMaterial* mats = (IPLMaterial*)malloc(sizeof(IPLMaterial));
    if (!raw_x || !raw_y || !raw_z || !remap || !tris || !midx || !mats) {
        free(raw_x); free(raw_y); free(raw_z); free(remap);
        free(tris); free(midx); free(mats);
        if (g_log) { fprintf(g_log,"[SCENE] OOM\n"); fflush(g_log); }
        fclose(f); return 0;
    }
    mats[0] = k_stone;

    int nv_raw = 0, nv_clean = 0;
    int fi = 0, fi_skip = 0;
    memset(remap, 0, nv_alloc * sizeof(int));
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='v' && line[1]==' ') {
            float x,y,z;
            if (sscanf(line+2, "%f %f %f", &x, &y, &z) == 3) {
                if (nv_raw < nv_alloc) {
                    raw_x[nv_raw] = x; raw_y[nv_raw] = y; raw_z[nv_raw] = z;
                    if (fabsf(z) > 100000.0f || fabsf(y) > 100000.0f) {
                        remap[nv_raw] = -1;
                    } else {
                        remap[nv_raw] = nv_clean;
                        nv_clean++;
                    }
                    nv_raw++;
                }
            }
        } else if (line[0]=='f' && line[1]==' ') {
            int a=0, b=0, c=0;
            const char* p = line + 2;
            if (sscanf(p, "%d", &a) == 1) {
                while (*p && *p != ' ' && *p != '\t') p++;
                while (*p == ' ' || *p == '\t') p++;
                if (sscanf(p, "%d", &b) == 1) {
                    while (*p && *p != ' ' && *p != '\t') p++;
                    while (*p == ' ' || *p == '\t') p++;
                    sscanf(p, "%d", &c);
                }
            }
            if (a>0 && b>0 && c>0 && a<=nv_raw && b<=nv_raw && c<=nv_raw
                && remap[a-1]>=0 && remap[b-1]>=0 && remap[c-1]>=0 && fi < nf_alloc) {
                tris[fi].indices[0] = remap[a-1];
                tris[fi].indices[1] = remap[b-1];
                tris[fi].indices[2] = remap[c-1];
                midx[fi] = 0; fi++;
            } else {
                fi_skip++;
            }
        }
    }
    fclose(f);

    IPLVector3* verts = (IPLVector3*)malloc(nv_clean * sizeof(IPLVector3));
    if (!verts) {
        free(raw_x); free(raw_y); free(raw_z); free(remap);
        free(tris); free(midx); free(mats);
        if (g_log) { fprintf(g_log,"[SCENE] OOM2\n"); fflush(g_log); }
        return 0;
    }
    for (int i = 0; i < nv_raw; i++) {
        if (remap[i] >= 0) {
            int j = remap[i];
            verts[j].x = raw_x[i];
            verts[j].y = raw_z[i];
            verts[j].z = -raw_y[i];
        }
    }
    free(raw_x); free(raw_y); free(raw_z); free(remap);

    if (g_log) { fprintf(g_log,"[SCENE] OBJ: raw=%d clean=%d(skip_corrupt=%d) tris=%d(skip=%d)\n",
        nv_raw, nv_clean, nv_raw-nv_clean, fi, fi_skip); fflush(g_log); }

    g_mesh_verts = verts; g_mesh_tris = tris;
    g_mesh_matidx = midx; g_mesh_mats = mats;
    g_mesh_nverts = nv_clean; g_mesh_ntris = fi;
    if (g_log) { fprintf(g_log,"[SCENE] OBJ loaded: %d verts, %d tris (L2->SA: x,z,-y)\n", nv_clean, fi);
        float bx0=1e9,bx1=-1e9,by0=1e9,by1=-1e9,bz0=1e9,bz1=-1e9;
        for (int j=0;j<nv_clean;j++) {
            if (verts[j].x<bx0) bx0=verts[j].x; if (verts[j].x>bx1) bx1=verts[j].x;
            if (verts[j].y<by0) by0=verts[j].y; if (verts[j].y>by1) by1=verts[j].y;
            if (verts[j].z<bz0) bz0=verts[j].z; if (verts[j].z>bz1) bz1=verts[j].z;
        }
        fprintf(g_log,"[SCENE] SA bounds: X=[%.0f..%.0f] Y=[%.0f..%.0f] Z=[%.0f..%.0f]\n",bx0,bx1,by0,by1,bz0,bz1);
        fflush(g_log);
    }
    return 1;
}

static void scene_update_source(int slot, SrcState* st)
{
    if (slot < 0 || !g_ipl_src[slot] || !sa_SourceSetInputs) return;
    if (!st || st->relative) return;

    IPLSimulationInputs_t inp;
    memset(&inp, 0, sizeof(inp));
    inp.flags       = 1;    // IPL_SIMULATIONFLAGS_DIRECT
    inp.directFlags = 1<<3; // IPL_DIRECTSIMULATIONFLAGS_OCCLUSION (FIX: было 1<<2=APPLYDIRECTIVITY, правильно 1<<3=APPLYOCCLUSION)
    inp.occlusionType = 1;  // RAYCAST (было 0)
    inp.occlusionRadius = 2.0f;  // радиус проверки (было 0)
    inp.numOcclusionSamples = 8; // больше лучей (было 1)
    // Позиция источника — as-is из игрового движка (UE2 = OBJ)
    inp.source.origin.x =  st->x;
    inp.source.origin.y =  st->z;
    inp.source.origin.z = -st->y;
    inp.source.ahead.x  = 0.0f; inp.source.ahead.y  = 0.0f; inp.source.ahead.z = -1.0f;
    inp.source.up.x     = 0.0f; inp.source.up.y     = 1.0f; inp.source.up.z    =  0.0f;
    inp.source.right.x  = 1.0f; inp.source.right.y  = 0.0f; inp.source.right.z =  0.0f;
    // модели DEFAULT — SA считает сам по расстоянию
    inp.distanceAttenuationModel.type = 0;
    inp.airAbsorptionModel.type       = 0;
    sa_SourceSetInputs(g_ipl_src[slot], 1 /*DIRECT*/, &inp);

    // Диагностика: лог первого обновления для каждого слота
    if (g_log && st->diag_log_count < 3) {
        fprintf(g_log, "[DIAG] src[%d] slot=%d game=(%.1f,%.1f,%.1f) sim=(%.1f,%.1f,%.1f) flags=%d directFlags=%d occType=%d\n",
            slot, slot,
            st->x, st->y, st->z,
            inp.source.origin.x, inp.source.origin.y, inp.source.origin.z,
            inp.flags, inp.directFlags, inp.occlusionType);
        fflush(g_log);
        st->diag_log_count++;
    }
}

// Поток симулятора — ~10 Hz
static DWORD WINAPI sim_thread_proc(LPVOID)
{
    int diag_tick = 0;  // диагностический лог каждые 20 тиков = 2 сек

    while (g_sim_run && g_ipl_sim) {
        // Обновляем listener
        IPLVector3 sim_listener_pos = {0,0,0};
        if (sa_SimulatorSetSharedInputs) {
            IPLSimulationSharedInputs_t si;
            memset(&si, 0, sizeof(si));
            // LA 1.2: координаты as-is (без инверсии Y)
            si.listener = g_sa_listener;
            si.listener.origin.x = g_sa_listener.origin.x;
            si.listener.origin.y = g_sa_listener.origin.z;
            si.listener.origin.z = -g_sa_listener.origin.y;
            si.numRays  = 32; si.numBounces = 1;
            si.duration = 1.0f; si.order = 0;
            si.irradianceMinDistance = 1.0f;
            sim_listener_pos = si.listener.origin;
            sa_SimulatorSetSharedInputs(g_ipl_sim, 1 /*DIRECT*/, &si);
        }
        sa_SimulatorRunDirect(g_ipl_sim);

        // Читаем occlusion для каждого активного 3D источника
        int active_count = 0;
        for (int i = 0; i < SA_MAX_SRC; i++) {
            if (!g_ipl_src[i] || !g_src[i].active || g_src[i].relative) {
                g_occlusion[i] = 1.0f;
                g_src_still_ticks[i] = 0;
                continue;
            }
            // Фильтруем невалидные координаты (pos=1e9 = источник без позиции)
            float ax = g_src[i].x < 0 ? -g_src[i].x : g_src[i].x;
            float ay = g_src[i].y < 0 ? -g_src[i].y : g_src[i].y;
            if (ax > 500000.0f || ay > 500000.0f) {
                g_occlusion[i] = 1.0f;
                g_src_still_ticks[i] = 0;
                continue;
            }
            IPLSimulationOutputs_t out;
            memset(&out, 0, sizeof(out));
            sa_SourceGetOutputs(g_ipl_src[i], 1 /*DIRECT*/, &out);
            float occ = out.direct.occlusion;
            if (occ < 0.0f) occ = 0.0f;
            if (occ > 1.0f) occ = 1.0f;
            g_occlusion[i] = occ;
            active_count++;

            // Трекинг неподвижности — ищем статичные источники (статуя)
            float dx = g_src[i].x - g_src_prev_x[i];
            float dy = g_src[i].y - g_src_prev_y[i];
            float dz = g_src[i].z - g_src_prev_z[i];
            float moved = dx*dx + dy*dy + dz*dz;
            if (moved < 1.0f) {
                g_src_still_ticks[i]++;
            } else {
                g_src_still_ticks[i] = 0;
                g_src_prev_x[i] = g_src[i].x;
                g_src_prev_y[i] = g_src[i].y;
                g_src_prev_z[i] = g_src[i].z;
            }
        }

        // Диагностика каждые 2 секунды
        if (g_log && (++diag_tick % 20) == 0) {
            SYSTEMTIME st; GetLocalTime(&st);
            // Определяем: лобби или игровой мир (|x|<5000 И |y|<5000 = возможно лобби)
            // ВАЖНО: малые координаты могут быть также ошибкой прокси или OBJ!
            // Ключевой критерий: лобби = СТАТИЧНЫЕ координаты, игра = МЕНЯЮТСЯ
            float lx = g_sa_listener.origin.x < 0 ? -g_sa_listener.origin.x : g_sa_listener.origin.x;
            float ly = g_sa_listener.origin.y < 0 ? -g_sa_listener.origin.y : g_sa_listener.origin.y;
            const char* zone_tag;
            if (g_sa_listener_static_ticks >= 20) {
                zone_tag = " [STATIC - lobby?]";
            } else if (lx < 5000.0f && ly < 5000.0f) {
                zone_tag = " [MOVING low coord - proxy/OBJ bug?]";
            } else {
                zone_tag = "";
            }
            fprintf(g_log,
                "[%02d:%02d:%02d.%03d] OCCL_DIAG active=%d "
                "listener_game=(%.0f,%.0f,%.0f) listener_sim=(%.0f,%.0f,%.0f)%s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                active_count,
                (double)g_sa_listener.origin.x,
                (double)g_sa_listener.origin.y,
                (double)g_sa_listener.origin.z,
                (double)sim_listener_pos.x,
                (double)sim_listener_pos.y,
                (double)sim_listener_pos.z,
                zone_tag);
            for (int i = 0; i < SA_MAX_SRC; i++) {
                if (!g_src[i].active || g_src[i].relative) continue;
                float ax = g_src[i].x < 0 ? -g_src[i].x : g_src[i].x;
                float ay = g_src[i].y < 0 ? -g_src[i].y : g_src[i].y;
                if (ax > 500000.0f || ay > 500000.0f) continue; // скрываем невалидные
                float gain_final = g_user_gain[i] * g_occlusion[i];
                // Расстояние listener→source в game coords
                float dx_ls = g_src[i].x - g_sa_listener.origin.x;
                float dy_ls = g_src[i].y - g_sa_listener.origin.y;
                float dz_ls = g_src[i].z - g_sa_listener.origin.z;
                float dist = sqrtf(dx_ls*dx_ls + dy_ls*dy_ls + dz_ls*dz_ls);
                // [STATIC] если источник не двигался 1+ секунду — кандидат на статую
                const char* tag = (g_src_still_ticks[i] >= STATIC_SRC_TICKS) ? " [STATIC]" : "";
                fprintf(g_log,
                    "  src=%u dist=%.0f pos_game=(%.0f,%.0f,%.0f) pos_sim=(%.0f,%.0f,%.0f) "
                    "occ=%.3f gain=%.2f->%.2f still=%d%s\n",
                    g_src[i].id,
                    (double)dist,
                    (double)g_src[i].x, (double)g_src[i].y, (double)g_src[i].z,
                    (double)g_src[i].x, (double)-g_src[i].y, (double)g_src[i].z,
                    (double)g_occlusion[i],
                    (double)g_user_gain[i], (double)gain_final,
                    g_src_still_ticks[i], tag);
            }
            fflush(g_log);
        }

        Sleep(100); // 10 Hz
    }
    return 0;
}

// Освобождает сцену (вызывается при ошибке и при shutdown)
static void sa_scene_shutdown(void)
{
    g_sim_run = 0;
    if (g_sim_thread) {
        WaitForSingleObject(g_sim_thread, 600);
        CloseHandle(g_sim_thread); g_sim_thread = NULL;
    }
    if (sa_SourceRemove && sa_SourceRelease) {
        for (int i = 0; i < SA_MAX_SRC; i++) {
            if (g_ipl_src[i]) {
                if (g_ipl_sim) sa_SourceRemove(g_ipl_src[i], g_ipl_sim);
                sa_SourceRelease(&g_ipl_src[i]);
            }
        }
    }
    if (g_ipl_sim  && sa_SimulatorRelease)   { sa_SimulatorRelease(&g_ipl_sim); }
    if (g_ipl_mesh && sa_StaticMeshRelease)  { sa_StaticMeshRelease(&g_ipl_mesh); }
    if (g_ipl_scene && sa_SceneRelease)      { sa_SceneRelease(&g_ipl_scene); }
    free(g_mesh_verts); g_mesh_verts = NULL;
    free(g_mesh_tris);  g_mesh_tris  = NULL;
    free(g_mesh_matidx);g_mesh_matidx= NULL;
    free(g_mesh_mats);  g_mesh_mats  = NULL;
    g_scene_ok = 0;
}

// Инициализация сцены — вызывается в конце sa_init() после g_sa_ok=1
static void sa_scene_init(void)
{
    // Инициализируем таблицу occlusion
    for (int i = 0; i < SA_MAX_SRC; i++) {
        g_occlusion[i] = 1.0f;
        g_user_gain[i] = 1.0f;
    }

    // Резолвим функции
    sa_SceneCreate    = (pfn_iplSceneCreate)    sa_getproc("iplSceneCreate");
    sa_SceneRelease   = (pfn_iplSceneRelease)   sa_getproc("iplSceneRelease");
    sa_SceneCommit    = (pfn_iplSceneCommit)    sa_getproc("iplSceneCommit");
    sa_StaticMeshCreate  = (pfn_iplStaticMeshCreate) sa_getproc("iplStaticMeshCreate");
    sa_StaticMeshRelease = (pfn_iplStaticMeshRelease)sa_getproc("iplStaticMeshRelease");
    sa_StaticMeshAdd     = (pfn_iplStaticMeshAdd)    sa_getproc("iplStaticMeshAdd");
    sa_SimulatorCreate   = (pfn_iplSimulatorCreate)  sa_getproc("iplSimulatorCreate");
    sa_SimulatorRelease  = (pfn_iplSimulatorRelease) sa_getproc("iplSimulatorRelease");
    sa_SimulatorSetScene = (pfn_iplSimulatorSetScene)sa_getproc("iplSimulatorSetScene");
    sa_SimulatorCommit   = (pfn_iplSimulatorCommit)  sa_getproc("iplSimulatorCommit");
    sa_SimulatorRunDirect= (pfn_iplSimulatorRunDirect)sa_getproc("iplSimulatorRunDirect");
    sa_SimulatorSetSharedInputs=(pfn_iplSimulatorSetSharedInputs)sa_getproc("iplSimulatorSetSharedInputs");
    sa_SourceCreate   = (pfn_iplSourceCreate)  sa_getproc("iplSourceCreate");
    sa_SourceRelease  = (pfn_iplSourceRelease) sa_getproc("iplSourceRelease");
    sa_SourceAdd      = (pfn_iplSourceAdd)     sa_getproc("iplSourceAdd");
    sa_SourceRemove   = (pfn_iplSourceRemove)  sa_getproc("iplSourceRemove");
    sa_SourceSetInputs  = (pfn_iplSourceSetInputs) sa_getproc("iplSourceSetInputs");
    sa_SourceGetOutputs = (pfn_iplSourceGetOutputs)sa_getproc("iplSourceGetOutputs");

    if (!sa_SceneCreate || !sa_StaticMeshCreate || !sa_StaticMeshAdd ||
        !sa_SceneCommit || !sa_SimulatorCreate  || !sa_SimulatorSetScene ||
        !sa_SimulatorCommit || !sa_SimulatorRunDirect ||
        !sa_SourceCreate || !sa_SourceAdd || !sa_SourceSetInputs || !sa_SourceGetOutputs) {
        if (g_log) { fprintf(g_log,"[SCENE] missing SA funcs — occlusion disabled\n"); fflush(g_log); }
        return;
    }

    // Загружаем OBJ — ищем в нескольких местах
    char obj_buf[MAX_PATH];
    const char* obj_paths[] = {
        "test_wall.obj",
        "de_20_18_offset.obj",
        "oren_scene_dev.obj",
        NULL
    };
    int loaded = 0;
    for (int i = 0; obj_paths[i]; i++) {
        if (g_my_dir[0]) {
            snprintf(obj_buf, MAX_PATH, "%s\\%s", g_my_dir, obj_paths[i]);
            if (load_obj_geometry(obj_buf)) { loaded=1; break; }
        }
        if (load_obj_geometry(obj_paths[i])) { loaded=1; break; }
    }
    if (!loaded) {
        if (g_log) { fprintf(g_log,"[SCENE] oren_scene_dev.obj not found — occlusion disabled\n"); fflush(g_log); }
        return;
    }

    // Создаём IPLScene
    {
        IPLSceneSettings_t ss; memset(&ss, 0, sizeof(ss)); ss.type = 0;
        IPLerror e = sa_SceneCreate(g_sa_ctx, &ss, &g_ipl_scene);
        if (e || !g_ipl_scene) {
            if (g_log) { fprintf(g_log,"[SCENE] SceneCreate err=%d\n",(int)e); fflush(g_log); }
            goto fail;
        }
        if (g_log) { fprintf(g_log,"[SCENE] SceneCreate OK\n"); fflush(g_log); }
    }

    // Создаём IPLStaticMesh
    {
        IPLStaticMeshSettings_t ms;
        ms.numVertices=g_mesh_nverts; ms.numTriangles=g_mesh_ntris; ms.numMaterials=1;
        ms.vertices=g_mesh_verts; ms.triangles=g_mesh_tris;
        ms.materialIndices=g_mesh_matidx; ms.materials=g_mesh_mats;
        IPLerror e = sa_StaticMeshCreate(g_ipl_scene, &ms, &g_ipl_mesh);
        if (e || !g_ipl_mesh) {
            if (g_log) { fprintf(g_log,"[SCENE] StaticMeshCreate err=%d\n",(int)e); fflush(g_log); }
            goto fail;
        }
        sa_StaticMeshAdd(g_ipl_mesh, g_ipl_scene);
        sa_SceneCommit(g_ipl_scene);
        if (g_log) {
            fprintf(g_log,"[SCENE] StaticMesh OK (%d verts, %d tris), SceneCommit OK\n",
                    g_mesh_nverts, g_mesh_ntris); fflush(g_log);
        }
    }

    // Создаём IPLSimulator (только DIRECT — минимальная нагрузка)
    {
        IPLSimulationSettings_t ss; memset(&ss, 0, sizeof(ss));
        ss.flags=1; ss.sceneType=0; ss.reflectionType=1;
        ss.maxNumOcclusionSamples=8; ss.maxNumRays=32; ss.numDiffuseSamples=32;
        ss.maxDuration=1.0f; ss.maxOrder=0; ss.maxNumSources=SA_MAX_SRC;
        ss.numThreads=1; ss.rayBatchSize=16;
        ss.samplingRate=SA_SAMPLE_RATE; ss.frameSize=SA_FRAME_SIZE;
        IPLerror e = sa_SimulatorCreate(g_sa_ctx, &ss, &g_ipl_sim);
        if (e || !g_ipl_sim) {
            if (g_log) { fprintf(g_log,"[SCENE] SimulatorCreate err=%d\n",(int)e); fflush(g_log); }
            goto fail;
        }
        sa_SimulatorSetScene(g_ipl_sim, g_ipl_scene);
        sa_SimulatorCommit(g_ipl_sim);
        if (g_log) { fprintf(g_log,"[SCENE] SimulatorCreate OK\n"); fflush(g_log); }
    }

    // Создаём пул IPLSource
    {
        IPLSourceSettings_t ss; ss.flags = 1; // DIRECT
        int ok=0;
        for (int i = 0; i < SA_MAX_SRC; i++) {
            if (sa_SourceCreate(g_ipl_sim, &ss, &g_ipl_src[i]) == IPL_STATUS_SUCCESS)
                { sa_SourceAdd(g_ipl_src[i], g_ipl_sim); ok++; }
            else g_ipl_src[i] = NULL;
        }
        if (g_log) { fprintf(g_log,"[SCENE] IPLSource pool: %d/%d OK\n",ok,SA_MAX_SRC); fflush(g_log); }
        if (!ok) goto fail;
    }

    g_scene_ok = 1;

    // Запускаем поток симулятора
    g_sim_run = 1;
    g_sim_thread = CreateThread(NULL, 0, sim_thread_proc, NULL, 0, NULL);
    if (g_sim_thread) {
        SetThreadPriority(g_sim_thread, THREAD_PRIORITY_BELOW_NORMAL);
        if (g_log) { fprintf(g_log,"[SCENE] occlusion thread started\n"); fflush(g_log); }
    } else {
        if (g_log) { fprintf(g_log,"[SCENE] WARNING: no thread, occlusion sync\n"); fflush(g_log); }
    }
    if (g_log) { fprintf(g_log,"[SCENE] occlusion ENABLED\n"); fflush(g_log); }
    return;
fail:
    sa_scene_shutdown();
    if (g_log) { fprintf(g_log,"[SCENE] init FAILED — audio OK, occlusion disabled\n"); fflush(g_log); }
}


static void load_config(void)
{
    char inipath[MAX_PATH];
    if (g_my_dir[0])
        snprintf(inipath, MAX_PATH, "%s\\LineageAudio.ini", g_my_dir);
    else
        snprintf(inipath, MAX_PATH, "LineageAudio.ini");

    char buf[256];
    GetPrivateProfileStringA("HRTF", "Enabled", "1", buf, sizeof(buf), inipath);
    cfg_hrtf_enabled = atoi(buf);
    GetPrivateProfileStringA("HRTF", "SpatialBlend", "1.0", buf, sizeof(buf), inipath);
    cfg_hrtf_blend = (float)atof(buf);
    GetPrivateProfileStringA("HRTF", "VolumeBoost", "1.0", buf, sizeof(buf), inipath);
    cfg_volume_boost = (float)atof(buf);
    GetPrivateProfileStringA("HRTF", "GainMaster", "1.0", buf, sizeof(buf), inipath);
    cfg_gain_master = (float)atof(buf);
    GetPrivateProfileStringA("Direct", "Enabled", "1", buf, sizeof(buf), inipath);
    cfg_direct_effect = atoi(buf);
    GetPrivateProfileStringA("Direct", "DistAtten", "1.0", buf, sizeof(buf), inipath);
    cfg_dist_atten = (float)atof(buf);
    GetPrivateProfileStringA("Direct", "AirAbsorb", "0.0", buf, sizeof(buf), inipath);
    cfg_air_absorb = (float)atof(buf);
    GetPrivateProfileStringA("Direct", "MinDistance", "1.0", buf, sizeof(buf), inipath);
    cfg_min_distance = (float)atof(buf);
    GetPrivateProfileStringA("Direct", "Occlusion", "1.0", buf, sizeof(buf), inipath);
    cfg_occlusion = (float)atof(buf);
    GetPrivateProfileStringA("HUD", "Enabled", "1", buf, sizeof(buf), inipath);
    cfg_hud = atoi(buf);

    if (g_log) {
        fprintf(g_log, "[CFG] %s\n", inipath);
        fprintf(g_log, "[CFG] HRTF: enabled=%d blend=%.2f boost=%.2f\n", cfg_hrtf_enabled, cfg_hrtf_blend, cfg_volume_boost);
        fprintf(g_log, "[CFG] Direct: enabled=%d dist=%.2f air=%.2f min=%.1f occ=%.2f\n", cfg_direct_effect, cfg_dist_atten, cfg_air_absorb, cfg_min_distance, cfg_occlusion);
        fprintf(g_log, "[CFG] HUD=%d gain=%.2f\n", cfg_hud, cfg_gain_master);
        fflush(g_log);
    }
}


static void sa_init(void)
{
    load_config();
    char path_buf[MAX_PATH];
    const char* names[] = { "phonon.dll", "steam_audio\\phonon.dll", NULL };
    for (int i = 0; names[i]; i++) {
        if (g_my_dir[0]) {
            snprintf(path_buf, MAX_PATH, "%s\\%s", g_my_dir, names[i]);
            g_phonon = LoadLibraryA(path_buf);
            if (g_phonon) { if (g_log) { fprintf(g_log,"[SA] loaded: %s\n", path_buf); fflush(g_log); } break; }
        }
        g_phonon = LoadLibraryA(names[i]);
        if (g_phonon) { if (g_log) { fprintf(g_log,"[SA] loaded: %s\n", names[i]); fflush(g_log); } break; }
    }
    if (!g_phonon) {
        if (g_log) { fprintf(g_log,"[SA] phonon.dll not found — passthrough\n"); fflush(g_log); }
        return;
    }

    SA_LOAD(ContextCreate) SA_LOAD(ContextRelease)
    SA_LOAD(HRTFCreate) SA_LOAD(HRTFRelease)

    // Load Direct/Binaural effect + AudioBuffer functions
    sa_DirectEffectCreate = (pfn_iplDirectEffectCreate)sa_getproc("iplDirectEffectCreate");
    sa_DirectEffectRelease = (pfn_iplDirectEffectRelease)sa_getproc("iplDirectEffectRelease");
    sa_DirectEffectApply = (pfn_iplDirectEffectApply)sa_getproc("iplDirectEffectApply");
    sa_DirectEffectReset = (pfn_iplDirectEffectReset)sa_getproc("iplDirectEffectReset");
    sa_BinauralEffectCreate = (pfn_iplBinauralEffectCreate)sa_getproc("iplBinauralEffectCreate");
    sa_BinauralEffectRelease = (pfn_iplBinauralEffectRelease)sa_getproc("iplBinauralEffectRelease");
    sa_BinauralEffectApply = (pfn_iplBinauralEffectApply)sa_getproc("iplBinauralEffectApply");
    sa_BinauralEffectReset = (pfn_iplBinauralEffectReset)sa_getproc("iplBinauralEffectReset");
    sa_AudioBufferAllocate = (pfn_iplAudioBufferAllocate)sa_getproc("iplAudioBufferAllocate");
    sa_AudioBufferFree = (pfn_iplAudioBufferFree)sa_getproc("iplAudioBufferFree");
    sa_AudioBufferInterleave = (pfn_iplAudioBufferInterleave)sa_getproc("iplAudioBufferInterleave");
    sa_AudioBufferDeinterleave = (pfn_iplAudioBufferDeinterleave)sa_getproc("iplAudioBufferDeinterleave");
    sa_AudioBufferMix = (pfn_iplAudioBufferMix)sa_getproc("iplAudioBufferMix");
    sa_CalculateRelativeDirection = (pfn_iplCalculateRelativeDirection)sa_getproc("iplCalculateRelativeDirection");

    if (g_log) {
        fprintf(g_log, "[SA] DirectEffect: create=%p release=%p apply=%p reset=%p\n",
            (void*)sa_DirectEffectCreate, (void*)sa_DirectEffectRelease,
            (void*)sa_DirectEffectApply, (void*)sa_DirectEffectReset);
        fprintf(g_log, "[SA] BinauralEffect: create=%p release=%p apply=%p reset=%p\n",
            (void*)sa_BinauralEffectCreate, (void*)sa_BinauralEffectRelease,
            (void*)sa_BinauralEffectApply, (void*)sa_BinauralEffectReset);
        fprintf(g_log, "[SA] AudioBuffer: alloc=%p free=%p interleave=%p deinterleave=%p mix=%p\n",
            (void*)sa_AudioBufferAllocate, (void*)sa_AudioBufferFree,
            (void*)sa_AudioBufferInterleave, (void*)sa_AudioBufferDeinterleave,
            (void*)sa_AudioBufferMix);
        fprintf(g_log, "[SA] CalcRelDir: %p\n", (void*)sa_CalculateRelativeDirection);
        fflush(g_log);
    }

    // sizeof диагностика — проверка layout структур
    if (g_log) {
        fprintf(g_log, "[SA] sizeof: IPLContextSettings=%d\n", (int)sizeof(IPLContextSettings));
        fprintf(g_log, "[SA] sizeof: IPLHRTFSettings=%d\n", (int)sizeof(IPLHRTFSettings));
        fprintf(g_log, "[SA] sizeof: IPLStaticMeshSettings=%d\n", (int)sizeof(IPLStaticMeshSettings_t));
        fprintf(g_log, "[SA] sizeof: IPLSimulationSettings=%d\n", (int)sizeof(IPLSimulationSettings_t));
        fprintf(g_log, "[SA] sizeof: IPLSimulationSharedInputs=%d\n", (int)sizeof(IPLSimulationSharedInputs_t));
        fprintf(g_log, "[SA] sizeof: IPLSimulationInputs=%d (expected 184)\n", (int)sizeof(IPLSimulationInputs_t));
        fprintf(g_log, "[SA] sizeof: IPLDirectEffectParams=%d\n", (int)sizeof(IPLDirectEffectParams_t));
        fprintf(g_log, "[SA] sizeof: IPLSimulationOutputs=%d\n", (int)sizeof(IPLSimulationOutputs_t));
        fprintf(g_log, "[SA] sizeof: IPLCoordinateSpace3=%d\n", (int)sizeof(IPLCoordinateSpace3));
        fprintf(g_log, "[SA] sizeof: IPLSourceSettings=%d\n", (int)sizeof(IPLSourceSettings_t));
        // Детальная диагностика смещений IPLSimulationInputs_t
        IPLSimulationInputs_t _diag;
        memset(&_diag, 0, sizeof(_diag));
        fprintf(g_log, "[SA] IPLSimulationInputs_t offsets: flags=%d directFlags=%d source.origin=%d occlusionType=%d occlusionRadius=%d numSamples=%d deviationModel=%d\n",
            (int)((char*)&_diag.flags - (char*)&_diag),
            (int)((char*)&_diag.directFlags - (char*)&_diag),
            (int)((char*)&_diag.source.origin - (char*)&_diag),
            (int)((char*)&_diag.occlusionType - (char*)&_diag),
            (int)((char*)&_diag.occlusionRadius - (char*)&_diag),
            (int)((char*)&_diag.numOcclusionSamples - (char*)&_diag),
            (int)((char*)&_diag.deviationModel - (char*)&_diag));
        fflush(g_log);
    }

    // Контекст
    {
        IPLContextSettings cs;
        memset(&cs, 0, sizeof(cs));
        cs.version   = (4 << 16) | (4 << 8) | 0;
        cs.simdLevel = 0;
        IPLerror ctx_err = sa_ContextCreate(&cs, &g_sa_ctx);
        if (ctx_err != IPL_STATUS_SUCCESS) {
            fprintf(g_log,"[SA] ContextCreate err=%d\n", (int)ctx_err); fflush(g_log); goto sa_fail;
        }
        if (g_log) { fprintf(g_log,"[SA] ContextCreate OK ctx=%p\n", (void*)g_sa_ctx); fflush(g_log); }
    }

    // HRTF (нужен IPLSimulator)
    {
        IPLAudioSettings as = { SA_SAMPLE_RATE, SA_FRAME_SIZE };
        IPLHRTFSettings hs;
        memset(&hs, 0, sizeof(hs));
        hs.type    = IPL_HRTFTYPE_DEFAULT;
        hs.volume  = 1.0f;
        hs.normType = 0;
        IPLerror e = sa_HRTFCreate(g_sa_ctx, &as, &hs, &g_sa_hrtf);
        if (e != IPL_STATUS_SUCCESS) {
            fprintf(g_log,"[SA] HRTFCreate err=%d\n", (int)e); fflush(g_log); goto sa_fail;
        }
        if (g_log) { fprintf(g_log,"[SA] HRTFCreate OK\n"); fflush(g_log); }
    }

    g_sa_ok = 1;
    if (g_log) { fprintf(g_log,"[SA] initialized OK\n"); fflush(g_log); }

    // ── SA Direct + Binaural effects init ──────────────────────────────────
    if (sa_DirectEffectCreate && sa_BinauralEffectCreate &&
        sa_AudioBufferAllocate && sa_AudioBufferInterleave && sa_AudioBufferDeinterleave &&
        sa_CalculateRelativeDirection) {
        IPLAudioSettings as = { SA_SAMPLE_RATE, SA_FRAME_SIZE };

        // DirectEffect — 1 channel (mono input)
        {
            IPLDirectEffectSettings ds; ds.numChannels = 1;
            IPLerror e = sa_DirectEffectCreate(g_sa_ctx, &as, &ds, &g_direct_effect);
            if (e || !g_direct_effect) {
                if (g_log) { fprintf(g_log,"[EFFECT] DirectEffectCreate err=%d\n",(int)e); fflush(g_log); }
            } else {
                if (g_log) { fprintf(g_log,"[EFFECT] DirectEffectCreate OK\n"); fflush(g_log); }
            }
        }

        // BinauralEffect — uses HRTF
        {
            IPLBinauralEffectSettings bs; bs.hrtf = g_sa_hrtf;
            IPLerror e = sa_BinauralEffectCreate(g_sa_ctx, &as, &bs, &g_binaural_effect);
            if (e || !g_binaural_effect) {
                if (g_log) { fprintf(g_log,"[EFFECT] BinauralEffectCreate err=%d\n",(int)e); fflush(g_log); }
            } else {
                if (g_log) { fprintf(g_log,"[EFFECT] BinauralEffectCreate OK\n"); fflush(g_log); }
            }
        }

        // Audio buffers
        if (g_direct_effect && g_binaural_effect) {
            sa_AudioBufferAllocate(g_sa_ctx, 1, SA_FRAME_SIZE, &g_buf_in_mono);
            sa_AudioBufferAllocate(g_sa_ctx, 1, SA_FRAME_SIZE, &g_buf_direct);
            sa_AudioBufferAllocate(g_sa_ctx, 2, SA_FRAME_SIZE, &g_buf_binaural);
            g_buf_interleaved = (IPLfloat32*)malloc(2 * SA_FRAME_SIZE * sizeof(IPLfloat32));
            g_buf_pcm16 = (short*)malloc(2 * SA_FRAME_SIZE * sizeof(short));
            if (g_buf_in_mono.data && g_buf_direct.data && g_buf_binaural.data && g_buf_interleaved && g_buf_pcm16) {
                g_effects_ok = 1;
                if (g_log) { fprintf(g_log,"[EFFECT] audio buffers OK (1ch in, 1ch direct, 2ch binaural, interleaved)\n"); fflush(g_log); }
            } else {
                if (g_log) { fprintf(g_log,"[EFFECT] audio buffer alloc FAILED\n"); fflush(g_log); }
            }
        }

        if (!g_effects_ok) {
            if (g_log) { fprintf(g_log,"[EFFECT] effects init FAILED — HRTF pipeline disabled\n"); fflush(g_log); }
        }
    } else {
        if (g_log) { fprintf(g_log,"[EFFECT] missing SA effect functions — HRTF pipeline disabled\n"); fflush(g_log); }
    }

    sa_scene_init();
    return;

sa_fail:
    g_sa_ok = 0;
    if (g_log) { fprintf(g_log,"[SA] disabled\n"); fflush(g_log); }
}

static void log_write(const char* fmt, ...);

static void sa_shutdown(void)
{
    if (!g_phonon) return;

    // Stop test audio thread
    g_test_audio_run = 0;
    if (g_test_audio_thread) {
        WaitForSingleObject(g_test_audio_thread, 2000);
        CloseHandle(g_test_audio_thread); g_test_audio_thread = NULL;
    }
    g_test_audio_active = 0;

    // Free SA effects
    if (g_effects_ok) {
        if (sa_AudioBufferFree) {
            sa_AudioBufferFree(g_sa_ctx, &g_buf_binaural);
            sa_AudioBufferFree(g_sa_ctx, &g_buf_direct);
            sa_AudioBufferFree(g_sa_ctx, &g_buf_in_mono);
        }
        free(g_buf_pcm16); g_buf_pcm16 = NULL;
        free(g_buf_interleaved); g_buf_interleaved = NULL;
        if (g_binaural_effect && sa_BinauralEffectRelease) sa_BinauralEffectRelease(&g_binaural_effect);
        if (g_direct_effect && sa_DirectEffectRelease) sa_DirectEffectRelease(&g_direct_effect);
        g_effects_ok = 0;
    }

    sa_scene_shutdown();
    if (g_sa_hrtf) sa_HRTFRelease(&g_sa_hrtf);
    if (g_sa_ctx) sa_ContextRelease(&g_sa_ctx);
    FreeLibrary(g_phonon); g_phonon = NULL;
    g_sa_ok = 0;
}

// ── Test tone generator + SA audio processing thread ──────────────────────

// Forward declaration (defined below after AL constants)
static void log_write(const char* fmt, ...);

#define AL_FORMAT_STEREO16 0x1103

static void generate_tone_frame(float* out, int numSamples, float freq, float sampleRate, float* phase)
{
    float phaseInc = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    for (int i = 0; i < numSamples; i++) {
        out[i] = 0.25f * sinf(*phase);
        *phase += phaseInc;
        if (*phase > 2.0f * 3.14159265358979323846f) *phase -= 2.0f * 3.14159265358979323846f;
    }
}

static DWORD WINAPI test_audio_thread_proc(LPVOID)
{
    if (g_log) { fprintf(g_log,"[TEST_AUDIO] thread started\n"); fflush(g_log); }

    typedef void (__cdecl *pfn_alGenSources)(int, unsigned int*);
    typedef void (__cdecl *pfn_alGenBuffers)(int, unsigned int*);
    typedef void (__cdecl *pfn_alSourcei)(unsigned int, int, int);
    typedef void (__cdecl *pfn_alSourcef)(unsigned int, int, float);
    typedef void (__cdecl *pfn_alSourcePlay)(unsigned int);
    typedef void (__cdecl *pfn_alBufferData)(unsigned int, int, const void*, int, int);
    typedef int (__cdecl *pfn_alGetError)(void);

    typedef int (__cdecl *pfn_alGetSourcei)(unsigned int, int, int*);

    pfn_alGenSources fnGenSrc = (pfn_alGenSources)GetProcAddress(g_orig, "alGenSources");
    pfn_alGenBuffers fnGenBuf = (pfn_alGenBuffers)GetProcAddress(g_orig, "alGenBuffers");
    pfn_alSourcei fnSrcI = (pfn_alSourcei)GetProcAddress(g_orig, "alSourcei");
    pfn_alSourcef fnSrcF = (pfn_alSourcef)GetProcAddress(g_orig, "alSourcef");
    pfn_alSourcePlay fnPlay = (pfn_alSourcePlay)GetProcAddress(g_orig, "alSourcePlay");
    pfn_alBufferData fnBufData = (pfn_alBufferData)GetProcAddress(g_orig, "alBufferData");
    pfn_alGetError fnErr = (pfn_alGetError)GetProcAddress(g_orig, "alGetError");

    if (!fnGenSrc || !fnGenBuf || !fnSrcI || !fnPlay || !fnBufData) {
        if (g_log) { fprintf(g_log,"[TEST_AUDIO] missing OpenAL functions\n"); fflush(g_log); }
        return 0;
    }

    fnGenSrc(1, &g_test_al_src);
    fnGenBuf(1, g_test_al_buf);
    if (fnErr) fnErr();

    fnSrcI(g_test_al_src, 0x202, 1);
    fnSrcF(g_test_al_src, 0x100A, 0.5f);

    if (g_log) { fprintf(g_log,"[TEST_AUDIO] AL source=%u buf=%u\n", g_test_al_src, g_test_al_buf[0]); fflush(g_log); }

    IPLVector3 test_src_pos = { 2.0f, 0.0f, -5.0f };

    int loop_frames = (int)(SA_SAMPLE_RATE * 10 / SA_FRAME_SIZE);
    int pcm16_total = loop_frames * SA_FRAME_SIZE * 2;
    short* loop_buf = (short*)HeapAlloc(GetProcessHeap(), 0, pcm16_total * sizeof(short));
    if (!loop_buf) {
        if (g_log) { fprintf(g_log,"[TEST_AUDIO] alloc FAILED\n"); fflush(g_log); }
        return 0;
    }

    if (g_log) { fprintf(g_log,"[TEST_AUDIO] generating %d frames (~1sec)...\n", loop_frames); fflush(g_log); }

    float phase = 0.0f;
    float* f_mono = (float*)HeapAlloc(GetProcessHeap(), 0, SA_FRAME_SIZE * sizeof(float));
    float* f_direct = (float*)HeapAlloc(GetProcessHeap(), 0, SA_FRAME_SIZE * sizeof(float));
    float* f_bin_l = (float*)HeapAlloc(GetProcessHeap(), 0, SA_FRAME_SIZE * sizeof(float));
    float* f_bin_r = (float*)HeapAlloc(GetProcessHeap(), 0, SA_FRAME_SIZE * sizeof(float));
    float* f_il = (float*)HeapAlloc(GetProcessHeap(), 0, SA_FRAME_SIZE * 2 * sizeof(float));
    if (!f_mono || !f_direct || !f_bin_l || !f_bin_r || !f_il) {
        if (g_log) { fprintf(g_log,"[TEST_AUDIO] frame alloc FAILED\n"); fflush(g_log); }
        return 0;
    }

    for (int f = 0; f < loop_frames && g_test_audio_run; f++) {
        generate_tone_frame(f_mono, SA_FRAME_SIZE, 440.0f, (float)SA_SAMPLE_RATE, &phase);

        if (g_direct_effect && sa_DirectEffectApply) {
            IPLAudioBuffer in_b = {1, SA_FRAME_SIZE, &f_mono};
            IPLAudioBuffer out_b = {1, SA_FRAME_SIZE, &f_direct};
            IPLDirectEffectParams dp;
            memset(&dp, 0, sizeof(dp));
            dp.flags = (IPLDirectEffectFlags)(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
            dp.transmissionType = IPL_TRANSMISSIONTYPE_FREQINDEPENDENT;
            float dx = test_src_pos.x - g_sa_listener.origin.x;
            float dy = test_src_pos.y - g_sa_listener.origin.y;
            float dz = test_src_pos.z - g_sa_listener.origin.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist < 1.0f) dist = 1.0f;
            dp.distanceAttenuation = 1.0f / dist;
            dp.airAbsorption[0] = 1.0f; dp.airAbsorption[1] = 1.0f; dp.airAbsorption[2] = 1.0f;
            dp.occlusion = 1.0f; dp.directivity = 1.0f;
            dp.transmission[0] = 1.0f; dp.transmission[1] = 1.0f; dp.transmission[2] = 1.0f;
            sa_DirectEffectApply(g_direct_effect, &dp, &in_b, &out_b);
        } else {
            memcpy(f_direct, f_mono, SA_FRAME_SIZE * sizeof(float));
        }

        if (g_binaural_effect && sa_BinauralEffectApply && sa_CalculateRelativeDirection) {
            IPLAudioBuffer in_b = {1, SA_FRAME_SIZE, &f_direct};
            float* ch[2] = {f_bin_l, f_bin_r};
            IPLAudioBuffer out_b = {2, SA_FRAME_SIZE, ch};
            IPLBinauralEffectParams bp;
            memset(&bp, 0, sizeof(bp));
            bp.interpolation = IPL_HRTFINTERP_BILINEAR;
            bp.spatialBlend = 1.0f;
            bp.hrtf = g_sa_hrtf;
            bp.peakDelays = NULL;
            bp.direction = sa_CalculateRelativeDirection(g_sa_ctx,
                test_src_pos, g_sa_listener.origin, g_sa_listener.ahead, g_sa_listener.up);
            sa_BinauralEffectApply(g_binaural_effect, &bp, &in_b, &out_b);
        } else {
            memcpy(f_bin_l, f_direct, SA_FRAME_SIZE * sizeof(float));
            memcpy(f_bin_r, f_direct, SA_FRAME_SIZE * sizeof(float));
        }

        for (int i = 0; i < SA_FRAME_SIZE; i++) {
            f_il[i*2] = f_bin_l[i];
            f_il[i*2+1] = f_bin_r[i];
        }

        short* dst = loop_buf + f * SA_FRAME_SIZE * 2;
        for (int i = 0; i < SA_FRAME_SIZE * 2; i++) {
            float s = f_il[i];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            dst[i] = (short)(s * 32767.0f);
        }

        if (g_log && f % 25 == 0) {
            fprintf(g_log,"[TEST_AUDIO] gen frame %d/%d\n", f, loop_frames);
            fflush(g_log);
        }
    }

    HeapFree(GetProcessHeap(), 0, f_mono);
    HeapFree(GetProcessHeap(), 0, f_direct);
    HeapFree(GetProcessHeap(), 0, f_bin_l);
    HeapFree(GetProcessHeap(), 0, f_bin_r);
    HeapFree(GetProcessHeap(), 0, f_il);

    fnBufData(g_test_al_buf[0], AL_FORMAT_STEREO16, loop_buf, pcm16_total * sizeof(short), SA_SAMPLE_RATE);
    fnSrcI(g_test_al_src, 0x1009, g_test_al_buf[0]);
    fnSrcI(g_test_al_src, 0x1005, 1);
    fnPlay(g_test_al_src);

    if (g_log) { fprintf(g_log,"[TEST_AUDIO] playing 10sec src=%u buf=%u\n", g_test_al_src, g_test_al_buf[0]); fflush(g_log); }

    int loop_ticks = 0;
    while (g_test_audio_run && g_effects_ok) {
        int state = 0;
        pfn_alGetSourcei fnGetSrcI2 = (pfn_alGetSourcei)GetProcAddress(g_orig, "alGetSourcei");
        if (fnGetSrcI2) fnGetSrcI2(g_test_al_src, 0x1010, &state);
        if (loop_ticks % 30 == 0 && g_log) {
            fprintf(g_log,"[TEST_AUDIO] alive tick=%d state=0x%X\n", loop_ticks, state);
            fflush(g_log);
        }
        if (state == 0x1014 && fnPlay) {
            fnPlay(g_test_al_src);
            if (g_log) { fprintf(g_log,"[TEST_AUDIO] restart at tick=%d\n", loop_ticks); fflush(g_log); }
        }
        loop_ticks++;
        Sleep(100);
    }

    HeapFree(GetProcessHeap(), 0, loop_buf);
    if (g_log) { fprintf(g_log,"[TEST_AUDIO] thread stopped\n"); fflush(g_log); }
    return 0;
}

// Start test audio thread (called from alSourcePlay lazy init)
static void start_test_audio(void)
{
    if (!g_effects_ok || g_test_audio_active) return;
    g_test_audio_run = 1;
    g_test_audio_thread = CreateThread(NULL, 0, test_audio_thread_proc, NULL, 0, NULL);
    if (g_test_audio_thread) {
        g_test_audio_active = 1;
        if (g_log) { fprintf(g_log,"[TEST_AUDIO] thread created\n"); fflush(g_log); }
    }
}
#define AL_POSITION 0x1004
#define AL_ORIENTATION 0x100F
#define AL_BUFFER 0x1009
#define AL_SOURCE_RELATIVE 0x202
#define AL_TRUE 1
#define AL_FALSE 0
#define AL_NO_ERROR 0
#define AL_INVALID_VALUE 0xA004
typedef int ALenum;

// Пороги дедупликации
// SRC_POS: логируем если источник сдвинулся больше чем на N единиц
#define POS_THRESHOLD    50.0f
// LST_POS: логируем если слушатель сдвинулся больше чем на N единиц
#define LST_POS_THRESHOLD 10.0f
// LST_ORI: логируем если forward-вектор изменился больше чем на N (в единицах dot-расстояния)
#define ORI_THRESHOLD    0.01f

// Кеш позиций источников — определён выше (перед sa_scene_init)

static SrcState* find_src(unsigned int id) {
    for (int i = 0; i < MAX_SOURCES; i++)
        if (g_src[i].active && g_src[i].id == id) return &g_src[i];
    return NULL;
}
static SrcState* alloc_src(unsigned int id) {
    for (int i = 0; i < MAX_SOURCES; i++)
        if (!g_src[i].active) {
            g_src[i].id       = id;
            g_src[i].active   = 1;
            g_src[i].relative = 0;
            g_src[i].x = g_src[i].y = g_src[i].z = 1e9f;
            return &g_src[i];
        }
    return NULL;
}

// Индекс слота — реализация (forward decl выше)
static int sa_src_slot(unsigned int id) {
    for (int i = 0; i < MAX_SOURCES; i++)
        if (g_src[i].active && g_src[i].id == id) return i;
    return -1;
}

static BufInfo* find_buf(unsigned int id) {
    for (int i = 0; i < g_buf_count; i++) {
        if (g_buf[i].active && g_buf[i].id == id) return &g_buf[i];
    }
    return NULL;
}
static BufInfo* alloc_buf(unsigned int id) {
    for (int i = 0; i < g_buf_count; i++) {
        if (!g_buf[i].active) {
            memset(&g_buf[i], 0, sizeof(BufInfo));
            g_buf[i].id = id;
            g_buf[i].active = 1;
            return &g_buf[i];
        }
    }
    if (g_buf_count < MAX_BUFFERS) {
        BufInfo* bi = &g_buf[g_buf_count++];
        memset(bi, 0, sizeof(BufInfo));
        bi->id = id;
        bi->active = 1;
        return bi;
    }
    return NULL;
}

// Кеш позиции/ориентации слушателя
static float g_lst_x = 1e9f, g_lst_y = 1e9f, g_lst_z = 1e9f;
static float g_lst_fx = 1e9f, g_lst_fy = 1e9f, g_lst_fz = 1e9f; // forward

static void log_write(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static inline float fdist(float ax,float ay,float az,float bx,float by,float bz) {
    float dx=ax-bx, dy=ay-by, dz=az-bz;
    return sqrtf(dx*dx+dy*dy+dz*dz);
}

// ─── EAX 3.0 extension ────────────────────────────────────────────────────
// ALAudio.dll загружает EAXSet/EAXGet через alGetProcAddress("EAXSet"/"EAXGet")
// после alIsExtensionPresent("EAX3.0"). Если EAXSet=NULL — весь EAX путь отключен.
//
// EAX property IDs used by ALAudio.dll (из реверса):
//   EAXSOURCE_OCCLUSIONDIRECTFACTOR = 0x0B (float, -10000..0 mB)
//   EAXSOURCE_OUTSIDEVOLUMEHF       = 0x05 (float, -10000..0 mB)
//   EAXLISTENER_ALLPARAMETERS       = 0x01 (112 bytes struct)
//
// EAX occlusion factor: 0 = no occlusion, -10000 = fully occluded
// Конвертация: eax_occ mB → linear: 10^(eax_occ/2000), затем gain *= linear

static const GUID EAX_GUID_LISTENER = 
    {0xA8FA6882, 0xB476, 0x11D3, {0xBD,0xB9,0x00,0xC0,0xF0,0x2D,0xDF,0x87}};
static const GUID EAX_GUID_SOURCE = 
    {0xA8FA6881, 0xB476, 0x11D3, {0xBD,0xB9,0x00,0xC0,0xF0,0x2D,0xDF,0x87}};

#define EAXLISTENER_ALLPARAMETERS       0x01
#define EAXSOURCE_OUTSIDEVOLUMEHF       0x05
#define EAXSOURCE_OCCLUSIONDIRECTFACTOR 0x0B

// EAX source occlusion state per AL source — перехватывается из EAXSet
#define EAX_MAX_SOURCES 128
static float g_eax_src_occlusion[EAX_MAX_SOURCES]; // linear 0..1 (0=occluded, 1=open)
static unsigned int g_eax_src_id[EAX_MAX_SOURCES]; // AL source ID mapping
static int g_eax_src_active[EAX_MAX_SOURCES]; // slot occupied?
static volatile long g_eax_set_count = 0;
static volatile long g_eax_set_occl_count = 0;
static volatile long g_eax_set_outsidehf_count = 0;
static volatile long g_eax_set_outsidehf_nonzero = 0;
static volatile long g_eax_set_listener_count = 0;

static int eax_find_slot(unsigned int al_src) {
    for (int i = 0; i < EAX_MAX_SOURCES; i++)
        if (g_eax_src_active[i] && g_eax_src_id[i] == al_src) return i;
    return -1;
}
static int eax_alloc_slot(unsigned int al_src) {
    int s = eax_find_slot(al_src);
    if (s >= 0) return s;
    for (int i = 0; i < EAX_MAX_SOURCES; i++)
        if (!g_eax_src_active[i]) {
            g_eax_src_id[i] = al_src;
            g_eax_src_active[i] = 1;
            g_eax_src_occlusion[i] = 1.0f;
            return i;
        }
    return -1;
}

// EAXSet — вызывается ALAudio.dll через указатель полученный от alGetProcAddress
// EAXSet(const GUID* guid, unsigned long property, unsigned long source, void* value, unsigned long size)
static ALenum __cdecl eax_set(const GUID* guid, unsigned long property,
    unsigned long source, void* value, unsigned long size)
{
    if (!guid || !value) return AL_INVALID_VALUE;
    InterlockedIncrement(&g_eax_set_count);

    if (memcmp(guid, &EAX_GUID_SOURCE, sizeof(GUID)) == 0) {
        // Source properties
        if (property == EAXSOURCE_OCCLUSIONDIRECTFACTOR && size == 4) {
            InterlockedIncrement(&g_eax_set_occl_count);
            float mb = *(float*)value;
            if (mb > 0.0f) mb = 0.0f;
            if (mb < -10000.0f) mb = -10000.0f;
            float linear = powf(10.0f, mb / 2000.0f);

            int slot = eax_alloc_slot((unsigned int)source);
            if (slot >= 0) {
                g_eax_src_occlusion[slot] = linear;
                if (g_log && linear < 0.95f) {
                    log_write("EAX_SET src=%lu prop=OCCLUSION mB=%.0f linear=%.4f", source, mb, linear);
                }
            }
            return AL_NO_ERROR;
        }
        else if (property == EAXSOURCE_OUTSIDEVOLUMEHF && size == 4) {
            InterlockedIncrement(&g_eax_set_outsidehf_count);
            float mb = *(float*)value;
            if (mb < -1.0f) InterlockedIncrement(&g_eax_set_outsidehf_nonzero);
            if (g_log && mb < -1.0f) {
                log_write("EAX_SET src=%lu prop=OUTSIDEVOLUMEHF mB=%.0f", source, mb);
            }
            return AL_NO_ERROR;
        }
    }
    else if (memcmp(guid, &EAX_GUID_LISTENER, sizeof(GUID)) == 0) {
        InterlockedIncrement(&g_eax_set_listener_count);
        if (property == EAXLISTENER_ALLPARAMETERS && size == 0x70) {
            if (g_log) {
                log_write("EAX_SET prop=LISTENER_ALLPARAMETERS size=%lu (no-op)", size);
            }
            return AL_NO_ERROR;
        }
        else if (property == 2 && size == 4) {
            unsigned long env = *(unsigned long*)value;
            if (g_log) {
                log_write("EAX_SET prop=LISTENER_ENVIRONMENT=%lu (no-op)", env);
            }
            return AL_NO_ERROR;
        }
    }

    if (g_log) {
        log_write("EAX_SET unhandled guid=%08X prop=%lu src=%lu size=%lu",
                  guid->Data1, property, source, size);
    }
    return AL_NO_ERROR;
}

// EAXGet — ALAudio.dll загружает но НИКОГДА не вызывает
// Заглушка возвращает AL_NO_ERROR для всех запросов
static ALenum __cdecl eax_get(const GUID* guid, unsigned long property,
                              unsigned long source, void* value, unsigned long size)
{
    if (!guid || !value) return AL_INVALID_VALUE;

    // Возвращаем нули для любых запросов
    memset(value, 0, size);

    if (memcmp(guid, &EAX_GUID_SOURCE, sizeof(GUID)) == 0) {
        if (property == EAXSOURCE_OCCLUSIONDIRECTFACTOR && size == 4) {
            int slot = eax_find_slot((unsigned int)source);
            if (slot >= 0) {
                // Конвертация linear → mB: mB = 2000 * log10(linear)
                float linear = g_eax_src_occlusion[slot];
                if (linear <= 0.0f) linear = 0.00001f;
                float mb = 2000.0f * log10f(linear);
                if (mb < -10000.0f) mb = -10000.0f;
                *(float*)value = mb;
            } else {
                *(float*)value = 0.0f; // no occlusion
            }
            return AL_NO_ERROR;
        }
    }

    if (g_log) {
        log_write("EAX_GET stub guid=%08X prop=%lu src=%lu size=%lu",
                  guid->Data1, property, source, size);
    }
    return AL_NO_ERROR;
}

// Тихий pass-through — без лога
#define FWD(ret, name, params, args) \
    extern "C" __declspec(dllexport) ret __cdecl name params { \
        typedef ret (__cdecl *PFN) params; \
        PFN pfn = (PFN)GetProcAddress(g_orig, #name); \
        if (pfn) return pfn args; \
        return (ret)0; \
    }
#define FWD_VOID(name, params, args) \
    extern "C" __declspec(dllexport) void __cdecl name params { \
        typedef void (__cdecl *PFN) params; \
        PFN pfn = (PFN)GetProcAddress(g_orig, #name); \
        if (pfn) pfn args; \
    }

// ── ALC (все тихо) ─────────────────────────────────────────────────────────
// alcOpenDevice — логируем имя устройства (EAX путь использует "DirectSound3D")
extern "C" __declspec(dllexport) void* __cdecl alcOpenDevice(const char* d)
{
    typedef void* (__cdecl *PFN)(const char*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alcOpenDevice");
    void* result = pfn ? pfn(d) : NULL;
    if (g_log) log_write("alcOpenDevice(\"%s\") → %p", d ? d : "(null)", result);
    return result;
}
FWD(int,   alcCloseDevice,         (void* d), (d))

// alcCreateContext — просто форвард (Creative Labs OpenAL не поддерживает ALC_SOFT_HRTF)
extern "C" __declspec(dllexport) void* __cdecl alcCreateContext(void* d, const int* a)
{
    typedef void* (__cdecl *PFN)(void*, const int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alcCreateContext");
    if (!pfn) return NULL;

    // Creative Labs OpenAL не поддерживает ALC_SOFT_HRTF — просто форвардим
    void* ctx = pfn(d, a);
    if (g_log) {
        // Проверим что поддерживает оригинальный OpenAL
        typedef int (__cdecl *pfn_alcIsExt)(void*, const char*);
        pfn_alcIsExt fnIsExt = (pfn_alcIsExt)GetProcAddress(g_orig, "alcIsExtensionPresent");
        int hrtf = fnIsExt && fnIsExt(d, "ALC_SOFT_HRTF");
        fprintf(g_log, "[ALC] alcCreateContext ctx=%p HRTF=%s\n", ctx, hrtf ? "yes" : "no");
        fflush(g_log);
    }
    return ctx;
}
FWD(int,   alcMakeContextCurrent,  (void* c), (c))
FWD_VOID(  alcDestroyContext,      (void* c), (c))
FWD(void*, alcGetCurrentContext,   (void), ())
FWD(void*, alcGetContextsDevice,   (void* c), (c))
FWD(int,   alcIsExtensionPresent,  (void* d, const char* e), (d,e))
// alcGetProcAddress — ALAudio.dll вызывает alcGetProcAddress(device,"EAXSet")
// а НЕ alGetProcAddress("EAXSet") — это критически важно!
extern "C" __declspec(dllexport) void* __cdecl alcGetProcAddress(void* d, const char* f)
{
    if (f && strcmp(f, "EAXSet") == 0) {
        if (g_log) log_write("alcGetProcAddress(\"EAXSet\") → proxy eax_set %p", &eax_set);
        return (void*)&eax_set;
    }
    if (f && strcmp(f, "EAXGet") == 0) {
        if (g_log) log_write("alcGetProcAddress(\"EAXGet\") → proxy eax_get %p", &eax_get);
        return (void*)&eax_get;
    }
    typedef void* (__cdecl *PFN)(void*, const char*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alcGetProcAddress");
    void* result = pfn ? pfn(d, f) : NULL;
    if (g_log) log_write("alcGetProcAddress(\"%s\") → %p (orig)", f ? f : "(null)", result);
    return result;
}
FWD(int,   alcGetEnumValue,        (void* d, const char* e), (d,e))
FWD(const char*, alcGetString,     (void* d, int p), (d,p))
FWD_VOID(  alcGetIntegerv,         (void* d, int p, int s, int* v), (d,p,s,v))
FWD(void*, alcCaptureOpenDevice,   (const char* n, unsigned int f, int fmt, int b), (n,f,fmt,b))
FWD(int,   alcCaptureCloseDevice,  (void* d), (d))
FWD_VOID(  alcProcessContext,      (void* c), (c))
FWD_VOID(  alcSuspendContext,      (void* c), (c))
FWD(int,   alcGetError,            (void* d), (d))
FWD_VOID(  alcCaptureStart,        (void* d), (d))
FWD_VOID(  alcCaptureStop,         (void* d), (d))
FWD_VOID(  alcCaptureSamples,      (void* d, void* b, int s), (d,b,s))

// ── AL sources ─────────────────────────────────────────────────────────────

// alGenSources — ЛОГИРУЕМ + регистрируем в кеше позиций
extern "C" __declspec(dllexport) void __cdecl alGenSources(int n, unsigned int* s)
{
    typedef void (__cdecl *PFN)(int, unsigned int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alGenSources");
    if (pfn) pfn(n, s);
    if (!s || n <= 0) return;
    for (int i = 0; i < n; i++) alloc_src(s[i]);
    if (g_log) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(g_log, "[%02d:%02d:%02d.%03d] SRC_GEN  n=%d ids=",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, n);
        for (int i = 0; i < n; i++) { if (i) fputc(',', g_log); fprintf(g_log, "%u", s[i]); }
        fputc('\n', g_log); fflush(g_log);
    }
}

extern "C" __declspec(dllexport) void __cdecl alGenBuffers(int n, unsigned int* b)
{
    typedef void (__cdecl *PFN)(int, unsigned int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alGenBuffers");
    if (pfn) pfn(n, b);
}

// alDeleteSources — освобождаем слоты в g_src[] (ранее — утечка после 64 ID)
extern "C" __declspec(dllexport) void __cdecl alDeleteSources(int n, const unsigned int* s)
{
    typedef void (__cdecl *PFN)(int, const unsigned int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alDeleteSources");
    if (pfn) pfn(n, s);
    if (s && n > 0) {
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < MAX_SOURCES; j++) {
                if (g_src[j].active && g_src[j].id == s[i]) {
                    g_src[j].active   = 0;
                    g_occlusion[j]    = 1.0f;
                    g_user_gain[j]    = 1.0f;
                    if (j < SA_MAX_SRC) g_ipl_src[j] = NULL;
                    break;
                }
            }
        }
    }
}
FWD(int,  alIsSource,              (unsigned int s), (s))

// alSourcef — перехватываем AL_GAIN
// Сохраняем gain пользователя, пересчитываем с occlusion
// g_internal_gain_set — флаг чтобы не затирать g_user_gain при внутреннем вызове
extern "C" __declspec(dllexport) void __cdecl alSourcef(unsigned int s, int p, float v)
{
    typedef void (__cdecl *PFN)(unsigned int, int, float);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcef");
    if (pfn) pfn(s, p, v);
    if (p == 0x100A) {  // AL_GAIN
        int slot = sa_src_slot(s);
        if (slot >= 0) {
            if (!g_internal_gain_set) {
                // Внешний вызов (игра) — запоминаем пользовательский gain
                g_user_gain[slot] = v;
                // Пере-применяем occlusion если SA активен и источник 3D
                if (g_sa_ok && g_sa_enabled && g_scene_ok &&
                    slot < SA_MAX_SRC && g_src[slot].active && !g_src[slot].relative) {
                    float occ = g_occlusion[slot];
                    if (occ < 0.99f) {
                        float applied = v * occ;
                        g_internal_gain_set = 1;
                        if (pfn) pfn(s, 0x100A, applied);
                        g_internal_gain_set = 0;
                    }
                }
            }
            // Если g_internal_gain_set — это наш внутренний вызов, ничего не делаем
        }
    }
}
FWD_VOID(alGetSourcef,             (unsigned int s, int p, float* v), (s,p,v))
FWD_VOID(alGetSourcefv,            (unsigned int s, int p, float* v), (s,p,v))
FWD_VOID(alGetSourcei,             (unsigned int s, int p, int* v), (s,p,v))
FWD_VOID(alGetSource3i,            (unsigned int s, int p, int* v1, int* v2, int* v3), (s,p,v1,v2,v3))
FWD_VOID(alSourceRewind,           (unsigned int s), (s))
FWD_VOID(alSourcePause,            (unsigned int s), (s))
// alSourcePlayv — пакетный вызов: обрабатываем occlusion для каждого источника
extern "C" __declspec(dllexport) void __cdecl alSourcePlayv(int n, const unsigned int* s)
{
    typedef void (__cdecl *PFN)(int, const unsigned int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcePlayv");
    if (pfn) pfn(n, s);
    if (!s || n <= 0) return;
    for (int i = 0; i < n; i++) {
        unsigned int src_id = s[i];
        int slot = sa_src_slot(src_id);
        if (slot < 0) continue;
        if (!g_scene_ok || !g_ipl_src[slot] || g_src[slot].relative) continue;
        float occ = g_occlusion[slot];
        float user_gain = g_user_gain[slot];
        float final_gain = user_gain * occ;
        typedef void (__cdecl *pfn_sf)(unsigned int, int, float);
        pfn_sf fn = (pfn_sf)GetProcAddress(g_orig, "alSourcef");
        if (fn) {
            g_internal_gain_set = 1;
            fn(src_id, 0x100A /*AL_GAIN*/, final_gain);
            g_internal_gain_set = 0;
        }
        if (g_log && occ < 0.95f) {
            log_write("OCCL src=%u slot=%d occ=%.3f gain %.2f->%.2f (playv)",
                      src_id, slot, occ, user_gain, final_gain);
        }
    }
}

// alSourceStopv — пакетный stop: без доп. обработки
extern "C" __declspec(dllexport) void __cdecl alSourceStopv(int n, const unsigned int* s)
{
    typedef void (__cdecl *PFN)(int, const unsigned int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourceStopv");
    if (pfn) pfn(n, s);
}
extern "C" __declspec(dllexport) void __cdecl alSourceStop(unsigned int s)
{
    typedef void (__cdecl *PFN)(unsigned int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourceStop");
    if (pfn) pfn(s);
    log_write("SRC_STOP src=%u", s);
}
FWD_VOID(alSourceRewindv, (int n, const unsigned int* s), (n,s))
FWD_VOID(alSourcePausev, (int n, const unsigned int* s), (n,s))
FWD_VOID(alSourceQueueBuffers, (unsigned int s, int n, const unsigned int* b), (s,n,b))
FWD_VOID(alSourceUnqueueBuffers, (unsigned int s, int n, unsigned int* b), (s,n,b))

extern "C" __declspec(dllexport) void __cdecl alSourceiv(unsigned int s, int p, const int* v)
{
    typedef void (__cdecl *PFN)(unsigned int, int, const int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourceiv");
    if (pfn) pfn(s, p, v);
}

extern "C" __declspec(dllexport) void __cdecl alGetSource3f(unsigned int s, int p, float* v1, float* v2, float* v3)
{
    typedef void (__cdecl *PFN)(unsigned int, int, float*, float*, float*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alGetSource3f");
    if (pfn) pfn(s, p, v1, v2, v3);
}

extern "C" __declspec(dllexport) void __cdecl alGetSourceiv(unsigned int s, int p, int* v)
{
    typedef void (__cdecl *PFN)(unsigned int, int, int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alGetSourceiv");
    if (pfn) pfn(s, p, v);
}

// alSourcei — ЛОГИРУЕМ AL_BUFFER (только buf!=0) и AL_SOURCE_RELATIVE
extern "C" __declspec(dllexport) void __cdecl alSourcei(unsigned int s, int p, int v)
{
    typedef void (__cdecl *PFN)(unsigned int, int, int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcei");
    if (pfn) pfn(s, p, v);
    if (p == AL_BUFFER) {
        SrcState* st = find_src(s);
        if (!st) st = alloc_src(s);
        if (st) st->current_buf = (unsigned int)v;
        if ((unsigned int)v != 0)
            log_write("SRC_BUF src=%u buf=%u", s, (unsigned int)v);
    }
    else if (p == AL_SOURCE_RELATIVE) {
        log_write("SRC_REL  src=%u relative=%d", s, v);
        SrcState* st = find_src(s);
        if (!st) st = alloc_src(s);
        if (st) st->relative = (v != 0);
    }
}

// alSourcefv — ЛОГИРУЕМ AL_POSITION с дедупликацией + обновляем inputs симулятора
extern "C" __declspec(dllexport) void __cdecl alSourcefv(
unsigned int s, int p, const float* v)
{
    typedef void (__cdecl *PFN)(unsigned int, int, const float*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcefv");

    if (p == AL_POSITION && v && g_sa_ok && g_sa_enabled) {
        SrcState* st = find_src(s);
        if (!st) st = alloc_src(s);
        if (st && fdist(v[0],v[1],v[2], st->x,st->y,st->z) > POS_THRESHOLD) {
            st->x = v[0]; st->y = v[1]; st->z = v[2];
            log_write("SRC_POS src=%u x=%.1f y=%.1f z=%.1f", s, v[0], v[1], v[2]);
        }
        // HRTF injection handles panning — pass position through unchanged
    }
    if (pfn) pfn(s, p, v);
}

// alSource3i — override AL_POSITION
extern "C" __declspec(dllexport) void __cdecl alSource3i(
unsigned int s, int p, int v1, int v2, int v3)
{
    typedef void (__cdecl *PFN)(unsigned int, int, int, int, int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSource3i");
    if (p == AL_POSITION && g_sa_ok && g_sa_enabled) {
        float fv[3] = { (float)v1, (float)v2, (float)v3 };
        alSourcefv(s, p, fv);
        return;
    }
    if (pfn) pfn(s, p, v1, v2, v3);
}

// alSource3f — override AL_POSITION
extern "C" __declspec(dllexport) void __cdecl alSource3f(
unsigned int s, int p, float v1, float v2, float v3)
{
    typedef void (__cdecl *PFN)(unsigned int, int, float, float, float);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSource3f");
    if (p == AL_POSITION && g_sa_ok && g_sa_enabled) {
        float fv[3] = { v1, v2, v3 };
        alSourcefv(s, p, fv);
        return;
    }
    if (pfn) pfn(s, p, v1, v2, v3);
}


// alSourcePlay
extern "C" __declspec(dllexport) void __cdecl alSourcePlay(unsigned int s)
{
    typedef void (__cdecl *PFN)(unsigned int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcePlay");
    log_write("SRC_PLAY src=%u", s);

    static int sa_init_done = 0;
    if (!sa_init_done) {
        sa_init_done = 1;
        sa_init();
        if (g_log) {
            typedef const char* (__cdecl *pfn_gs)(int);
            pfn_gs gs = (pfn_gs)GetProcAddress(g_orig, "alGetString");
            if (gs) { fprintf(g_log,"[AL] ren=%s ver=%s\n", gs(0xB003), gs(0xB002)); }
            fflush(g_log);
        }
    }

    if (!g_sa_ok || !g_sa_enabled) { if (pfn) pfn(s); return; }

    SrcState* st = find_src(s);
    if (!st || st->relative) { if (pfn) pfn(s); return; }

    int slot = sa_src_slot(s);
    if (slot < 0) { if (pfn) pfn(s); return; }

    // Occlusion gain - disabled for now (occ=1.0 for all sources, incorrect)
    // Will re-enable when OBJ geometry is properly loaded for the scene
    /*
    if (g_scene_ok && g_ipl_src[slot]) {
        scene_update_source(slot, st);
        if (!g_sim_thread && sa_SimulatorRunDirect) {
            if (sa_SimulatorSetSharedInputs) {
                IPLSimulationSharedInputs_t si; memset(&si,0,sizeof(si));
                si.listener=g_sa_listener; si.numRays=32; si.numBounces=1;
                si.duration=1.0f; si.irradianceMinDistance=1.0f;
                sa_SimulatorSetSharedInputs(g_ipl_sim, 1, &si);
            }
            sa_SimulatorRunDirect(g_ipl_sim);
            if (sa_SourceGetOutputs) {
                IPLSimulationOutputs_t out; memset(&out,0,sizeof(out));
                sa_SourceGetOutputs(g_ipl_src[slot], 1, &out);
                float occ = out.direct.occlusion;
                if (occ < 0.0f) occ = 0.0f;
                if (occ > 1.0f) occ = 1.0f;
                g_occlusion[slot] = occ;
            }
        }
        float occ = g_occlusion[slot];
        float final_gain = g_user_gain[slot] * occ;
        typedef void (__cdecl *pfn_sf)(unsigned int, int, float);
        pfn_sf fn = (pfn_sf)GetProcAddress(g_orig, "alSourcef");
        if (fn) { g_internal_gain_set = 1; fn(s, 0x100A, final_gain); g_internal_gain_set = 0; }
        if (g_log && occ < 0.95f) log_write("OCCL src=%u slot=%d occ=%.3f", s, slot, occ);
    }
    */

    // HRTF injection: convert MONO16 → STEREO16 with Steam Audio binaural
    {
        BufInfo* bi = find_buf(st->current_buf);
        if (!cfg_hrtf_enabled || !g_effects_ok || !bi || !bi->pcm16 || bi->pcm16_samples <= 0) {
            if (pfn) pfn(s); return;
        }
        int src_ns = bi->pcm16_samples;
            int freq = bi->frequency > 0 ? bi->frequency : 22050;
            int total_out_samples = src_ns * 2;
            short* stereo_buf = (short*)HeapAlloc(GetProcessHeap(), 0, total_out_samples * sizeof(short));
            if (stereo_buf) {
                IPLVector3 src_sa = { st->x, st->z, -st->y };
                IPLVector3 dir = {1,0,0};
                if (sa_CalculateRelativeDirection) {
                    dir = sa_CalculateRelativeDirection(g_sa_ctx,
                        src_sa, g_sa_listener.origin, g_sa_listener.ahead, g_sa_listener.up);
                }
                int n_frames = (src_ns + SA_FRAME_SIZE - 1) / SA_FRAME_SIZE;
                for (int f = 0; f < n_frames; f++) {
                    int off = f * SA_FRAME_SIZE;
                    int ns = SA_FRAME_SIZE;
                    if (off + ns > src_ns) ns = src_ns - off;
                    if (ns <= 0) break;
                    for (int i = 0; i < ns; i++)
                        g_buf_in_mono.data[0][i] = (float)bi->pcm16[off + i] / 32768.0f;
                    for (int i = ns; i < SA_FRAME_SIZE; i++)
                        g_buf_in_mono.data[0][i] = 0.0f;
                    if (cfg_direct_effect && g_direct_effect && sa_DirectEffectApply) {
                        IPLAudioBuffer in_b = {1, SA_FRAME_SIZE, g_buf_in_mono.data};
                        IPLAudioBuffer out_b = {1, SA_FRAME_SIZE, g_buf_direct.data};
                        IPLDirectEffectParams dp;
                        memset(&dp, 0, sizeof(dp));
                        int dflags = 0;
                        if (cfg_dist_atten > 0.0f) dflags |= 1;
                        if (cfg_air_absorb > 0.0f) dflags |= 2;
                        if (cfg_occlusion > 0.0f) dflags |= 8;
                        dp.flags = (IPLDirectEffectFlags)dflags;
                        dp.transmissionType = (IPLTransmissionType)0;
                        float dx = src_sa.x - g_sa_listener.origin.x;
                        float dy = src_sa.y - g_sa_listener.origin.y;
                        float dz = src_sa.z - g_sa_listener.origin.z;
                        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                        if (dist < cfg_min_distance) dist = cfg_min_distance;
                        dp.distanceAttenuation = cfg_dist_atten > 0.0f ? cfg_dist_atten / dist : 1.0f;
                        dp.airAbsorption[0] = 1.0f - cfg_air_absorb; dp.airAbsorption[1] = 1.0f - cfg_air_absorb; dp.airAbsorption[2] = 1.0f - cfg_air_absorb;
                        dp.occlusion = cfg_occlusion > 0.0f ? g_occlusion[slot] : 1.0f;
                        dp.directivity = 1.0f;
                        dp.transmission[0] = 1.0f; dp.transmission[1] = 1.0f; dp.transmission[2] = 1.0f;
                        sa_DirectEffectApply(g_direct_effect, &dp, &in_b, &out_b);
                    } else {
                        memcpy(g_buf_direct.data, g_buf_in_mono.data, SA_FRAME_SIZE * sizeof(float));
                    }
                    if (g_binaural_effect && sa_BinauralEffectApply) {
                        IPLAudioBuffer in_b = {1, SA_FRAME_SIZE, g_buf_direct.data};
                        IPLAudioBuffer out_b = {2, SA_FRAME_SIZE, g_buf_binaural.data};
                        IPLBinauralEffectParams bp;
                        memset(&bp, 0, sizeof(bp));
                        bp.interpolation = (IPLHRTFInterpolation)0;
                        bp.spatialBlend = cfg_hrtf_blend;
                        bp.hrtf = g_sa_hrtf;
                        bp.peakDelays = NULL;
                        bp.direction = dir;
                        sa_BinauralEffectApply(g_binaural_effect, &bp, &in_b, &out_b);
                    } else {
                        memcpy(g_buf_binaural.data[0], g_buf_direct.data, SA_FRAME_SIZE * sizeof(float));
                        memcpy(g_buf_binaural.data[1], g_buf_direct.data, SA_FRAME_SIZE * sizeof(float));
                    }
                    for (int i = 0; i < ns; i++) {
                        float l = g_buf_binaural.data[0][i] * cfg_volume_boost;
                        float r = g_buf_binaural.data[1][i] * cfg_volume_boost;
                        if (l > 1.0f) l = 1.0f; if (l < -1.0f) l = -1.0f;
                        if (r > 1.0f) r = 1.0f; if (r < -1.0f) r = -1.0f;
                        stereo_buf[(off+i)*2]   = (short)(l * 32767.0f);
                        stereo_buf[(off+i)*2+1] = (short)(r * 32767.0f);
                    }
                }
                static unsigned int hrtf_bufs[MAX_SOURCES];
                static int hrtf_bufs_inited = 0;
                if (!hrtf_bufs_inited) {
                    memset(hrtf_bufs, 0, sizeof(hrtf_bufs));
                    hrtf_bufs_inited = 1;
                }
                unsigned int hbuf = hrtf_bufs[slot];
                typedef void (__cdecl *pfn_gb)(int, unsigned int*);
                pfn_gb fnGB = (pfn_gb)GetProcAddress(g_orig, "alGenBuffers");
                if (!hbuf && fnGB) { fnGB(1, &hbuf); hrtf_bufs[slot] = hbuf; }
                if (hbuf) {
                    typedef void (__cdecl *pfn_bd)(unsigned int, int, const void*, int, int);
                    pfn_bd fnBD = (pfn_bd)GetProcAddress(g_orig, "alBufferData");
                    if (fnBD) fnBD(hbuf, 0x1103, stereo_buf, total_out_samples * sizeof(short), freq);
                    typedef void (__cdecl *pfn_si)(unsigned int, int, int);
                    pfn_si fnSI = (pfn_si)GetProcAddress(g_orig, "alSourcei");
                    if (fnSI) {
                        fnSI(s, 0x1009, hbuf);
                        fnSI(s, 0x202, 1);
                    }
                    typedef void (__cdecl *pfn_sf)(unsigned int, int, float);
                    pfn_sf fnSF = (pfn_sf)GetProcAddress(g_orig, "alSourcef");
                    if (fnSF) { g_internal_gain_set = 1; fnSF(s, 0x100A, cfg_gain_master); g_internal_gain_set = 0; }
                    float z0[3] = {0,0,0};
                    typedef void (__cdecl *pfn_sv)(unsigned int, int, const float*);
                    pfn_sv fnSV = (pfn_sv)GetProcAddress(g_orig, "alSourcefv");
                    if (fnSV) fnSV(s, 0x1004, z0);
                    log_write("[HRTF-INJECT] src=%u slot=%d buf=%u→hbuf=%u %d samples %dHz dir=(%.2f,%.2f,%.2f)",
                        s, slot, st->current_buf, hbuf, src_ns, freq, dir.x, dir.y, dir.z);
                }
            HeapFree(GetProcessHeap(), 0, stereo_buf);
        }
    }

    if (pfn) pfn(s);
}

extern "C" __declspec(dllexport) void __cdecl alDeleteBuffers(int n, const unsigned int* b)
{
    typedef void (__cdecl *PFN)(int, const unsigned int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alDeleteBuffers");
    if (pfn) pfn(n, b);
    for (int i = 0; i < n; i++) {
        BufInfo* bi = find_buf(b[i]);
        if (bi) { bi->active = 0; if (bi->pcm16) { HeapFree(GetProcessHeap(), 0, bi->pcm16); bi->pcm16 = NULL; } }
    }
}

extern "C" __declspec(dllexport) void __cdecl alBufferData(unsigned int b, int f, const void* d, int s, int freq) {
    typedef void (__cdecl *PFN)(unsigned int, int, const void*, int, int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alBufferData");
    if (pfn) pfn(b, f, d, s, freq);
    BufInfo* bi = find_buf(b);
    if (!bi) bi = alloc_buf(b);
    if (bi) {
        bi->format = f;
        bi->frequency = freq;
        bi->data_size = s;
        if (f == 0x1100 || f == 0x1101) bi->channels = 1;
        else if (f == 0x1102 || f == 0x1103) bi->channels = 2;
        else bi->channels = 0;
        if (f == 0x1100 || f == 0x1102) bi->sample_size = 1;
        else if (f == 0x1101 || f == 0x1103) bi->sample_size = 2;
        else bi->sample_size = 0;
        if (bi->pcm16) { HeapFree(GetProcessHeap(), 0, bi->pcm16); bi->pcm16 = NULL; }
        bi->pcm16_samples = 0;
        if (d && s > 0) {
            if (f == 0x1101) {
                int ns = s / 2;
                bi->pcm16 = (short*)HeapAlloc(GetProcessHeap(), 0, s);
                if (bi->pcm16) { memcpy(bi->pcm16, d, s); bi->pcm16_samples = ns; }
            } else if (f == 0x1103) {
                int ns = s / 4;
                bi->pcm16 = (short*)HeapAlloc(GetProcessHeap(), 0, ns * 2);
                if (bi->pcm16) {
                    const short* sp = (const short*)d;
                    for (int i = 0; i < ns; i++)
                        bi->pcm16[i] = (short)(((int)sp[i*2] + (int)sp[i*2+1]) / 2);
                    bi->pcm16_samples = ns;
                    bi->format = 0x1101;
                    bi->channels = 1;
                }
            }
        }
    }
    if (g_log) {
        fprintf(g_log,"[BUF] alBufferData buf=%u fmt=0x%X freq=%d size=%d ch=%d bps=%d\n",
                b, f, freq, s, bi?bi->channels:0, bi?(bi->sample_size*8):0);
        fflush(g_log);
    }
}
FWD(int, alIsBuffer, (unsigned int b), (b))
FWD_VOID(alBufferf, (unsigned int b, int p, float v), (b,p,v))
FWD_VOID(alBuffer3f,               (unsigned int b, int p, float v1, float v2, float v3), (b,p,v1,v2,v3))
FWD_VOID(alBufferfv,               (unsigned int b, int p, const float* v), (b,p,v))
FWD_VOID(alBufferi,                (unsigned int b, int p, int v), (b,p,v))
FWD_VOID(alBuffer3i,               (unsigned int b, int p, int v1, int v2, int v3), (b,p,v1,v2,v3))
FWD_VOID(alBufferiv,               (unsigned int b, int p, const int* v), (b,p,v))
FWD_VOID(alGetBufferf,             (unsigned int b, int p, float* v), (b,p,v))
FWD_VOID(alGetBuffer3f,            (unsigned int b, int p, float* v1, float* v2, float* v3), (b,p,v1,v2,v3))
FWD_VOID(alGetBufferfv,            (unsigned int b, int p, float* v), (b,p,v))
FWD_VOID(alGetBufferi,             (unsigned int b, int p, int* v), (b,p,v))
FWD_VOID(alGetBuffer3i,            (unsigned int b, int p, int* v1, int* v2, int* v3), (b,p,v1,v2,v3))
FWD_VOID(alGetBufferiv,            (unsigned int b, int p, int* v), (b,p,v))

// ── AL listener ────────────────────────────────────────────────────────────
FWD_VOID(alListenerf,              (int p, float v), (p,v))
FWD_VOID(alListener3f,             (int p, float v1, float v2, float v3), (p,v1,v2,v3))
FWD_VOID(alGetListenerf,           (int p, float* v), (p,v))
FWD_VOID(alGetListener3f,          (int p, float* v1, float* v2, float* v3), (p,v1,v2,v3))
FWD_VOID(alGetListenerfv,          (int p, float* v), (p,v))
FWD_VOID(alGetListeneri,           (int p, int* v), (p,v))
FWD_VOID(alGetListener3i,          (int p, int* v1, int* v2, int* v3), (p,v1,v2,v3))
FWD_VOID(alGetListeneriv,          (int p, int* v), (p,v))
FWD_VOID(alListeneri,              (int p, int v), (p,v))
FWD_VOID(alListener3i,             (int p, int v1, int v2, int v3), (p,v1,v2,v3))
FWD_VOID(alListeneriv,             (int p, const int* v), (p,v))

// alListenerfv — ЛОГИРУЕМ AL_POSITION и AL_ORIENTATION + обновляем SA listener
// UE2 координаты: X=вперёд, Y=вправо, Z=вверх
// OpenAL координаты: X=вправо, Y=вверх, Z=назад
// Трансформация: OAL(x,y,z) = UE(y, z, -x)
extern "C" __declspec(dllexport) void __cdecl alListenerfv(int p, const float* v)
{
    typedef void (__cdecl *PFN)(int, const float*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alListenerfv");

    // RightAlt toggle + HUD title update
    {
        static int ralt_was_down = 0;
        int ralt_now = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        if (ralt_now && !ralt_was_down) {
            g_sa_enabled = !g_sa_enabled;
            log_write("SA binaural %s (RightAlt)", g_sa_enabled ? "ON" : "OFF");
        }
        ralt_was_down = ralt_now;
        static int rctrl_was_down = 0;
        int rctrl_now = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
        if (rctrl_now && !rctrl_was_down) {
            load_config();
            log_write("Config reloaded (RightCtrl)");
        }
        rctrl_was_down = rctrl_now;
        static DWORD s_last_hud = 0;
        DWORD now = GetTickCount();
        if (cfg_hud && now - s_last_hud > 500) {
            s_last_hud = now;
            HWND hw = FindWindowA(NULL, "Lineage II");
            if (!hw) hw = FindWindowA("UnrealWindow", NULL);
            if (hw) {
                char t[160];
                const char* status = g_sa_enabled ? "ON" : "OFF";
                const char* hrtf = g_effects_ok ? "HRTF" : "---";
                const char* scene = g_scene_ok ? "GEO" : "---";
                const char* sa = g_sa_ok ? "SA" : "--";
                snprintf(t, sizeof(t), "L2 [%s|%s|%s|%s] vol=%.1f blend=%.1f dist=%.1f",
                    sa, hrtf, scene, status, cfg_volume_boost, cfg_hrtf_blend, cfg_dist_atten);
                SetWindowTextA(hw, t);
            }
        }
    }

    if (!v) { if (pfn) pfn(p, v); return; }

    if (p == AL_ORIENTATION && pfn) {
        // v[0..2] = forward, v[3..5] = up — трансформируем UE2 → OpenAL
        float fwd_ue[3] = { v[0], v[1], v[2] };
        float up_ue[3]  = { v[3], v[4], v[5] };
        // Горизонтализируем forward: обнуляем Z (вертикальную составляющую) и нормализуем
        float fx = fwd_ue[0], fy = fwd_ue[1];
        float flen = sqrtf(fx*fx + fy*fy);
        if (flen < 0.001f) flen = 0.001f;
        fx /= flen; fy /= flen;
        // OAL forward = (fy, 0, -fx), up = (0, 1, 0) — горизонтальная плоскость
        float oal[6] = { fy, 0.0f, -fx,   0.0f, 1.0f, 0.0f };
        pfn(p, oal);
        // Лог оригинальных данных
        float dx=v[0]-g_lst_fx, dy=v[1]-g_lst_fy, dz=v[2]-g_lst_fz;
        if (sqrtf(dx*dx+dy*dy+dz*dz) > ORI_THRESHOLD) {
            g_lst_fx=v[0]; g_lst_fy=v[1]; g_lst_fz=v[2];
            log_write("LST_ORI  fwd=(%.3f,%.3f,%.3f) up=(%.3f,%.3f,%.3f) -> oal_fwd=(%.3f,0,%.3f)",
                      v[0],v[1],v[2],v[3],v[4],v[5], oal[0], oal[2]);
        }
        // SA orientation
        if (g_sa_ok) {
            g_sa_listener.ahead.x = oal[0]; g_sa_listener.ahead.y = oal[1]; g_sa_listener.ahead.z = oal[2];
            g_sa_listener.up.x    = oal[3]; g_sa_listener.up.y    = oal[4]; g_sa_listener.up.z    = oal[5];
            g_sa_listener.right.x = oal[1]*oal[5] - oal[2]*oal[4];
            g_sa_listener.right.y = oal[2]*oal[3] - oal[0]*oal[5];
            g_sa_listener.right.z = oal[0]*oal[4] - oal[1]*oal[3];
        }
        return;
    }

    if (pfn) pfn(p, v);
    if (p == AL_POSITION) {
        if (fdist(v[0],v[1],v[2], g_lst_x,g_lst_y,g_lst_z) > LST_POS_THRESHOLD) {
            g_lst_x=v[0]; g_lst_y=v[1]; g_lst_z=v[2];
            log_write("LST_POS  x=%.1f y=%.1f z=%.1f", v[0], v[1], v[2]);
        }
        // SA listener position — rate-limit 60 Hz + трекинг движения
        if (g_sa_ok) {
            DWORD now = GetTickCount();
            if ((now - g_sa_listener_ts) >= SA_LISTENER_INTERVAL_MS) {
                g_sa_listener_ts = now;
                // Трекинг: двигается ли listener?
                float dx = v[0] - g_sa_listener_prev_x;
                float dy = v[1] - g_sa_listener_prev_y;
                float dz = v[2] - g_sa_listener_prev_z;
                float moved = dx*dx + dy*dy + dz*dz;
                if (moved < 1.0f) {
                    g_sa_listener_static_ticks++;
                } else {
                    g_sa_listener_static_ticks = 0;
                    g_sa_listener_prev_x = v[0];
                    g_sa_listener_prev_y = v[1];
                    g_sa_listener_prev_z = v[2];
                }
g_sa_listener.origin.x = v[0];
g_sa_listener.origin.y = v[2];
g_sa_listener.origin.z = -v[1];
            }
        }
    }
}

// ── AL misc (все тихо) ─────────────────────────────────────────────────────
FWD_VOID(alEnable,                 (int c), (c))
FWD_VOID(alDisable,                (int c), (c))
FWD(int,    alIsEnabled,           (int c), (c))
FWD(const char*, alGetString,      (int p), (p))
FWD_VOID(alGetBooleanv,            (int p, char* d), (p,d))
FWD_VOID(alGetIntegerv,            (int p, int* d), (p,d))
FWD_VOID(alGetFloatv,              (int p, float* d), (p,d))
FWD_VOID(alGetDoublev,             (int p, double* d), (p,d))
FWD(char,   alGetBoolean,          (int p), (p))
FWD(int,    alGetInteger,          (int p), (p))
FWD(float,  alGetFloat,            (int p), (p))
FWD(double, alGetDouble,           (int p), (p))
FWD(int,    alGetError,            (void), ())
// alIsExtensionPresent — перехватываем "EAX3.0" → AL_TRUE
// Остальные расширения — форвард к оригиналу
extern "C" __declspec(dllexport) int __cdecl alIsExtensionPresent(const char* e)
{
    if (e && strcmp(e, "EAX3.0") == 0) {
        if (g_log) log_write("alIsExtensionPresent(\"EAX3.0\") → AL_TRUE (proxy)");
        return AL_TRUE; // 1
    }
    if (e && strcmp(e, "EAX4.0") == 0) {
        if (g_log) log_write("alIsExtensionPresent(\"EAX4.0\") → AL_TRUE (proxy, compat)");
        return AL_TRUE;
    }
    // Форвард к оригиналу для всего остального
    typedef int (__cdecl *PFN)(const char*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alIsExtensionPresent");
    if (pfn) return pfn(e);
    return AL_FALSE;
}

// alGetProcAddress — возвращаем наши EAXSet/EAXGet для EAX запросов
// Остальные — форвард к оригиналу
extern "C" __declspec(dllexport) void* __cdecl alGetProcAddress(const char* f)
{
    if (f && strcmp(f, "EAXSet") == 0) {
        if (g_log) log_write("alGetProcAddress(\"EAXSet\") → proxy eax_set %p", &eax_set);
        return (void*)&eax_set;
    }
    if (f && strcmp(f, "EAXGet") == 0) {
        if (g_log) log_write("alGetProcAddress(\"EAXGet\") → proxy eax_get %p", &eax_get);
        return (void*)&eax_get;
    }
    typedef void* (__cdecl *PFN)(const char*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alGetProcAddress");
    void* result = pfn ? pfn(f) : NULL;
    if (g_log) log_write("alGetProcAddress(\"%s\") → %p (orig)", f ? f : "(null)", result);
    return result;
}
FWD(int,    alGetEnumValue,        (const char* e), (e))
FWD_VOID(alDopplerFactor,          (float v), (v))
FWD_VOID(alDopplerVelocity,        (float v), (v))
FWD_VOID(alSpeedOfSound,           (float v), (v))
FWD_VOID(alDistanceModel, (int m), (m))

// ── EAX direct exports ────────────────────────────────────────────────────
// ALAudio.dll загружает EAXSet/EAXGet через GetProcAddress(hModule, "EAXSet")
// а НЕ через alGetProcAddress! Наш прокси должен экспортировать их напрямую.
extern "C" __declspec(dllexport) ALenum __cdecl EAXSet(const GUID* guid, unsigned long property,
                          unsigned long source, void* value, unsigned long size)
{
    if (g_log) {
        log_write("EAXSet() called! guid=%08X prop=%lu src=%lu size=%lu",
                  guid ? guid->Data1 : 0, property, source, size);
    }
    return eax_set(guid, property, source, value, size);
}
extern "C" __declspec(dllexport) ALenum __cdecl EAXGet(const GUID* guid, unsigned long property,
                          unsigned long source, void* value, unsigned long size)
{
    if (g_log) {
        log_write("EAXGet() called! guid=%08X prop=%lu src=%lu size=%lu",
                  guid ? guid->Data1 : 0, property, source, size);
    }
    return eax_get(guid, property, source, value, size);
}

// ── DllMain ────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        GetModuleFileNameA(hModule, g_my_dir, MAX_PATH);
        char* last_slash = strrchr(g_my_dir, '\\');
        if (!last_slash) last_slash = strrchr(g_my_dir, '/');
        if (last_slash) *last_slash = 0; else g_my_dir[0] = 0;

        g_log = fopen("openal_log.txt", "w");
        if (g_log) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(g_log,
                "=== OpenAL32 Proxy v11.0-SA-HRTF (LA 2.7) ===\n"
                "=== %04d-%02d-%02d %02d:%02d:%02d ===\n\n"
                "Logged: SRC_GEN | SRC_BUF(!=0) | SRC_REL | SRC_POS(d>50) | SRC_PLAY | SRC_STOP | LST_POS(d>10) | LST_ORI\n\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
            fflush(g_log);
        }
        g_orig = load_original_openal();
        if (!g_orig) {
            if (g_log) {
                fprintf(g_log,
                    "WARNING: no original OpenAL found — passthrough mode\n");
                fflush(g_log);
            }
        } else {
            log_write("orig loaded at 0x%p", (void*)g_orig);
        }
        log_write("EAXSet export at %p, EAXGet export at %p", (void*)&EAXSet, (void*)&EAXGet);
        log_write("eax_set internal at %p, eax_get internal at %p", (void*)&eax_set, (void*)&eax_get);
        // sa_init() вызывается лениво при первом alSourcePlay —
        // к тому моменту OpenAL контекст уже создан и активен
    } else if (reason == DLL_PROCESS_DETACH) {
        sa_shutdown();
        if (g_log) {
            log_write("=== EAX stats: total=%ld occl=%ld outsidehf=%ld(nonzero=%ld) listener=%ld",
                g_eax_set_count, g_eax_set_occl_count,
                g_eax_set_outsidehf_count, g_eax_set_outsidehf_nonzero,
                g_eax_set_listener_count);
            log_write("=== unloading ==="); fclose(g_log); g_log = NULL;
        }
        if (g_orig) { FreeLibrary(g_orig); g_orig = NULL; }
    }
    return TRUE;
}
