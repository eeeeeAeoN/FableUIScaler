#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
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

    // HUD 2D Particles
    constexpr uintptr_t ADDR_CPARTICLEEMITTER_SET2DPOS     = 0x00AED090;

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

using GetBackBufferDim_t = double(__cdecl*)();
using GetUIScale_t       = C2DVector* (__fastcall*)(void* _this, void* edx, C2DVector* outResult);
using IsHovered_t        = bool(__fastcall*)(void* _this, void* edx, const C2DBoxI* mousePos);

using GetRenderTargetDimensions_t = C2DExtentsI* (__fastcall*)(void* _this, void* edx, C2DExtentsI* outDims);

using VoidPrep_t         = void* (__fastcall*)(void* _this, void* edx, const void* info);
using CharPrep_t         = char(__fastcall*)(void* _this, void* edx, const void* info);
using IntPrep_t          = int(__fastcall*)(void* _this, void* edx, const void* info);

using TavernDrawBetAndMoney_t = void(__fastcall*)(void* _this, void* edx, void* info, float a, float b, float c, float d, float e, float f);
using TavernDrawTextLabel_t   = void(__fastcall*)(void* _this, void* edx, float a2, float a3, float a4, int a5, void* a6, int a7);
using TavernDrawInfoBox_t     = void(__fastcall*)(void* _this, void* edx, void* info);

using PrimUpdateChar_t   = char(__fastcall*)(void* _this, void* edx, const void* src, uint32_t flags);
using PrimUpdateUint_t   = uint32_t(__fastcall*)(void* _this, void* edx, const void* src, uint32_t flags);
using PrimUpdateInt_t    = int(__fastcall*)(void* _this, void* edx, const void* src, uint32_t flags);
using PrimUpdateAnim_t   = int(__fastcall*)(void* _this, void* edx, const void* src, char flags);
using Set2DPosition_t    = int(__fastcall*)(void* _this, void* edx, const C2DVector* pos, int layer);

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
static Set2DPosition_t             o_Set2DPosition = nullptr;

static SetVWindow_t                o_SetVWindow = nullptr;
static RegionGlobalDraw_t          o_RegionGlobalDraw = nullptr;
static DrawRetailDisplay_t         o_DrawRetailDisplay = nullptr;
static AddText_t                   o_AddText = nullptr;
static Add2DTri_t                  o_Add2DTri = nullptr;

thread_local int   tl_hudScopeDepth   = 0;
thread_local int   tl_hudExcludeDepth = 0;
thread_local int   tl_virtualW        = 1280;
thread_local int   tl_virtualH        = 720;
thread_local float tl_hudScaleX       = 1.0f;
thread_local float tl_hudScaleY       = 1.0f;

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

static C2DVector* __fastcall hk_GetUIScale(void* /*_this*/, void* /*edx*/, C2DVector* outResult) {
    LoadConfigFromIni(false);

    void* mainGame = *reinterpret_cast<void* volatile*>(Rebase(Retail::ADDR_PMAINGAMECOMPONENT));
    if (!g_Config.ScaleMainMenu && mainGame == nullptr) {
        outResult->X = 1.0f;
        outResult->Y = 1.0f;
        return outResult;
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

    float finalScale = ClampScale(baseScale * g_Config.UIScaleMultiplier * g_Config.MenuScaleMultiplier);
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
    if (IsHudScalingActive() && pos) {
        C2DVector scaled = { pos->X * tl_hudScaleX, pos->Y * tl_hudScaleY };
        return o_Set2DPosition(_this, edx, &scaled, layer);
    }
    return o_Set2DPosition(_this, edx, pos, layer);
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
    HookAddr(Retail::ADDR_CDRAWBETTINGHUD_DRAW,          &hk_BettingHUDDraw,            &o_BettingHUDDraw);
    HookAddr(Retail::ADDR_CDRAW2DTEXT_DRAW,              &hk_Draw2DTextFlush,           &o_Draw2DTextFlush);
    HookAddr(Retail::ADDR_CDRAWSCREENFADE_DRAW,          &hk_ScreenFadeDraw,            &o_ScreenFadeDraw);
    HookAddr(Retail::ADDR_CDRAWLETTERBOX_DRAW,           &hk_LetterBoxDraw,             &o_LetterBoxDraw);
    HookAddr(Retail::ADDR_TAVERN_DRAWBETANDMONEY,        &hk_TavernDrawBetAndMoney,     &o_TavernDrawBetAndMoney);
    HookAddr(Retail::ADDR_TAVERN_DRAWTEXTLABEL,          &hk_TavernDrawTextLabel,       &o_TavernDrawTextLabel);
    HookAddr(Retail::ADDR_TAVERN_DRAWINFOBOX,            &hk_TavernDrawInfoBox,         &o_TavernDrawInfoBox);

    // CIEngine 2D primitive Update hooks
    HookAddr(Retail::ADDR_PRIM_2DSPRITE_UPDATE,          &hk_2DSpriteUpdate,            &o_2DSpriteUpdate);
    HookAddr(Retail::ADDR_PRIM_2DCLOCKSPRITE_UPDATE,     &hk_2DClockSpriteUpdate,       &o_2DClockSpriteUpdate);
    HookAddr(Retail::ADDR_PRIM_2DTEXT_UPDATE,            &hk_2DTextUpdate,              &o_2DTextUpdate);
    HookAddr(Retail::ADDR_PRIM_2DSTATICMESH_UPDATE,      &hk_2DStaticMeshUpdate,        &o_2DStaticMeshUpdate);
    HookAddr(Retail::ADDR_PRIM_2DANIMMESH_UPDATE,        &hk_2DAnimMeshUpdate,          &o_2DAnimMeshUpdate);
    HookAddr(Retail::ADDR_CPARTICLEEMITTER_SET2DPOS,     &hk_Set2DPosition,             &o_Set2DPosition);

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
