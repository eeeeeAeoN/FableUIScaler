#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include "MinHook.h"

namespace Retail {
    // NUISystem (Menus, Inventory, Pause, Map, Front-End, Cursor)
    constexpr uintptr_t ADDR_NUISYSTEM_GETUISCALE          = 0x0041CF47;
    constexpr uintptr_t ADDR_NUISYSTEM_GETBACKBUFFERWIDTH  = 0x0041CC14;
    constexpr uintptr_t ADDR_NUISYSTEM_GETBACKBUFFERHEIGHT = 0x0041CC2B;
    constexpr uintptr_t ADDR_PMAINGAMECOMPONENT            = 0x013B86A0;
    constexpr uintptr_t ADDR_CHOVERABLE_ISHOVERED          = 0x0055B8F0;

    // DisplayManager & Virtual HUD Scope (CPlayerGui / NPlayerGui / CSubtitleRenderer / CTCTavernGame)
    constexpr uintptr_t ADDR_GPDISPLAYMANAGER              = 0x013B8390;
    constexpr uintptr_t ADDR_CDISPLAYMANAGER_GETRTDIMS     = 0x009BEDC0;

    constexpr uintptr_t ADDR_CPLAYERGUI_DRAWPREPARE        = 0x00439070;
    constexpr uintptr_t ADDR_CSUBTITLERENDERER_DRAWPREPARE = 0x00490494;
    constexpr uintptr_t ADDR_CDRAWRECEIVEDITEMS_DRAW       = 0x0064E978;
    constexpr uintptr_t ADDR_CTCHERORECEIVEITEMS_PREPARE   = 0x007CA670;
    constexpr uintptr_t ADDR_CTCHERORECEIVEITEMS_GLOBALUPD = 0x007CACB0;
    constexpr uintptr_t ADDR_CDRAWBETTINGHUD_DRAW          = 0x00649952;
    constexpr uintptr_t ADDR_CDRAW2DTEXT_DRAW              = 0x006541D1;

    constexpr uintptr_t ADDR_CDRAWSCREENFADE_DRAW          = 0x006495A8;
    constexpr uintptr_t ADDR_CDRAWLETTERBOX_DRAW           = 0x00644677;

    constexpr uintptr_t ADDR_TAVERN_DRAWBETANDMONEY        = 0x005EA400;
    constexpr uintptr_t ADDR_TAVERN_DRAWTEXTLABEL          = 0x005EC7C0;
    constexpr uintptr_t ADDR_TAVERN_DRAWINFOBOX            = 0x005EC990;

    // CIEngine 2D Internal Primitives::Update
    constexpr uintptr_t ADDR_PRIM_2DSPRITE_UPDATE          = 0x00BAD8A0;
    constexpr uintptr_t ADDR_PRIM_2DCLOCKSPRITE_UPDATE     = 0x00BADB40;
    constexpr uintptr_t ADDR_PRIM_2DTEXT_UPDATE            = 0x00BC6860;
    constexpr uintptr_t ADDR_PRIM_2DSTATICMESH_UPDATE      = 0x00B9DFC0;
    constexpr uintptr_t ADDR_PRIM_2DANIMMESH_UPDATE        = 0x00BD2BC0; // Also called by 2DMorphedAnimatedMesh::Update

    // HUD & UI 2D Particles
    constexpr uintptr_t ADDR_CPARTICLEEMITTER_SET2DPOS       = 0x00AED090;
    constexpr uintptr_t ADDR_CPARTICLEEMITTER_PREPPARAMS     = 0x00AEE560; // CParticleEmitter::PreparePrimitives
    constexpr uintptr_t ADDR_CPARTICLEEMITTER_DTOR           = 0x00AF10E0; // CParticleEmitter::~CParticleEmitter
    constexpr uintptr_t ADDR_PRIM_2DPARTICLEGROUP_UPDATE     = 0x00BE0DE0; // CEngineInternalPrimitive2DParticleGroup::Update
    constexpr uintptr_t ADDR_UIPARTICLEEFFECT_INITIALISE     = 0x00543D30; // NUISystem::CParticleEffect::Initialise
    constexpr uintptr_t ADDR_UIPARTICLEEFFECT_CHANGESTATE    = 0x00543DF0; // NUISystem::CParticleEffect::ChangeState
    constexpr uintptr_t ADDR_UIPARTICLEEFFECT_DRAW           = 0x00543EE0; // NUISystem::CParticleEffect::Draw

    // RenderManager Viewport & 2D QuickDraw (Subtitles, Region Preview, Loading / Transition Progress)
    constexpr uintptr_t ADDR_CRENDERMANAGERCORE_SETVWINDOW = 0x00A0B560;
    constexpr uintptr_t ADDR_CTCREGIONDISPLAY_GLOBALDRAW   = 0x00639E40;
    constexpr uintptr_t ADDR_CPROGRESSDISPLAY_DRAWRETAIL   = 0x00498490;
    constexpr uintptr_t ADDR_CRENDERMANAGER2D_ADDTEXT      = 0x009DB700;
    constexpr uintptr_t ADDR_CRENDERMANAGER2D_ADD2DTRI     = 0x009DC030;
}

struct C2DVector {
    float X;
    float Y;
};

struct C2DBoxF {
    float TLX;
    float TLY;
    float BRX;
    float BRY;
};

struct C2DBoxI {
    int TLX;
    int TLY;
    int BRX;
    int BRY;
};

struct C2DExtentsI {
    int X;
    int Y;
};

