/**
 * OpenAL32.dll — proxy для Lineage 2 Kamael (UE2.5)  v9-SA-OCCL
 * База: v8-SA (HRTF binaural)
 * Добавлено: Steam Audio IPLScene + IPLSimulator — окклюзия по геометрии DEV
 *
 * Логирует (как v8):
 *   SRC_GEN / SRC_BUF / SRC_REL / SRC_POS / SRC_PLAY / SRC_STOP
 *   LST_POS / LST_ORI / OCCL (при occlusion < 0.95)
 *
 * Steam Audio:
 *   - relative=0 (3D источники) → occlusion через сцену + binaural HRTF
 *   - relative=1 (UI/музыка)    → pass-through без обработки
 *   - phonon.dll отсутствует    → тихий fallback, звук как в v7
 *   - сцена не загрузилась      → тихий fallback, звук как в v8
 *
 * Требует: oren_scene_dev.obj рядом с OpenAL32.dll в папке игры
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
typedef struct { IPLVector3 origin,ahead,up,right; } IPLCoordinateSpace3;
typedef struct { IPLint32 numChannels; IPLint32 numSamples; IPLfloat32** data; } IPLAudioBuffer;

typedef void* IPLContext;
typedef void* IPLHRTF;
typedef void* IPLBinauralEffect;

typedef enum { IPL_HRTFTYPE_DEFAULT=0 }        IPLHRTFType;
typedef enum { IPL_HRTFINTERP_BILINEAR=1 }      IPLHRTFInterpolation;
typedef enum { IPL_EFFECTSTATE_TAILCOMPLETE=1 } IPLAudioEffectState;

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

// Function pointers — ОБЯЗАТЕЛЬНО __stdcall (phonon.dll экспортирует как IPLCALL=__stdcall)
typedef IPLerror (__stdcall *pfn_iplContextCreate)(IPLContextSettings*, IPLContext*);
typedef void     (__stdcall *pfn_iplContextRelease)(IPLContext*);
typedef IPLerror (__stdcall *pfn_iplHRTFCreate)(IPLContext, IPLAudioSettings*, IPLHRTFSettings*, IPLHRTF*);
typedef void     (__stdcall *pfn_iplHRTFRelease)(IPLHRTF*);
typedef IPLerror (__stdcall *pfn_iplBinauralEffectCreate)(IPLContext, IPLAudioSettings*, IPLBinauralEffectSettings*, IPLBinauralEffect*);
typedef void     (__stdcall *pfn_iplBinauralEffectRelease)(IPLBinauralEffect*);
typedef IPLAudioEffectState (__stdcall *pfn_iplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*);
typedef void     (__stdcall *pfn_iplBinauralEffectReset)(IPLBinauralEffect);
typedef void     (__stdcall *pfn_iplAudioBufferAllocate)(IPLContext, IPLint32, IPLint32, IPLAudioBuffer*);
typedef void     (__stdcall *pfn_iplAudioBufferFree)(IPLContext, IPLAudioBuffer*);
typedef void     (__stdcall *pfn_iplAudioBufferInterleave)(IPLContext, IPLAudioBuffer*, IPLfloat32*);
typedef void     (__stdcall *pfn_iplAudioBufferDeinterleave)(IPLContext, IPLfloat32*, IPLAudioBuffer*);
typedef void     (__stdcall *pfn_iplAudioBufferMix)(IPLContext, IPLAudioBuffer*, IPLAudioBuffer*);

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
    IPLint32   type;         // 0 = DEFAULT
    IPLfloat32 minDistance;
    void* callback; void* userData; IPLbool dirty;
} IPLDistAttModel_t;

typedef struct {
    IPLint32   type;         // 0 = DEFAULT
    IPLfloat32 coefficients[3];
    void* callback; void* userData; IPLbool dirty;
} IPLAirAbsModel_t;

typedef struct {
    IPLfloat32 dipoleWeight; IPLfloat32 dipolePower;
    void* callback; void* userData;
} IPLDirectivity_t;

typedef struct {
    IPLint32   flags;       // IPL_SIMULATIONFLAGS_DIRECT = 1
    IPLint32   directFlags; // IPL_DIRECTSIMULATIONFLAGS_OCCLUSION = 1<<3
    IPLCoordinateSpace3  source;
    IPLDistAttModel_t    distanceAttenuationModel;
    IPLAirAbsModel_t     airAbsorptionModel;
    IPLDirectivity_t     directivity;
    IPLint32   occlusionType;    // 0 = RAYCAST
    IPLfloat32 occlusionRadius;
    IPLint32   numOcclusionSamples;
    IPLint32   numTransmissionRays;
    // pathing-поля (не используются, нужны для правильного sizeof)
    void*      reverbProbes;
    IPLint32   bakedDataId[8];  // IPLBakedDataIdentifier = 2×int
    void*      pathingProbes;
    IPLfloat32 visRadius; IPLfloat32 visThreshold; IPLfloat32 visRange;
    IPLint32   pathingOrder;
    IPLbool    enableValidation; IPLbool findAlternatePaths;
    IPLint32   numTransmissionRays2;
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
#define SA_SAMPLE_RATE  44100
#define SA_FRAME_SIZE   512
#define SA_LISTENER_INTERVAL_MS 16   // ~60 Hz

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
static float g_orig_gain[SA_MAX_SRC] = {
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

// Трекинг неподвижных источников — для поиска статуи
// Источник считается статичным если не двигался N тиков подряд
#define STATIC_SRC_TICKS 10  // 1 секунда при 10Hz
static int g_src_still_ticks[SA_MAX_SRC] = {0};  // счётчик тиков без движения
static float g_src_prev_x[SA_MAX_SRC] = {0};
static float g_src_prev_y[SA_MAX_SRC] = {0};
static float g_src_prev_z[SA_MAX_SRC] = {0};

// SA function pointers — только Context/HRTF (нужны для IPLSimulator)
static pfn_iplContextCreate           sa_ContextCreate           = NULL;
static pfn_iplContextRelease          sa_ContextRelease          = NULL;
static pfn_iplHRTFCreate              sa_HRTFCreate              = NULL;
static pfn_iplHRTFRelease             sa_HRTFRelease             = NULL;

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

// Forward decls — g_src[], SrcState, sa_src_slot определены ниже после SA-констант
#define MAX_SOURCES 64
struct SrcState { unsigned int id; float x,y,z; int active; int relative; };
static SrcState g_src[MAX_SOURCES];
static int sa_src_slot(unsigned int id);

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
    // Первый проход — считаем
    int nv = 0, nf = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='v' && line[1]==' ') nv++;
        else if (line[0]=='f' && line[1]==' ') nf++;
    }
    rewind(f);
    if (!nv || !nf) {
        if (g_log) { fprintf(g_log,"[SCENE] OBJ empty v=%d f=%d\n",nv,nf); fflush(g_log); }
        fclose(f); return 0;
    }
    IPLVector3*  verts = (IPLVector3*)malloc(nv * sizeof(IPLVector3));
    IPLTriangle* tris  = (IPLTriangle*)malloc(nf * sizeof(IPLTriangle));
    IPLint32*    midx  = (IPLint32*)malloc(nf * sizeof(IPLint32));
    IPLMaterial* mats  = (IPLMaterial*)malloc(sizeof(IPLMaterial));
    if (!verts || !tris || !midx || !mats) {
        free(verts); free(tris); free(midx); free(mats);
        if (g_log) { fprintf(g_log,"[SCENE] OOM\n"); fflush(g_log); }
        fclose(f); return 0;
    }
    mats[0] = k_stone;

    // Второй проход — читаем
    // OBJ координаты = UE2 world units = те же что в AL-логе, масштаб 1:1
    int vi = 0, fi = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='v' && line[1]==' ' && vi < nv) {
            float x,y,z;
            if (sscanf(line+2, "%f %f %f", &x, &y, &z) == 3) {
                verts[vi].x = x; verts[vi].y = y; verts[vi].z = z; vi++;
            }
        } else if (line[0]=='f' && line[1]==' ' && fi < nf) {
            int a=0, b=0, c=0;
            // Читаем три токена, берём только первый int из каждого (до '/' или пробела)
            // Работает для: "i", "i/t", "i/t/n", "i//n"
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
            if (a>0 && b>0 && c>0) {
                tris[fi].indices[0] = a-1;
                tris[fi].indices[1] = b-1;
                tris[fi].indices[2] = c-1;
                midx[fi] = 0; fi++;
            }
        }
    }
    fclose(f);
    g_mesh_verts = verts; g_mesh_tris = tris;
    g_mesh_matidx = midx; g_mesh_mats = mats;
    g_mesh_nverts = vi;   g_mesh_ntris = fi;
    if (g_log) {
        fprintf(g_log,"[SCENE] OBJ loaded: %d verts, %d tris\n", vi, fi); fflush(g_log);
    }
    return 1;
}

// Обновляет IPLSimulationInputs для одного источника
static void scene_update_source(int slot, SrcState* st)
{
    if (slot < 0 || !g_ipl_src[slot] || !sa_SourceSetInputs) return;
    if (!st || st->relative) return;

    IPLSimulationInputs_t inp;
    memset(&inp, 0, sizeof(inp));
    inp.flags       = 1;    // IPL_SIMULATIONFLAGS_DIRECT
    inp.directFlags = 1<<3; // IPL_DIRECTSIMULATIONFLAGS_OCCLUSION
    inp.occlusionType = 0;  // RAYCAST
    inp.occlusionRadius = 0.0f;
    inp.numOcclusionSamples = 1;
    // Позиция источника — игровой Y инвертирован относительно OBJ (json_y = -game_y)
    inp.source.origin.x =  st->x;
    inp.source.origin.y = -st->y;
    inp.source.origin.z =  st->z;
    inp.source.ahead.x  = 0.0f; inp.source.ahead.y  = 0.0f; inp.source.ahead.z = -1.0f;
    inp.source.up.x     = 0.0f; inp.source.up.y     = 1.0f; inp.source.up.z    =  0.0f;
    inp.source.right.x  = 1.0f; inp.source.right.y  = 0.0f; inp.source.right.z =  0.0f;
    // модели DEFAULT — SA считает сам по расстоянию
    inp.distanceAttenuationModel.type = 0;
    inp.airAbsorptionModel.type       = 0;
    sa_SourceSetInputs(g_ipl_src[slot], 1 /*DIRECT*/, &inp);
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
            // Копируем listener но инвертируем Y (игровой Y = -json Y)
            si.listener = g_sa_listener;
            si.listener.origin.y = -g_sa_listener.origin.y;
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
            fprintf(g_log,
                "[%02d:%02d:%02d.%03d] OCCL_DIAG active=%d "
                "listener_game=(%.0f,%.0f,%.0f) listener_sim=(%.0f,%.0f,%.0f)\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                active_count,
                (double)g_sa_listener.origin.x,
                (double)g_sa_listener.origin.y,
                (double)g_sa_listener.origin.z,
                (double)sim_listener_pos.x,
                (double)sim_listener_pos.y,
                (double)sim_listener_pos.z);
            for (int i = 0; i < SA_MAX_SRC; i++) {
                if (!g_src[i].active || g_src[i].relative) continue;
                float ax = g_src[i].x < 0 ? -g_src[i].x : g_src[i].x;
                float ay = g_src[i].y < 0 ? -g_src[i].y : g_src[i].y;
                if (ax > 500000.0f || ay > 500000.0f) continue; // скрываем невалидные
                float gain_final = g_orig_gain[i] * g_occlusion[i];
                // [STATIC] если источник не двигался 1+ секунду — кандидат на статую
                const char* tag = (g_src_still_ticks[i] >= STATIC_SRC_TICKS) ? " [STATIC]" : "";
                fprintf(g_log,
                    "  src=%u pos_game=(%.0f,%.0f,%.0f) pos_sim=(%.0f,%.0f,%.0f) "
                    "occ=%.3f gain=%.2f->%.2f still=%d%s\n",
                    g_src[i].id,
                    (double)g_src[i].x, (double)g_src[i].y, (double)g_src[i].z,
                    (double)g_src[i].x, (double)-g_src[i].y, (double)g_src[i].z,
                    (double)g_occlusion[i],
                    (double)g_orig_gain[i], (double)gain_final,
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
        g_orig_gain[i] = 1.0f;
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

    // Загружаем OBJ
    const char* obj_paths[] = {
        "oren_scene_dev.obj", "steam_audio/oren_scene_dev.obj", NULL
    };
    int loaded = 0;
    for (int i = 0; obj_paths[i]; i++)
        if (load_obj_geometry(obj_paths[i])) { loaded=1; break; }
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


static void sa_init(void)
{
    const char* candidates[] = { "phonon.dll", "steam_audio/phonon.dll", NULL };
    for (int i = 0; candidates[i]; i++) {
        g_phonon = LoadLibraryA(candidates[i]);
        if (g_phonon) break;
    }
    if (!g_phonon) {
        if (g_log) { fprintf(g_log,"[SA] phonon.dll not found — passthrough\n"); fflush(g_log); }
        return;
    }

    SA_LOAD(ContextCreate) SA_LOAD(ContextRelease)
    SA_LOAD(HRTFCreate)    SA_LOAD(HRTFRelease)

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
    sa_scene_init();
    return;

sa_fail:
    g_sa_ok = 0;
    if (g_log) { fprintf(g_log,"[SA] disabled\n"); fflush(g_log); }
}

static void sa_shutdown(void)
{
    if (!g_phonon) return;
    sa_scene_shutdown();
    if (g_sa_hrtf) sa_HRTFRelease(&g_sa_hrtf);
    if (g_sa_ctx)  sa_ContextRelease(&g_sa_ctx);
    FreeLibrary(g_phonon); g_phonon = NULL;
    g_sa_ok = 0;
}

// OpenAL constants (без подключения заголовков AL)
#define AL_POSITION         0x1004
#define AL_ORIENTATION      0x100F
#define AL_BUFFER           0x1009
#define AL_SOURCE_RELATIVE  0x202

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
FWD(void*, alcOpenDevice,          (const char* d), (d))
FWD(int,   alcCloseDevice,         (void* d), (d))

// alcCreateContext — перехватываем чтобы включить HRTF через ALC_HRTF_SOFT
extern "C" __declspec(dllexport) void* __cdecl alcCreateContext(void* d, const int* a)
{
    typedef void* (__cdecl *PFN)(void*, const int*);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alcCreateContext");
    if (!pfn) return NULL;

    // Проверяем поддержку ALC_SOFT_HRTF extension
    typedef int (__cdecl *pfn_alcIsExt)(void*, const char*);
    pfn_alcIsExt fnIsExt = (pfn_alcIsExt)GetProcAddress(g_orig, "alcIsExtensionPresent");
    int hrtf_supported = fnIsExt && fnIsExt(d, "ALC_SOFT_HRTF");

    if (hrtf_supported) {
        // ALC_HRTF_SOFT = 0x1992, ALC_TRUE = 1
        // Добавляем HRTF атрибуты к существующим
        int hrtf_attrs[64] = {0};
        int n = 0;
        if (a) {
            while (a[n] != 0 && n < 60) { hrtf_attrs[n] = a[n]; n++; }
        }
        hrtf_attrs[n++] = 0x1992;  // ALC_HRTF_SOFT
        hrtf_attrs[n++] = 1;       // ALC_TRUE
        hrtf_attrs[n]   = 0;       // terminate

        void* ctx = pfn(d, hrtf_attrs);
        if (g_log) {
            fprintf(g_log, "[HRTF] alcCreateContext with ALC_HRTF_SOFT=1: ctx=%p\n", ctx);
            fflush(g_log);
        }
        // Проверим что HRTF реально включился
        if (ctx) {
            typedef void (__cdecl *pfn_alcGetIntegerv)(void*, int, int, int*);
            pfn_alcGetIntegerv fnGetInt = (pfn_alcGetIntegerv)GetProcAddress(g_orig, "alcGetIntegerv");
            if (fnGetInt) {
                int hrtf_status = 0;
                fnGetInt(d, 0x1993, 1, &hrtf_status); // ALC_HRTF_STATUS_SOFT
                if (g_log) { fprintf(g_log, "[HRTF] status=%d (1=enabled, 2=required)\n", hrtf_status); fflush(g_log); }
            }
        }
        return ctx;
    }

    if (g_log) { fprintf(g_log, "[HRTF] ALC_SOFT_HRTF not supported — passthrough\n"); fflush(g_log); }
    return pfn(d, a);
}
FWD(int,   alcMakeContextCurrent,  (void* c), (c))
FWD_VOID(  alcDestroyContext,      (void* c), (c))
FWD(void*, alcGetCurrentContext,   (void), ())
FWD(void*, alcGetContextsDevice,   (void* c), (c))
FWD(int,   alcIsExtensionPresent,  (void* d, const char* e), (d,e))
FWD(void*, alcGetProcAddress,      (void* d, const char* f), (d,f))
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
FWD_VOID(alDeleteSources,          (int n, const unsigned int* s), (n,s))
FWD(int,  alIsSource,              (unsigned int s), (s))

// alSourcef — перехватываем AL_GAIN чтобы сохранить оригинальное значение
// (окклюзия применяется как множитель: applied = orig_gain * occlusion)
extern "C" __declspec(dllexport) void __cdecl alSourcef(unsigned int s, int p, float v)
{
    typedef void (__cdecl *PFN)(unsigned int, int, float);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcef");
    if (pfn) pfn(s, p, v);
    if (p == 0x100A) {  // AL_GAIN
        int slot = sa_src_slot(s);
        if (slot >= 0) g_orig_gain[slot] = v;
    }
}
FWD_VOID(alGetSourcef,             (unsigned int s, int p, float* v), (s,p,v))
FWD_VOID(alGetSourcefv,            (unsigned int s, int p, float* v), (s,p,v))
FWD_VOID(alGetSourcei,             (unsigned int s, int p, int* v), (s,p,v))
FWD_VOID(alGetSource3i,            (unsigned int s, int p, int* v1, int* v2, int* v3), (s,p,v1,v2,v3))
FWD_VOID(alSourceRewind,           (unsigned int s), (s))
FWD_VOID(alSourcePause,            (unsigned int s), (s))
FWD_VOID(alSourcePlayv,            (int n, const unsigned int* s), (n,s))
FWD_VOID(alSourceStopv,            (int n, const unsigned int* s), (n,s))
FWD_VOID(alSourceRewindv,          (int n, const unsigned int* s), (n,s))
FWD_VOID(alSourcePausev,           (int n, const unsigned int* s), (n,s))
FWD_VOID(alSourceQueueBuffers,     (unsigned int s, int n, const unsigned int* b), (s,n,b))
FWD_VOID(alSourceUnqueueBuffers,   (unsigned int s, int n, unsigned int* b), (s,n,b))

// alSourcei — ЛОГИРУЕМ AL_BUFFER (только buf!=0) и AL_SOURCE_RELATIVE
extern "C" __declspec(dllexport) void __cdecl alSourcei(unsigned int s, int p, int v)
{
    typedef void (__cdecl *PFN)(unsigned int, int, int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcei");
    if (pfn) pfn(s, p, v);
    if (p == AL_BUFFER && (unsigned int)v != 0)
        log_write("SRC_BUF  src=%u buf=%u", s, (unsigned int)v);
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
    if (pfn) pfn(s, p, v);
    if (p == AL_POSITION && v) {
        SrcState* st = find_src(s);
        if (!st) st = alloc_src(s);
        if (st && fdist(v[0],v[1],v[2], st->x,st->y,st->z) > POS_THRESHOLD) {
            st->x = v[0]; st->y = v[1]; st->z = v[2];
            log_write("SRC_POS  src=%u  x=%.1f y=%.1f z=%.1f", s, v[0], v[1], v[2]);
            // Обновляем позицию в симуляторе при каждом движении источника
            if (g_scene_ok) scene_update_source(sa_src_slot(s), st);
        }
    }
}

// alSource3i — ЛОГИРУЕМ AL_POSITION с дедупликацией
extern "C" __declspec(dllexport) void __cdecl alSource3i(
    unsigned int s, int p, int v1, int v2, int v3)
{
    typedef void (__cdecl *PFN)(unsigned int, int, int, int, int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSource3i");
    if (pfn) pfn(s, p, v1, v2, v3);
    if (p == AL_POSITION) {
        SrcState* st = find_src(s);
        if (!st) st = alloc_src(s);
        float fx=(float)v1, fy=(float)v2, fz=(float)v3;
        if (st && fdist(fx,fy,fz, st->x,st->y,st->z) > POS_THRESHOLD) {
            st->x=fx; st->y=fy; st->z=fz;
            log_write("SRC_POS  src=%u  x=%d y=%d z=%d (3i)", s, v1, v2, v3);
        }
    }
}

// alSource3f — ЛОГИРУЕМ AL_POSITION + обновляем симулятор
extern "C" __declspec(dllexport) void __cdecl alSource3f(
    unsigned int s, int p, float v1, float v2, float v3)
{
    typedef void (__cdecl *PFN)(unsigned int, int, float, float, float);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSource3f");
    if (pfn) pfn(s, p, v1, v2, v3);
    if (p == AL_POSITION) {
        log_write("SRC_POS src=%u  x=%.3f y=%.3f z=%.3f", s, v1, v2, v3);
        SrcState* st = find_src(s);
        if (!st) st = alloc_src(s);
        if (st) {
            st->x = v1; st->y = v2; st->z = v3;
            if (g_scene_ok) scene_update_source(sa_src_slot(s), st);
        }
    }
}

// alSourcePlay — ЛОГИРУЕМ + Steam Audio binaural для 3D источников
extern "C" __declspec(dllexport) void __cdecl alSourcePlay(unsigned int s)
{
    typedef void (__cdecl *PFN)(unsigned int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourcePlay");
    if (pfn) pfn(s);
    log_write("SRC_PLAY src=%u", s);

    // Lazy init — вызываем один раз когда OpenAL контекст уже точно активен
    static int sa_init_done = 0;
    if (!sa_init_done) {
        sa_init_done = 1;
        sa_init();
        // Логируем что поддерживает оригинальный OpenAL
        typedef const char* (__cdecl *pfn_alGetString)(int);
        pfn_alGetString fnGetStr = (pfn_alGetString)GetProcAddress(g_orig, "alGetString");
        if (g_log) {
            fprintf(g_log, "[AL] alGetString ptr: %p\n", (void*)fnGetStr);
            if (fnGetStr) {
                const char* ren = fnGetStr(0xB003);
                const char* ver = fnGetStr(0xB002);
                const char* ext = fnGetStr(0xB004);
                fprintf(g_log, "[AL] renderer:   %s\n", ren ? ren : "NULL");
                fprintf(g_log, "[AL] version:    %s\n", ver ? ver : "NULL");
                fprintf(g_log, "[AL] extensions: %.200s\n", ext ? ext : "NULL");
            }
            fflush(g_log);
        }
    }

    if (!g_sa_ok || !g_sa_enabled) return;

    SrcState* st = find_src(s);
    if (!st || st->relative) return;  // UI/музыка — не трогаем

    int slot = sa_src_slot(s);
    if (slot < 0) return;

    // --- Окклюзия через геометрию сцены ---
    if (g_scene_ok && g_ipl_src[slot]) {
        // Обновляем позицию источника в симуляторе
        scene_update_source(slot, st);

        // Если поток не запущен — синхронный запуск (fallback)
        if (!g_sim_thread && sa_SimulatorRunDirect) {
            if (sa_SimulatorSetSharedInputs) {
                IPLSimulationSharedInputs_t si; memset(&si,0,sizeof(si));
                si.listener=g_sa_listener;
                si.listener.origin.y = -g_sa_listener.origin.y;
                si.numRays=32; si.numBounces=1;
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

        // Применяем occlusion как множитель к gain
        float occ        = g_occlusion[slot];
        float orig_gain  = g_orig_gain[slot];
        float final_gain = orig_gain * occ;
        typedef void (__cdecl *pfn_sf)(unsigned int, int, float);
        pfn_sf fn = (pfn_sf)GetProcAddress(g_orig, "alSourcef");
        if (fn) fn(s, 0x100A /*AL_GAIN*/, final_gain);
        if (g_log && occ < 0.95f) {
            log_write("OCCL src=%u slot=%d occ=%.3f gain %.2f->%.2f",
                      s, slot, occ, orig_gain, final_gain);
        }
    }

}

// alSourceStop — ЛОГИРУЕМ source ID
extern "C" __declspec(dllexport) void __cdecl alSourceStop(unsigned int s)
{
    typedef void (__cdecl *PFN)(unsigned int);
    PFN pfn = (PFN)GetProcAddress(g_orig, "alSourceStop");
    if (pfn) pfn(s);
    log_write("SRC_STOP src=%u", s);
}

// ── AL buffers (все тихо) ──────────────────────────────────────────────────
FWD_VOID(alGenBuffers,             (int n, unsigned int* b), (n,b))
FWD_VOID(alDeleteBuffers,          (int n, const unsigned int* b), (n,b))
FWD(int,  alIsBuffer,              (unsigned int b), (b))
FWD_VOID(alBufferData,             (unsigned int b, int f, const void* d, int s, int freq), (b,f,d,s,freq))
FWD_VOID(alBufferf,                (unsigned int b, int p, float v), (b,p,v))
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

    // RightAlt toggle — проверяем каждый кадр
    {
        static int ralt_was_down = 0;
        int ralt_now = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        if (ralt_now && !ralt_was_down) {
            g_sa_enabled = !g_sa_enabled;
            log_write("SA binaural %s (RightAlt)", g_sa_enabled ? "ON" : "OFF");
        }
        ralt_was_down = ralt_now;
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
        // SA listener position — rate-limit 60 Hz
        if (g_sa_ok) {
            DWORD now = GetTickCount();
            if ((now - g_sa_listener_ts) >= SA_LISTENER_INTERVAL_MS) {
                g_sa_listener_ts = now;
                g_sa_listener.origin.x = v[0];
                g_sa_listener.origin.y = v[1];
                g_sa_listener.origin.z = v[2];
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
FWD(int,    alIsExtensionPresent,  (const char* e), (e))
FWD(void*,  alGetProcAddress,      (const char* f), (f))
FWD(int,    alGetEnumValue,        (const char* e), (e))
FWD_VOID(alDopplerFactor,          (float v), (v))
FWD_VOID(alDopplerVelocity,        (float v), (v))
FWD_VOID(alSpeedOfSound,           (float v), (v))
FWD_VOID(alDistanceModel,          (int m), (m))

// ── DllMain ────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_log = fopen("openal_log.txt", "w");
        if (g_log) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(g_log,
                "=== OpenAL32 Proxy v8-SA (Steam Audio) ===\n"
                "=== %04d-%02d-%02d %02d:%02d:%02d ===\n\n"
                "Logged: SRC_GEN | SRC_BUF(!=0) | SRC_REL | SRC_POS(d>50) | SRC_PLAY | SRC_STOP | LST_POS(d>10) | LST_ORI\n\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
            fflush(g_log);
        }
        g_orig = LoadLibraryA("OpenAL32_orig.dll");
        if (!g_orig) {
            if (g_log) { fprintf(g_log, "FATAL: cannot load OpenAL32_orig.dll\n"); fflush(g_log); }
            return FALSE;
        }
        log_write("orig loaded at 0x%p", (void*)g_orig);
        // sa_init() вызывается лениво при первом alSourcePlay —
        // к тому моменту OpenAL контекст уже создан и активен
    } else if (reason == DLL_PROCESS_DETACH) {
        sa_shutdown();
        if (g_log) { log_write("=== unloading ==="); fclose(g_log); g_log = NULL; }
        if (g_orig) { FreeLibrary(g_orig); g_orig = NULL; }
    }
    return TRUE;
}