struct CTVertexRHWColSpecTex1 {
    float    X;
    float    Y;
    float    Z;
    float    RHW;
    uint32_t DiffuseColour;
    uint32_t SpecularColour;
    float    U1;
    float    V1;
};
static_assert(sizeof(CTVertexRHWColSpecTex1) == 32, "CTVertexRHWColSpecTex1 must be 32 bytes");

struct ScalerConfig {
    float UIScaleMultiplier   = 1.0f;
    float MenuScaleMultiplier = 1.0f;
    float HUDScaleMultiplier  = 1.0f;
    float ReferenceHeight     = 720.0f;
    bool  AutoScaleWithRes    = true;
    bool  AllowDownscale      = true;
    bool  ScaleMainMenu       = true;
    bool  FilterScaledSprites = true;
};

static uintptr_t g_ExeBase = 0;
static HMODULE   g_SelfModule = nullptr;
static wchar_t   g_IniPath[MAX_PATH] = {};
static ScalerConfig g_Config;
static ULONGLONG g_LastIniCheckMs = 0;
static FILETIME  g_LastIniWriteTime = {};

inline uintptr_t Rebase(uintptr_t idaAddr) {
    return g_ExeBase + (idaAddr - 0x00400000u);
}

inline float ClampScale(float s) {
    if (s < 0.25f) return 0.25f;
    if (s > 8.0f)  return 8.0f;
    return s;
}

static float ReadIniFloat(const wchar_t* section, const wchar_t* key, float defVal, const wchar_t* path) {
    wchar_t buf[64] = {};
    DWORD len = GetPrivateProfileStringW(section, key, L"", buf, 64, path);
    if (len == 0) return defVal;
    wchar_t* endPtr = nullptr;
    float parsed = wcstof(buf, &endPtr);
    if (endPtr == buf) return defVal;
    return parsed;
}

static bool ReadIniBool(const wchar_t* section, const wchar_t* key, bool defVal, const wchar_t* path) {
    return GetPrivateProfileIntW(section, key, defVal ? 1 : 0, path) != 0;
}

static void LoadConfigFromIni(bool forceReload) {
    if (g_IniPath[0] == L'\0') return;

    ULONGLONG now = GetTickCount64();
    if (!forceReload && (now - g_LastIniCheckMs) < 1000) {
        return;
    }
    g_LastIniCheckMs = now;

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(g_IniPath, GetFileExInfoStandard, &fad)) {
        return;
    }
    if (!forceReload &&
        fad.ftLastWriteTime.dwLowDateTime == g_LastIniWriteTime.dwLowDateTime &&
        fad.ftLastWriteTime.dwHighDateTime == g_LastIniWriteTime.dwHighDateTime) {
        return;
    }
    g_LastIniWriteTime = fad.ftLastWriteTime;

    ScalerConfig cfg;
    cfg.UIScaleMultiplier   = ClampScale(ReadIniFloat(L"General", L"UIScaleMultiplier", 1.0f, g_IniPath));
    cfg.MenuScaleMultiplier = ClampScale(ReadIniFloat(L"General", L"MenuScaleMultiplier", 1.0f, g_IniPath));
    cfg.HUDScaleMultiplier  = ClampScale(ReadIniFloat(L"General", L"HUDScaleMultiplier", 1.0f, g_IniPath));
    float refH = ReadIniFloat(L"Behavior", L"ReferenceHeight", 720.0f, g_IniPath);
    cfg.ReferenceHeight     = (refH >= 240.0f && refH <= 4320.0f) ? refH : 720.0f;
    cfg.AutoScaleWithRes    = ReadIniBool(L"Behavior", L"AutoScaleWithResolution", true, g_IniPath);
    cfg.AllowDownscale      = ReadIniBool(L"Behavior", L"AllowDownscale", true, g_IniPath);
    cfg.ScaleMainMenu       = ReadIniBool(L"Behavior", L"ScaleMainMenu", true, g_IniPath);
    cfg.FilterScaledSprites = ReadIniBool(L"Behavior", L"FilterScaledSprites", true, g_IniPath);
    g_Config = cfg;
}

static void InitIniPath(HMODULE hMod) {
    g_SelfModule = hMod;
    wchar_t modPath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(hMod, modPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    wchar_t* slash = wcsrchr(modPath, L'\\');
    wchar_t* dot = wcsrchr(modPath, L'.');
    if (dot && (!slash || dot > slash)) {
        *dot = L'\0';
    }
    wcsncpy_s(g_IniPath, modPath, _TRUNCATE);
    wcsncat_s(g_IniPath, L".ini", _TRUNCATE);
}

// ============================================================================
// Typedefs & Original Pointers
// ============================================================================
using GetBackBufferDim_t = double(__cdecl*)();
using GetUIScale_t       = C2DVector* (__fastcall*)(void* _this, void* edx, C2DVector* outResult);
using IsHovered_t        = bool(__fastcall*)(void* _this, void* edx, const C2DBoxI* mousePos);

using GetRenderTargetDimensions_t = C2DExtentsI* (__fastcall*)(void* _this, void* edx, C2DExtentsI* outDims);

using VoidPrep_t         = void* (__fastcall*)(void* _this, void* edx, const void* info);
using CharPrep_t         = char(__fastcall*)(void* _this, void* edx, const void* info);
using IntPrep_t          = int(__fastcall*)(void* _this, void* edx, const void* info);
using ReceiveItemsPrepare_t = void(__fastcall*)(void* _this, void* edx, const void* pos);
using ReceiveItemsUpdate_t  = char(__fastcall*)(void* _this, void* edx, bool paused);

using TavernDrawBetAndMoney_t = void(__fastcall*)(void* _this, void* edx, void* info, float a, float b, float c, float d, float e, float f);
using TavernDrawTextLabel_t   = void(__fastcall*)(void* _this, void* edx, float a2, float a3, float a4, int a5, void* a6, int a7);
using TavernDrawInfoBox_t     = void(__fastcall*)(void* _this, void* edx, void* info);

using PrimUpdateChar_t   = char(__fastcall*)(void* _this, void* edx, const void* src, uint32_t flags);
using PrimUpdateUint_t   = uint32_t(__fastcall*)(void* _this, void* edx, const void* src, uint32_t flags);
using PrimUpdateInt_t    = int(__fastcall*)(void* _this, void* edx, const void* src, uint32_t flags);
using PrimUpdateAnim_t   = int(__fastcall*)(void* _this, void* edx, const void* src, char flags);
using PrimUpdate2DPart_t = int(__fastcall*)(void* _this, void* edx, const void* src, uint32_t flags);
using Set2DPosition_t    = int(__fastcall*)(void* _this, void* edx, const C2DVector* pos, int layer);
using EmitterPrepare_t   = char(__fastcall*)(void* _this, void* edx, float interpolate, const void* info);
using EmitterDtor_t      = void(__fastcall*)(void* _this, void* edx);
using UIPartInit_t       = int(__fastcall*)(void* _this, void* edx);
using UIPartState_t      = int(__fastcall*)(void* _this, void* edx, uint32_t state);
using UIPartDraw_t       = int(__fastcall*)(void* _this, void* edx, int a2, int a3, int a4, int a5, int a6);

using SetVWindow_t        = void(__fastcall*)(void* _this, void* edx, const C2DBoxF* twindow);
using RegionGlobalDraw_t  = char(__fastcall*)(void* _this, void* edx, int interp);
using DrawRetailDisplay_t = void(__fastcall*)(void* _this, void* edx, const void* textToDisplay, float percentComplete, bool forcePercentage);
using AddText_t           = void(__fastcall*)(void* _this, void* edx, const void* pos, float scale, const void* wideStr, const void* font, const void* colour, uint32_t flags);
using Add2DTri_t          = void(__fastcall*)(void* _this, void* edx, const CTVertexRHWColSpecTex1* v1, const CTVertexRHWColSpecTex1* v2, const CTVertexRHWColSpecTex1* v3, uint32_t flags, void* texture);

static GetUIScale_t                o_GetUIScale = nullptr;
static IsHovered_t                 o_IsHovered = nullptr;
static GetRenderTargetDimensions_t o_GetRTDims = nullptr;

static VoidPrep_t                  o_PlayerGuiDrawPrepare = nullptr;
static CharPrep_t                  o_SubtitleDrawPrepare = nullptr;
static CharPrep_t                  o_ReceivedItemsDraw = nullptr;
static ReceiveItemsPrepare_t       o_HeroReceiveItemsPrepareToShow = nullptr;
static ReceiveItemsUpdate_t        o_HeroReceiveItemsGlobalUpdate = nullptr;
static IntPrep_t                   o_BettingHUDDraw = nullptr;
static VoidPrep_t                  o_Draw2DTextFlush = nullptr;

static CharPrep_t                  o_ScreenFadeDraw = nullptr;
static IntPrep_t                   o_LetterBoxDraw = nullptr;

static TavernDrawBetAndMoney_t     o_TavernDrawBetAndMoney = nullptr;
static TavernDrawTextLabel_t       o_TavernDrawTextLabel = nullptr;
static TavernDrawInfoBox_t         o_TavernDrawInfoBox = nullptr;

static PrimUpdateChar_t            o_2DSpriteUpdate = nullptr;
static PrimUpdateUint_t            o_2DClockSpriteUpdate = nullptr;
static PrimUpdateInt_t             o_2DTextUpdate = nullptr;
static PrimUpdateInt_t             o_2DStaticMeshUpdate = nullptr;
static PrimUpdateAnim_t            o_2DAnimMeshUpdate = nullptr;
static PrimUpdate2DPart_t          o_2DParticleGroupUpdate = nullptr;
static Set2DPosition_t             o_Set2DPosition = nullptr;
static EmitterPrepare_t            o_ParticleEmitterPreparePrimitives = nullptr;
static EmitterDtor_t               o_ParticleEmitterDtor = nullptr;
static UIPartInit_t                o_UIParticleEffectInitialise = nullptr;
static UIPartState_t               o_UIParticleEffectChangeState = nullptr;
static UIPartDraw_t                o_UIParticleEffectDraw = nullptr;

static SetVWindow_t                o_SetVWindow = nullptr;
static RegionGlobalDraw_t          o_RegionGlobalDraw = nullptr;
static DrawRetailDisplay_t         o_DrawRetailDisplay = nullptr;
static AddText_t                   o_AddText = nullptr;
static Add2DTri_t                  o_Add2DTri = nullptr;

// ============================================================================
// Thread-local HUD Virtual Canvas State
// ============================================================================
thread_local int   tl_hudScopeDepth   = 0;
thread_local int   tl_hudExcludeDepth = 0;
thread_local int   tl_virtualW        = 1280;
thread_local int   tl_virtualH        = 720;
thread_local float tl_hudScaleX             = 1.0f;
thread_local float tl_hudScaleY             = 1.0f;
thread_local int   tl_uiParticleScopeDepth  = 0;
thread_local float tl_pending2DParticleScale = 1.0f;

enum class ParticleScaleMode : uint8_t {
    None = 0,
    Hud  = 1,
    Menu = 2,
};

static SRWLOCK g_EmitterScaleLock = SRWLOCK_INIT;
static std::unordered_map<const void*, ParticleScaleMode> g_EmitterScaleModes;

static void SetParticleEmitterScaleMode(const void* emitter, ParticleScaleMode mode) {
    if (!emitter || mode == ParticleScaleMode::None) return;
    AcquireSRWLockExclusive(&g_EmitterScaleLock);
    g_EmitterScaleModes[emitter] = mode;
    ReleaseSRWLockExclusive(&g_EmitterScaleLock);
}

static ParticleScaleMode GetParticleEmitterScaleMode(const void* emitter) {
    if (!emitter) return ParticleScaleMode::None;
    AcquireSRWLockShared(&g_EmitterScaleLock);
    auto it = g_EmitterScaleModes.find(emitter);
    ParticleScaleMode mode = (it != g_EmitterScaleModes.end()) ? it->second : ParticleScaleMode::None;
    ReleaseSRWLockShared(&g_EmitterScaleLock);
    return mode;
}

static void RemoveParticleEmitterScaleMode(const void* emitter) {
    if (!emitter) return;
    AcquireSRWLockExclusive(&g_EmitterScaleLock);
    g_EmitterScaleModes.erase(emitter);
    ReleaseSRWLockExclusive(&g_EmitterScaleLock);
}

struct UIParticleScopeGuard {
    UIParticleScopeGuard()  { ++tl_uiParticleScopeDepth; }
    ~UIParticleScopeGuard() { --tl_uiParticleScopeDepth; }
};

inline bool IsHudScalingActive() {
    return tl_hudScopeDepth > 0 && tl_hudExcludeDepth == 0;
}

static void RefreshHudTransformFromDisplayManager() {
    LoadConfigFromIni(false);

    void* dm = *reinterpret_cast<void* volatile*>(Rebase(Retail::ADDR_GPDISPLAYMANAGER));
    if (!dm || !o_GetRTDims) return;

    C2DExtentsI actual = { 0, 0 };
    o_GetRTDims(dm, nullptr, &actual);
    if (actual.X <= 0 || actual.Y <= 0) return;

    float effectiveH = (std::min)(static_cast<float>(actual.Y), static_cast<float>(actual.X) * 0.75f);
    float baseScale = g_Config.AutoScaleWithRes ? (effectiveH / g_Config.ReferenceHeight) : 1.0f;
    if (g_Config.AutoScaleWithRes && !g_Config.AllowDownscale && baseScale < 1.0f) {
        baseScale = 1.0f;
    }

    float desiredScale = ClampScale(baseScale * g_Config.UIScaleMultiplier * g_Config.HUDScaleMultiplier);
    int vw = (std::max)(1, static_cast<int>(std::lroundf(static_cast<float>(actual.X) / desiredScale)));
    int vh = (std::max)(1, static_cast<int>(std::lroundf(static_cast<float>(actual.Y) / desiredScale)));

    tl_virtualW = vw;
    tl_virtualH = vh;
    tl_hudScaleX = static_cast<float>(actual.X) / static_cast<float>(vw);
    tl_hudScaleY = static_cast<float>(actual.Y) / static_cast<float>(vh);
}

struct HudScopeGuard {
    bool active;
    HudScopeGuard() : active(true) {
        if (tl_hudScopeDepth == 0) {
            RefreshHudTransformFromDisplayManager();
        }
        ++tl_hudScopeDepth;
    }
    ~HudScopeGuard() {
        --tl_hudScopeDepth;
    }
};

struct HudExcludeGuard {
    HudExcludeGuard()  { ++tl_hudExcludeDepth; }
    ~HudExcludeGuard() { --tl_hudExcludeDepth; }
};

// ============================================================================
// Part 1: NUISystem Hooks (Menus, Inventory, Pause, Map, Front-End, Cursor)
// ============================================================================
static float ComputeMenuUIScale() {
    LoadConfigFromIni(false);

    void* mainGame = *reinterpret_cast<void* volatile*>(Rebase(Retail::ADDR_PMAINGAMECOMPONENT));
    if (!g_Config.ScaleMainMenu && mainGame == nullptr) {
        return 1.0f;
    }

    auto fnW = reinterpret_cast<GetBackBufferDim_t>(Rebase(Retail::ADDR_NUISYSTEM_GETBACKBUFFERWIDTH));
    auto fnH = reinterpret_cast<GetBackBufferDim_t>(Rebase(Retail::ADDR_NUISYSTEM_GETBACKBUFFERHEIGHT));
    float w = static_cast<float>(fnW());
    float h = static_cast<float>(fnH());

    float baseScale = 1.0f;
    if (g_Config.AutoScaleWithRes) {
        float effectiveH = (std::min)(h, w * 0.75f);
        // NUISystem panels were authored for 1024x768 (0xcad854). Scaling ReferenceHeight by 768/720
        // keeps 1080p/1440p/4K menus proportional to 720p without clipping full-height screens.
        float menuRefHeight = g_Config.ReferenceHeight * (768.0f / 720.0f);
        baseScale = effectiveH / menuRefHeight;
        if (!g_Config.AllowDownscale && baseScale < 1.0f) {
            baseScale = 1.0f;
        }
    }

    return ClampScale(baseScale * g_Config.UIScaleMultiplier * g_Config.MenuScaleMultiplier);
}

static C2DVector* __fastcall hk_GetUIScale(void* /*_this*/, void* /*edx*/, C2DVector* outResult) {
    float finalScale = ComputeMenuUIScale();
    outResult->X = finalScale;
    outResult->Y = finalScale;
    return outResult;
}

static bool __fastcall hk_CHoverable_IsHovered(void* _this, void* /*edx*/, const C2DBoxI* mousePos) {
    if (!mousePos) return false;

    using VecFn = void(__thiscall*)(void*, C2DVector*);
    using BoxFn = void(__thiscall*)(void*, C2DBoxF*);

    uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(_this);
    auto getTargetPos  = reinterpret_cast<VecFn>(vtable[0x1E8 / 4]);
    auto getTargetZoom = reinterpret_cast<VecFn>(vtable[0x1EC / 4]);
    auto getBBox       = reinterpret_cast<BoxFn>(vtable[0x60 / 4]);

    C2DVector target = { 0.0f, 0.0f };
    C2DVector zoom   = { 1.0f, 1.0f };
    C2DBoxF   box    = { 0.0f, 0.0f, 0.0f, 0.0f };

    getTargetPos(_this, &target);
    getTargetZoom(_this, &zoom);
    getBBox(_this, &box);

    // Fix vanilla bug where box.TLX/TLY were added without multiplying by zoom.
    int tlX = static_cast<int>(zoom.X * box.TLX + target.X);
    int tlY = static_cast<int>(zoom.Y * box.TLY + target.Y);
    int brX = static_cast<int>(zoom.X * box.BRX + target.X);
    int brY = static_cast<int>(zoom.Y * box.BRY + target.Y);

    return mousePos->TLX >= tlX && mousePos->TLX < brX &&
           mousePos->TLY >= tlY && mousePos->TLY < brY;
}

// ============================================================================
// Part 2: Virtual HUD Canvas + Native-Resolution Primitive Rescaling
// ============================================================================
static C2DExtentsI* __fastcall hk_GetRenderTargetDimensions(void* _this, void* edx, C2DExtentsI* outDims) {
    if (IsHudScalingActive() && outDims) {
        outDims->X = tl_virtualW;
        outDims->Y = tl_virtualH;
        return outDims;
    }
    return o_GetRTDims(_this, edx, outDims);
}

static void* __fastcall hk_PlayerGuiDrawPrepare(void* _this, void* edx, const void* info) {
    HudScopeGuard scope;
    return o_PlayerGuiDrawPrepare(_this, edx, info);
}

static char __fastcall hk_SubtitleDrawPrepare(void* _this, void* edx, const void* info) {
    HudScopeGuard scope;
    return o_SubtitleDrawPrepare(_this, edx, info);
}

static char __fastcall hk_ReceivedItemsDraw(void* _this, void* edx, const void* info) {
    HudScopeGuard scope;
    return o_ReceivedItemsDraw(_this, edx, info);
}

static void __fastcall hk_HeroReceiveItems_PrepareToShowItems(void* _this, void* edx, const void* pos) {
    auto* base = reinterpret_cast<uint8_t*>(_this);
    void* listHead = *reinterpret_cast<void* const*>(base + 0x0C);
    const bool hasItems = (listHead != nullptr && *reinterpret_cast<void* const*>(listHead) != listHead);
    const bool wasDisplaying = (*(base + 0x26) != 0);

    o_HeroReceiveItemsPrepareToShow(_this, edx, pos);

    // PrepareToShowItems writes real screen coordinates via ConvertWorldPosToScreenPos /
    // GetAVisibleTelevisionScreenArea; convert to virtual HUD coordinates so GlobalUpdate
    // and 2DStaticMesh/2DAnimMesh::Update don't double-scale the preview ClipBox.
    if (hasItems && !wasDisplaying) {
        RefreshHudTransformFromDisplayManager();
        if (tl_hudScaleX > 0.0f && tl_hudScaleY > 0.0f) {
            const float invX = 1.0f / tl_hudScaleX;
            const float invY = 1.0f / tl_hudScaleY;
            for (size_t offset : { size_t(0x3C), size_t(0x4C), size_t(0x5C) }) {
                auto* box = reinterpret_cast<C2DBoxF*>(base + offset);
                box->TLX *= invX;
                box->TLY *= invY;
                box->BRX *= invX;
                box->BRY *= invY;
            }
        }
    }
}

static char __fastcall hk_HeroReceiveItems_GlobalUpdate(void* _this, void* edx, bool paused) {
    HudScopeGuard scope;
    return o_HeroReceiveItemsGlobalUpdate(_this, edx, paused);
}

static int __fastcall hk_BettingHUDDraw(void* _this, void* edx, const void* info) {
    HudScopeGuard scope;
    return o_BettingHUDDraw(_this, edx, info);
}

static void* __fastcall hk_Draw2DTextFlush(void* _this, void* edx, const void* info) {
    HudScopeGuard scope;
    return o_Draw2DTextFlush(_this, edx, info);
}

static char __fastcall hk_ScreenFadeDraw(void* _this, void* edx, const void* info) {
    HudExcludeGuard exclude;
    return o_ScreenFadeDraw(_this, edx, info);
}

static int __fastcall hk_LetterBoxDraw(void* _this, void* edx, const void* info) {
    HudExcludeGuard exclude;
    return o_LetterBoxDraw(_this, edx, info);
}

static void __fastcall hk_TavernDrawBetAndMoney(void* _this, void* edx, void* info, float a, float b, float c, float d, float e, float f) {
    HudScopeGuard scope;
    o_TavernDrawBetAndMoney(_this, edx, info, a, b, c, d, e, f);
}

static void __fastcall hk_TavernDrawTextLabel(void* _this, void* edx, float a2, float a3, float a4, int a5, void* a6, int a7) {
    HudScopeGuard scope;
    o_TavernDrawTextLabel(_this, edx, a2, a3, a4, a5, a6, a7);
}

static void __fastcall hk_TavernDrawInfoBox(void* _this, void* edx, void* info) {
    HudScopeGuard scope;
    o_TavernDrawInfoBox(_this, edx, info);
}

inline void ScaleClipBoxAt(void* obj, size_t offset) {
    auto* box = reinterpret_cast<C2DBoxF*>(reinterpret_cast<uint8_t*>(obj) + offset);
    box->TLX *= tl_hudScaleX;
    box->TLY *= tl_hudScaleY;
    box->BRX *= tl_hudScaleX;
    box->BRY *= tl_hudScaleY;
}

inline bool ShouldFilterScaledHud() {
    return g_Config.FilterScaledSprites &&
           (std::fabsf(tl_hudScaleX - 1.0f) > 0.01f || std::fabsf(tl_hudScaleY - 1.0f) > 0.01f);
}

static char __fastcall hk_2DSpriteUpdate(void* _this, void* edx, const void* src, uint32_t flags) {
    char ret = o_2DSpriteUpdate(_this, edx, src, flags);
    if (IsHudScalingActive()) {
        ScaleClipBoxAt(_this, 72);
        if (ShouldFilterScaledHud()) {
            *(reinterpret_cast<uint8_t*>(_this) + 116) = 1;
        }
    }
    return ret;
}

static uint32_t __fastcall hk_2DClockSpriteUpdate(void* _this, void* edx, const void* src, uint32_t flags) {
    uint32_t ret = o_2DClockSpriteUpdate(_this, edx, src, flags);
    if (IsHudScalingActive()) {
        ScaleClipBoxAt(_this, 72);
        if (ShouldFilterScaledHud()) {
            *(reinterpret_cast<uint8_t*>(_this) + 124) = 1;
        }
    }
    return ret;
}

static int __fastcall hk_2DTextUpdate(void* _this, void* edx, const void* src, uint32_t flags) {
    int ret = o_2DTextUpdate(_this, edx, src, flags);
    if (IsHudScalingActive()) {
        auto* base  = reinterpret_cast<uint8_t*>(_this);
        auto* pos   = reinterpret_cast<C2DVector*>(base + 72);
        auto* scale = reinterpret_cast<float*>(base + 84);
        pos->X *= tl_hudScaleX;
        pos->Y *= tl_hudScaleY;
        *scale *= tl_hudScaleY;
    }
    return ret;
}

static int __fastcall hk_2DStaticMeshUpdate(void* _this, void* edx, const void* src, uint32_t flags) {
    int ret = o_2DStaticMeshUpdate(_this, edx, src, flags);
    if (IsHudScalingActive()) {
        ScaleClipBoxAt(_this, 176);
    }
    return ret;
}

static int __fastcall hk_2DAnimMeshUpdate(void* _this, void* edx, const void* src, char flags) {
    int ret = o_2DAnimMeshUpdate(_this, edx, src, flags);
    if (IsHudScalingActive()) {
        ScaleClipBoxAt(_this, 304);
    }
    return ret;
}

static int __fastcall hk_Set2DPosition(void* _this, void* edx, const C2DVector* pos, int layer) {
    if (tl_uiParticleScopeDepth > 0) {
        SetParticleEmitterScaleMode(_this, ParticleScaleMode::Menu);
        return o_Set2DPosition(_this, edx, pos, layer);
    }
    if (IsHudScalingActive()) {
        SetParticleEmitterScaleMode(_this, ParticleScaleMode::Hud);
        if (pos) {
            C2DVector scaled = { pos->X * tl_hudScaleX, pos->Y * tl_hudScaleY };
            return o_Set2DPosition(_this, edx, &scaled, layer);
        }
    }
    return o_Set2DPosition(_this, edx, pos, layer);
}

static int __fastcall hk_UIParticleEffectInitialise(void* _this, void* edx) {
    UIParticleScopeGuard scope;
    return o_UIParticleEffectInitialise(_this, edx);
}

static int __fastcall hk_UIParticleEffectChangeState(void* _this, void* edx, uint32_t state) {
    UIParticleScopeGuard scope;
    return o_UIParticleEffectChangeState(_this, edx, state);
}

static int __fastcall hk_UIParticleEffectDraw(void* _this, void* edx, int a2, int a3, int a4, int a5, int a6) {
    UIParticleScopeGuard scope;
    return o_UIParticleEffectDraw(_this, edx, a2, a3, a4, a5, a6);
}

static void __fastcall hk_ParticleEmitterDtor(void* _this, void* edx) {
    RemoveParticleEmitterScaleMode(_this);
    o_ParticleEmitterDtor(_this, edx);
}

static char __fastcall hk_ParticleEmitterPreparePrimitives(void* _this, void* edx, float interpolate, const void* info) {
    const float prevScale = tl_pending2DParticleScale;
    float scale = prevScale;
    switch (GetParticleEmitterScaleMode(_this)) {
        case ParticleScaleMode::Menu:
            scale = ComputeMenuUIScale();
            break;
        case ParticleScaleMode::Hud:
            RefreshHudTransformFromDisplayManager();
            scale = tl_hudScaleY;
            break;
        case ParticleScaleMode::None:
        default:
            // Preserve parent scale if PreparePrimitives recurses into a child emitter.
            scale = prevScale;
            break;
    }

    tl_pending2DParticleScale = scale;
    char ret = o_ParticleEmitterPreparePrimitives(_this, edx, interpolate, info);
    tl_pending2DParticleScale = prevScale;
    return ret;
}

static int __fastcall hk_2DParticleGroupUpdate(void* _this, void* edx, const void* src, uint32_t flags) {
    int ret = o_2DParticleGroupUpdate(_this, edx, src, flags);
    if (tl_pending2DParticleScale != 1.0f && _this) {
        // CEngineInternalPrimitive2DParticleGroup::Update copies 0x38 bytes starting at +176 (0xB0):
        // 12 floats for InverseTransform (48 bytes) followed by Pos.X/Pos.Y (and Pos.Z at +232).
        float* invTransform = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(_this) + 176);
        for (int i = 0; i < 12; ++i) {
            invTransform[i] *= tl_pending2DParticleScale;
        }
    }
    return ret;
}

static void __fastcall hk_SetVWindow(void* _this, void* edx, const C2DBoxF* twindow) {
    // Viewport boxes must always map against true render-target dimensions, never virtual HUD dimensions.
    HudExcludeGuard exclude;
    o_SetVWindow(_this, edx, twindow);
}

static char __fastcall hk_RegionGlobalDraw(void* _this, void* edx, int interp) {
    HudScopeGuard scope;
    return o_RegionGlobalDraw(_this, edx, interp);
}

static void __fastcall hk_DrawRetailDisplay(void* _this, void* edx, const void* textToDisplay, float percentComplete, bool forcePercentage) {
    HudScopeGuard scope;
    o_DrawRetailDisplay(_this, edx, textToDisplay, percentComplete, forcePercentage);
}

static void __fastcall hk_AddText(void* _this, void* edx, const void* pos, float scale, const void* wideStr, const void* font, const void* colour, uint32_t flags) {
    // QuickDraw2DTriInfo vector in CRenderManager2D: _Myfirst at +0x3E94, _Mylast at +0x3E98 (60 bytes/entry).
    constexpr size_t kTriInfoVecOffset = 0x3E94;
    constexpr size_t kTriInfoStride = 60;
    auto** myFirstPtr = reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(_this) + kTriInfoVecOffset);
    auto** myLastPtr  = reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(_this) + kTriInfoVecOffset + 4);
    const size_t countBefore = (*myLastPtr >= *myFirstPtr) ? static_cast<size_t>(*myLastPtr - *myFirstPtr) / kTriInfoStride : 0;

    o_AddText(_this, edx, pos, scale, wideStr, font, colour, flags);

    if (IsHudScalingActive()) {
        uint8_t* first = *myFirstPtr;
        uint8_t* last  = *myLastPtr;
        const size_t countAfter = (last >= first) ? static_cast<size_t>(last - first) / kTriInfoStride : 0;
        if (first && countAfter > countBefore) {
            uint8_t* entry = last - kTriInfoStride;
            *reinterpret_cast<float*>(entry + 0x20) *= tl_hudScaleX; // StringX
            *reinterpret_cast<float*>(entry + 0x24) *= tl_hudScaleY; // ZAverage (screen Y)
            *reinterpret_cast<float*>(entry + 0x2C) *= tl_hudScaleY; // StringScale
        }
    }
}

static void __fastcall hk_Add2DTri(void* _this, void* edx, const CTVertexRHWColSpecTex1* v1, const CTVertexRHWColSpecTex1* v2, const CTVertexRHWColSpecTex1* v3, uint32_t flags, void* texture) {
    if (IsHudScalingActive() && v1 && v2 && v3) {
        CTVertexRHWColSpecTex1 sv1 = *v1;
        CTVertexRHWColSpecTex1 sv2 = *v2;
        CTVertexRHWColSpecTex1 sv3 = *v3;
        sv1.X *= tl_hudScaleX; sv1.Y *= tl_hudScaleY;
        sv2.X *= tl_hudScaleX; sv2.Y *= tl_hudScaleY;
        sv3.X *= tl_hudScaleX; sv3.Y *= tl_hudScaleY;
        o_Add2DTri(_this, edx, &sv1, &sv2, &sv3, flags, texture);
        return;
    }
    o_Add2DTri(_this, edx, v1, v2, v3, flags, texture);
}

// ============================================================================
// Hook Registration
// ============================================================================
template <typename T>
static void HookAddr(uintptr_t idaAddr, void* detour, T** original) {
    LPVOID target = reinterpret_cast<LPVOID>(Rebase(idaAddr));
    if (MH_CreateHook(target, detour, reinterpret_cast<LPVOID*>(original)) == MH_OK) {
        MH_EnableHook(target);
    }
}

static DWORD WINAPI InitThread(LPVOID lpParam) {
    HMODULE hSelf = reinterpret_cast<HMODULE>(lpParam);
    g_ExeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    InitIniPath(hSelf);
    LoadConfigFromIni(true);

    if (MH_Initialize() != MH_OK) {
        return 1;
    }

    // NUISystem hooks
    HookAddr(Retail::ADDR_NUISYSTEM_GETUISCALE,          &hk_GetUIScale,                &o_GetUIScale);
    HookAddr(Retail::ADDR_CHOVERABLE_ISHOVERED,          &hk_CHoverable_IsHovered,      &o_IsHovered);

    // Virtual HUD scope hooks
    HookAddr(Retail::ADDR_CDISPLAYMANAGER_GETRTDIMS,     &hk_GetRenderTargetDimensions, &o_GetRTDims);
    HookAddr(Retail::ADDR_CPLAYERGUI_DRAWPREPARE,        &hk_PlayerGuiDrawPrepare,      &o_PlayerGuiDrawPrepare);
    HookAddr(Retail::ADDR_CSUBTITLERENDERER_DRAWPREPARE, &hk_SubtitleDrawPrepare,       &o_SubtitleDrawPrepare);
    HookAddr(Retail::ADDR_CDRAWRECEIVEDITEMS_DRAW,       &hk_ReceivedItemsDraw,         &o_ReceivedItemsDraw);
    HookAddr(Retail::ADDR_CTCHERORECEIVEITEMS_PREPARE,   &hk_HeroReceiveItems_PrepareToShowItems, &o_HeroReceiveItemsPrepareToShow);
    HookAddr(Retail::ADDR_CTCHERORECEIVEITEMS_GLOBALUPD, &hk_HeroReceiveItems_GlobalUpdate,       &o_HeroReceiveItemsGlobalUpdate);
    HookAddr(Retail::ADDR_CDRAWBETTINGHUD_DRAW,          &hk_BettingHUDDraw,            &o_BettingHUDDraw);
    HookAddr(Retail::ADDR_CDRAW2DTEXT_DRAW,              &hk_Draw2DTextFlush,           &o_Draw2DTextFlush);
    HookAddr(Retail::ADDR_CDRAWSCREENFADE_DRAW,          &hk_ScreenFadeDraw,            &o_ScreenFadeDraw);
    HookAddr(Retail::ADDR_CDRAWLETTERBOX_DRAW,           &hk_LetterBoxDraw,             &o_LetterBoxDraw);
    HookAddr(Retail::ADDR_TAVERN_DRAWBETANDMONEY,        &hk_TavernDrawBetAndMoney,     &o_TavernDrawBetAndMoney);
    HookAddr(Retail::ADDR_TAVERN_DRAWTEXTLABEL,          &hk_TavernDrawTextLabel,       &o_TavernDrawTextLabel);
    HookAddr(Retail::ADDR_TAVERN_DRAWINFOBOX,            &hk_TavernDrawInfoBox,         &o_TavernDrawInfoBox);

    // CIEngine 2D primitive Update & 2D Particle hooks
    HookAddr(Retail::ADDR_PRIM_2DSPRITE_UPDATE,          &hk_2DSpriteUpdate,                   &o_2DSpriteUpdate);
    HookAddr(Retail::ADDR_PRIM_2DCLOCKSPRITE_UPDATE,     &hk_2DClockSpriteUpdate,              &o_2DClockSpriteUpdate);
    HookAddr(Retail::ADDR_PRIM_2DTEXT_UPDATE,            &hk_2DTextUpdate,                     &o_2DTextUpdate);
    HookAddr(Retail::ADDR_PRIM_2DSTATICMESH_UPDATE,      &hk_2DStaticMeshUpdate,               &o_2DStaticMeshUpdate);
    HookAddr(Retail::ADDR_PRIM_2DANIMMESH_UPDATE,        &hk_2DAnimMeshUpdate,                 &o_2DAnimMeshUpdate);
    HookAddr(Retail::ADDR_PRIM_2DPARTICLEGROUP_UPDATE,   &hk_2DParticleGroupUpdate,            &o_2DParticleGroupUpdate);
    HookAddr(Retail::ADDR_CPARTICLEEMITTER_SET2DPOS,     &hk_Set2DPosition,                    &o_Set2DPosition);
    HookAddr(Retail::ADDR_CPARTICLEEMITTER_PREPPARAMS,   &hk_ParticleEmitterPreparePrimitives, &o_ParticleEmitterPreparePrimitives);
    HookAddr(Retail::ADDR_CPARTICLEEMITTER_DTOR,         &hk_ParticleEmitterDtor,              &o_ParticleEmitterDtor);
    HookAddr(Retail::ADDR_UIPARTICLEEFFECT_INITIALISE,   &hk_UIParticleEffectInitialise,       &o_UIParticleEffectInitialise);
    HookAddr(Retail::ADDR_UIPARTICLEEFFECT_CHANGESTATE,  &hk_UIParticleEffectChangeState,      &o_UIParticleEffectChangeState);
    HookAddr(Retail::ADDR_UIPARTICLEEFFECT_DRAW,         &hk_UIParticleEffectDraw,             &o_UIParticleEffectDraw);

    // Viewport exclusion & CRenderManager2D QuickDraw scaling
    HookAddr(Retail::ADDR_CRENDERMANAGERCORE_SETVWINDOW, &hk_SetVWindow,                &o_SetVWindow);
    HookAddr(Retail::ADDR_CTCREGIONDISPLAY_GLOBALDRAW,   &hk_RegionGlobalDraw,          &o_RegionGlobalDraw);
    HookAddr(Retail::ADDR_CPROGRESSDISPLAY_DRAWRETAIL,   &hk_DrawRetailDisplay,         &o_DrawRetailDisplay);
    HookAddr(Retail::ADDR_CRENDERMANAGER2D_ADDTEXT,      &hk_AddText,                   &o_AddText);
    HookAddr(Retail::ADDR_CRENDERMANAGER2D_ADD2DTRI,     &hk_Add2DTri,                  &o_Add2DTri);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
