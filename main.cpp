// GTA:SA GPS -> ARM64 (libGTASA.so 2.10) port, milestone 1.
//
// What changed versus the 32-bit 1.2.1 source:
//  * No GTASA_STRUCTS.h: every game struct is read by raw offsets that were
//    extracted from the DWARF info of the 64-bit libGTASA.so (see OFF_* below).
//  * No hardcoded libGTASA.so offsets / PLT hooks: everything is resolved by symbol.
//  * The block of patches that enlarges the path-node limits and the node
//    streaming radius is NOT ported yet (those were Thumb-2 patches). Without it
//    the route is only found over nodes that the game has already streamed in.
//  * The old PreRenderEnd / InitRenderWare hooks are gone, text and init now live
//    in the DrawRadarGangOverlay hook.
//  * Drawing on the pause map is disabled for now (the menu class was rewritten in 2.x).
//
// Offsets marked (UNVERIFIED) were deduced from structs and not yet tested in game.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <float.h>
#include <stdarg.h>
#include <string.h>

#define MAX_NODE_POINTS 2000
#define MAX_RADAR_TRACES 250

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#define GPS_LINE_R      235
#define GPS_LINE_G      212
#define GPS_LINE_B      0
#define GPS_LINE_A      255

MYMODCFG(net.dk22pac.rusjj.gps, GTA:SA GPS, 1.2.1-arm64, DK22Pac & juicermv & RusJJ)
NEEDGAME(com.rockstargames.gtasa)

/* ------------------------------------------------------------------ */
/* Minimal types                                                      */
/* ------------------------------------------------------------------ */
struct CVector { float x, y, z; };
struct CVector2D { float x, y; };
struct CRect { float left, top, right, bottom; };
struct CNodeAddress { uint16_t region, index; };
struct RwOpenGLVertex { float x, y, z, rhw; uint8_t r, g, b, a; float s, t; }; // 28 bytes
struct RGBA { uint8_t r, g, b, a; };

template<typename T> static inline T& At(void* base, size_t off) { return *(T*)((uint8_t*)base + off); }

/* ------------------------------------------------------------------ */
/* Offsets in the 64-bit game (from DWARF of libGTASA.so 2.10)        */
/* ------------------------------------------------------------------ */
// CPathFind (ThePaths): pNodes[region] pointer table; CPathNode is 40 bytes
#define OFF_PATHS_NODES         4104
#define SIZEOF_PATHNODE         40
#define OFF_NODE_X              16   // int16, /8.0f
#define OFF_NODE_Y              18   // int16, /8.0f
// RsGlobal
#define OFF_RS_WIDTH            8
#define OFF_RS_HEIGHT           12
// MobileMenu
#define OFF_MENU_WAYPOINT_BLIP  104  // int (UNVERIFIED that this is the map target blip)
// CWidget
#define WIDGET_RADAR            161
#define OFF_WIDGET_ENABLED      89   // bool m_bEnabled
// sRadarTrace (48 bytes, 250 entries)
#define SIZEOF_TRACE            48
#define OFF_TR_COLOUR           0    // uint32
#define OFF_TR_POOLINDEX        4    // int  (entity handle)
#define OFF_TR_POS              8    // CVector
#define OFF_TR_REFINDEX         20   // uint16
#define OFF_TR_SPRITE           40   // int8
#define OFF_TR_FLAGS1           41   // bit 3 (>>3 &1) = bFriendly
#define OFF_TR_FLAGS2           42   // &3 = display flag, (>>2)&0xF = blip type
// CPickup (36 bytes, 620 entries)
#define SIZEOF_PICKUP           36
#define OFF_PICKUP_X            20   // int16, /8.0f
#define OFF_PICKUP_Y            22
#define OFF_PICKUP_Z            24
#define MAX_PICKUPS             620
// CPed / CVehicle
#define OFF_PED_FLAGS           1412 // CPedFlags, bInVehicle = bit 8 (UNVERIFIED bit index)
#define OFF_PED_VEHICLE         1800 // m_pMyVehicle
#define OFF_VEH_BASETYPE        1844 // m_baseVehicleType
#define OFF_VEH_TYPE            1848 // m_vehicleType
// CPlaceable
#define OFF_PLC_TRANSFORM       8    // CSimpleTransform: CVector translate
#define OFF_PLC_MATRIX          24   // CMatrix*; translation at +48

enum { VEH_HELI = 3, VEH_PLANE = 4, VEH_BOAT = 5, VEH_BMX = 10 };
enum { BLIP_CAR = 1, BLIP_CHAR = 2, BLIP_OBJECT = 3, BLIP_COORD = 4, BLIP_CONTACT_POINT = 5, BLIP_PICKUP = 7 };
enum { BLIP_COLOUR_RED = 0, BLIP_COLOUR_GREEN, BLIP_COLOUR_BLUE, BLIP_COLOUR_WHITE, BLIP_COLOUR_YELLOW,
       BLIP_COLOUR_PURPLE, BLIP_COLOUR_CYAN, BLIP_COLOUR_THREAT, BLIP_COLOUR_DESTINATION };
enum { ALIGN_CENTER = 0, ALIGN_LEFT = 1, ALIGN_RIGHT = 2 };
#define FONT_SUBTITLES 1
#define rwRENDERSTATETEXTURERASTER 1
#define rwPRIMTYPETRISTRIP 4

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */
uintptr_t pGTASA;
void* hGTASA;

CNodeAddress resultNodes[MAX_NODE_POINTS];
CVector2D nodePoints[MAX_NODE_POINTS];
RwOpenGLVertex lineVerts[MAX_NODE_POINTS * 4];
float gpsDistance;
CVector2D gpsDistanceTextPos;
CRect emptyRect = {0, 0, 0, 0}, radarRect;
RGBA gpsLineColor = {GPS_LINE_R, GPS_LINE_G, GPS_LINE_B, GPS_LINE_A};
float lineWidth = 3.5f, textOffset, textScale;
CVector2D vecTextOffset;
uint8_t g_nTextAlignment;
int g_targetBlip = 0;
void* g_pMissionTrace = NULL;
bool g_bInited = false, g_bTextInited = false;

CVector2D g_vecUnderRadar = {0.0f, -1.05f};
CVector2D g_vecAboveRadar = {0.0f, 1.05f};
CVector2D g_vecLeftRadar  = {-1.05f, 0.0f};
CVector2D g_vecRightRadar = {1.05f, 0.0f};

// Config
ConfigEntry *pCfgClosestMaxGPSDistance, *pCfgGPSLineColorRGB, *pCfgGPSLineWidth, *pCfgGPSDrawDistance,
            *pCfgGPSDrawDistancePosition, *pCfgGPSDrawDistanceTextScale, *pCfgGPSDrawDistanceTextOffset;
bool bAllowBMX, bAllowBoat, bAllowMission, bImperialUnits;

// Game vars (all are objects/arrays exported by libGTASA.so, so we hold their addresses)
uint8_t* ThePaths;
uint8_t* gMobileMenu;
uint8_t* RsGlobal;
uint8_t* pRadarTrace;     // sRadarTrace[250]
uint8_t* aPickUps;        // CPickup[620]
void**   aWidgets;        // CWidget*[]
float *NearScreenZ, *RecipNearClip;
bool *m_UserPause, *m_CodePause;

// Game funcs
void* (*FindPlayerPed)(int);
float (*FindGroundZForCoord)(float, float);
int (*DoPathSearch)(uintptr_t, unsigned char, CVector, CNodeAddress, CVector, CNodeAddress*, short*, int, float*, float, CNodeAddress*, float, bool, CNodeAddress, bool, bool);
void (*TransformRealWorldPointToRadarSpace)(CVector2D& out, CVector2D const& in);
void (*TransformRadarPointToScreenSpace)(CVector2D& out, CVector2D const& in);
void (*LimitRadarPoint)(CVector2D& in);
void (*RwRenderStateSet)(int, void*);
void (*RwIm2DRenderPrimitive)(int, RwOpenGLVertex*, int);
void (*SetScissorRect)(CRect&);
void (*ClearRadarBlip)(int);
bool (*IsOnAMission)();
void* (*GetPoolPed)(int);
void* (*GetPoolVeh)(int);
void* (*GetPoolObj)(int);

void (*FontSetOrientation)(unsigned char);
void (*FontSetColor)(uint32_t);          // CRGBA by value == one 32-bit register
void (*FontSetBackground)(unsigned char, unsigned char);
void (*FontSetWrapx)(float);
void (*FontSetStyle)(unsigned char);
void (*FontSetScale)(float);
void (*FontSetProportional)(unsigned char);
void (*FontSetDropShadowPosition)(char);
void (*FontPrintString)(float, float, unsigned short*);
void (*AsciiToGxtChar)(const char* txt, unsigned short* ret);
void (*RenderFontBuffer)(void);

/* ------------------------------------------------------------------ */
/* Debug log (file in the game's own external dir, always writable)   */
/* ------------------------------------------------------------------ */
static FILE* g_dbg = NULL;
static unsigned g_frame = 0;
static bool g_logNow = false;
static void Dbg(const char* fmt, ...)
{
    if(!g_dbg)
    {
        // The path comes from AML, so it is right for clones / dual space as well.
        char path[512];
        const char* root = aml->GetAndroidDataPath();
        if(root && *root)
            snprintf(path, sizeof(path), "%s%sgps_debug.txt", root, root[strlen(root) - 1] == '/' ? "" : "/");
        else
            snprintf(path, sizeof(path), "/storage/emulated/0/Android/data/com.rockstargames.gtasa/files/gps_debug.txt");
        g_dbg = fopen(path, "w");
        logger->Info("GPS: debug log -> %s (%s)", path, g_dbg ? "ok" : "cannot open");
        if(!g_dbg) return;
    }
    va_list a; va_start(a, fmt); vfprintf(g_dbg, fmt, a); va_end(a);
    fputc('\n', g_dbg); fflush(g_dbg);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static inline uint32_t PackRGBA(RGBA c) { return (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)c.a << 24); }
static inline bool IsRGBValue(int v) { return v >= 0 && v <= 255; }
static inline bool IsGamePaused() { return *m_CodePause || *m_UserPause; }
static inline bool IsRadarVisible()
{
    void* radar = aWidgets[WIDGET_RADAR];
    return radar != NULL && At<bool>(radar, OFF_WIDGET_ENABLED);
}
static inline uint8_t* TraceAt(int i) { return pRadarTrace + SIZEOF_TRACE * i; }

static CVector GetEntityPos(void* ent)
{
    CVector v = {0, 0, 0};
    if(!ent) return v;
    uint8_t* mat = At<uint8_t*>(ent, OFF_PLC_MATRIX);
    if(mat) { v.x = At<float>(mat, 48); v.y = At<float>(mat, 52); v.z = At<float>(mat, 56); }
    else    { v = At<CVector>(ent, OFF_PLC_TRANSFORM); }
    return v;
}

static inline bool VehIs(void* veh, int type)
{
    // We do not know yet which of the two fields carries the "sub type" the old
    // code used, so a match in either one counts.
    return At<int>(veh, OFF_VEH_BASETYPE) == type || At<int>(veh, OFF_VEH_TYPE) == type;
}

static void* GetSupportedVehicle(void* player)
{
    if(!player) return NULL;
    void* veh = At<void*>(player, OFF_PED_VEHICLE);
    if(!veh) return NULL;
    if(!(At<uint32_t>(player, OFF_PED_FLAGS) & (1u << 8))) return NULL; // bInVehicle
    if(VehIs(veh, VEH_PLANE) || VehIs(veh, VEH_HELI)) return NULL;
    if(!bAllowBMX && VehIs(veh, VEH_BMX)) return NULL;
    return veh;
}

static RGBA TraceRGBA(uint32_t clr, bool friendly)
{
    switch(clr)
    {
        case BLIP_COLOUR_RED:         return {127, 0, 0, 255};
        case BLIP_COLOUR_GREEN:       return {0, 127, 0, 255};
        case BLIP_COLOUR_BLUE:        return {0, 0, 127, 255};
        case BLIP_COLOUR_WHITE:       return {127, 127, 127, 255};
        case BLIP_COLOUR_YELLOW:      return {200, 200, 0, 255};
        case BLIP_COLOUR_PURPLE:      return {127, 0, 127, 255};
        case BLIP_COLOUR_CYAN:        return {0, 127, 127, 255};
        case BLIP_COLOUR_THREAT:      return friendly ? RGBA{0, 0, 127, 255} : RGBA{127, 0, 0, 255};
        case BLIP_COLOUR_DESTINATION: return {200, 200, 0, 255};
        default: return {(uint8_t)(clr >> 24), (uint8_t)(clr >> 16), (uint8_t)(clr >> 8), 255};
    }
}

static inline void Setup2DVertex(RwOpenGLVertex& v, float x, float y, RGBA c)
{
    v.x = x; v.y = y;
    v.z = *NearScreenZ + 0.0001f;
    v.rhw = *RecipNearClip;
    v.r = c.r; v.g = c.g; v.b = c.b; v.a = c.a;
    v.s = v.t = 0.0f;
}

static void InitializeConfigValues()
{
    float w = (float)At<int>(RsGlobal, OFF_RS_WIDTH), h = (float)At<int>(RsGlobal, OFF_RS_HEIGHT);
    textOffset = (8.0f * h) / 448.0f;
    textScale = (0.4f * w / 640.0f) * pCfgGPSDrawDistanceTextScale->GetFloat();
    if(sscanf(pCfgGPSDrawDistanceTextOffset->GetString(), "%f %f", &vecTextOffset.x, &vecTextOffset.y) != 2)
        vecTextOffset.x = vecTextOffset.y = 0;
}

static void SetDistanceTextValues()
{
    CVector2D posn, in = {-1.0f, -1.0f};
    TransformRadarPointToScreenSpace(posn, in);
    radarRect.left = posn.x + 2.0f;
    radarRect.bottom = posn.y - 2.0f;
    in = {1.0f, 1.0f};
    TransformRadarPointToScreenSpace(posn, in);
    radarRect.right = posn.x - 2.0f;
    radarRect.top = posn.y + 2.0f;

    switch(pCfgGPSDrawDistancePosition->GetInt())
    {
        default:
        case 0: g_nTextAlignment = ALIGN_CENTER; TransformRadarPointToScreenSpace(gpsDistanceTextPos, g_vecUnderRadar);
                gpsDistanceTextPos.x += vecTextOffset.x; gpsDistanceTextPos.y += vecTextOffset.y + textOffset; break;
        case 1: g_nTextAlignment = ALIGN_CENTER; TransformRadarPointToScreenSpace(gpsDistanceTextPos, g_vecAboveRadar);
                gpsDistanceTextPos.x += vecTextOffset.x; gpsDistanceTextPos.y += vecTextOffset.y - textOffset; break;
        case 2: g_nTextAlignment = ALIGN_RIGHT;  TransformRadarPointToScreenSpace(gpsDistanceTextPos, g_vecLeftRadar);
                gpsDistanceTextPos.x += vecTextOffset.x - textOffset; gpsDistanceTextPos.y += vecTextOffset.y; break;
        case 3: g_nTextAlignment = ALIGN_LEFT;   TransformRadarPointToScreenSpace(gpsDistanceTextPos, g_vecRightRadar);
                gpsDistanceTextPos.x += vecTextOffset.x + textOffset; gpsDistanceTextPos.y += vecTextOffset.y; break;
        case 4: gpsDistanceTextPos = vecTextOffset; break;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */
static char text[32];
static unsigned short textGxt[0xFF];

static void DrawDistanceText()
{
    if(!(gpsDistance > 0.0f) || !pCfgGPSDrawDistance->GetBool()) return;
    if(!g_bTextInited) { g_bTextInited = true; SetDistanceTextValues(); }

    if(gpsDistance == 100000.0f) snprintf(text, sizeof(text), "Far from the road!");
    else if(!bImperialUnits)
    {
        if(gpsDistance >= 1000.0f) snprintf(text, sizeof(text), "%.2fkm", 0.001f * gpsDistance);
        else snprintf(text, sizeof(text), "%dm", (int)gpsDistance);
    }
    else
    {
        if(gpsDistance > 1609.344f) snprintf(text, sizeof(text), "%.2fmil", 0.000621371192237334f * gpsDistance);
        else snprintf(text, sizeof(text), "%dyrd", (int)(gpsDistance * 1.094f));
    }
    AsciiToGxtChar(text, textGxt);

    RGBA col = {255, 255, 255, 255};
    if(!g_targetBlip && g_pMissionTrace)
        col = TraceRGBA(At<uint32_t>(g_pMissionTrace, OFF_TR_COLOUR), (At<uint8_t>(g_pMissionTrace, OFF_TR_FLAGS1) >> 3) & 1);

    FontSetOrientation(g_nTextAlignment);
    FontSetColor(PackRGBA(col));
    FontSetBackground(false, false);
    FontSetWrapx(500.0f);
    FontSetScale(textScale);
    FontSetStyle(FONT_SUBTITLES);
    FontSetProportional(true);
    FontSetDropShadowPosition(1);
    FontPrintString(gpsDistanceTextPos.x, gpsDistanceTextPos.y, textGxt);
    RenderFontBuffer();
}

static void DoPathDraw(CVector to, RGBA color, bool isTargetBlip = false, float* dist = NULL)
{
    void* player = FindPlayerPed(-1);
    void* veh = GetSupportedVehicle(player);
    if(!veh) { if(g_logNow) Dbg("DoPathDraw: no supported vehicle (player=%p)", player); return; }

    short nodesCount = 0;
    float trashVar;
    bool isBoat = VehIs(veh, VEH_BOAT);
    CNodeAddress invalid = {0xFFFF, 0xFFFF};

    DoPathSearch((uintptr_t)ThePaths, false, GetEntityPos(player), invalid, to, resultNodes, &nodesCount,
                 MAX_NODE_POINTS, dist ? dist : &trashVar, 1000000.0f, NULL, 1000000.0f, false,
                 invalid, false, isBoat && bAllowBoat);

    if(g_logNow) Dbg("DoPathDraw: DoPathSearch nodes=%d dist=%.1f target=(%.1f %.1f %.1f)", (int)nodesCount, dist ? *dist : trashVar, to.x, to.y, to.z);
    if(nodesCount <= 0) return;

    if(isTargetBlip && gpsDistance < pCfgClosestMaxGPSDistance->GetFloat())
    {
        ClearRadarBlip(g_targetBlip);
        At<int>(gMobileMenu, OFF_MENU_WAYPOINT_BLIP) = 0;
        g_targetBlip = 0;
        return;
    }

    short good = 0;
    for(short i = 0; i < nodesCount; ++i)
    {
        uint8_t* table = At<uint8_t*>(ThePaths, OFF_PATHS_NODES + 8 * resultNodes[i].region);
        if(!table) continue;
        uint8_t* node = table + SIZEOF_PATHNODE * resultNodes[i].index;
        CVector2D nodePos = { At<int16_t>(node, OFF_NODE_X) / 8.0f, At<int16_t>(node, OFF_NODE_Y) / 8.0f };
        TransformRealWorldPointToRadarSpace(nodePos, nodePos);
        TransformRadarPointToScreenSpace(nodePoints[good++], nodePos);
    }
    if(g_logNow) Dbg("DoPathDraw: good nodes=%d", (int)good);
    if(good < 2) return;

    if(!g_bTextInited) { g_bTextInited = true; SetDistanceTextValues(); }
    SetScissorRect(radarRect);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);

    unsigned int vertIndex = 0;
    short segs = good - 1;
    for(short i = 0; i < segs; i++)
    {
        CVector2D dir = { nodePoints[i + 1].x - nodePoints[i].x, nodePoints[i + 1].y - nodePoints[i].y };
        float angle = atan2f(dir.y, dir.x);
        CVector2D s0 = { cosf(angle - 1.5707963f) * lineWidth, sinf(angle - 1.5707963f) * lineWidth };
        CVector2D s1 = { cosf(angle + 1.5707963f) * lineWidth, sinf(angle + 1.5707963f) * lineWidth };
        Setup2DVertex(lineVerts[vertIndex++], nodePoints[i].x     + s0.x, nodePoints[i].y     + s0.y, color);
        Setup2DVertex(lineVerts[vertIndex++], nodePoints[i + 1].x + s0.x, nodePoints[i + 1].y + s0.y, color);
        Setup2DVertex(lineVerts[vertIndex++], nodePoints[i].x     + s1.x, nodePoints[i].y     + s1.y, color);
        Setup2DVertex(lineVerts[vertIndex++], nodePoints[i + 1].x + s1.x, nodePoints[i + 1].y + s1.y, color);
    }
    RwIm2DRenderPrimitive(rwPRIMTYPETRISTRIP, lineVerts, 4 * segs);
    SetScissorRect(emptyRect);
}

DECL_HOOKv(DrawRadarHook)
{
    DrawRadarHook();
    ++g_frame;
    g_logNow = (g_frame % 180) == 1;
    if(g_frame == 1) Dbg("hook called for the first time");

    if(!g_bInited)
    {
        if(At<int>(RsGlobal, OFF_RS_WIDTH) <= 0) return;
        g_bInited = true;
        InitializeConfigValues();
    }
    gpsDistance = 0;
    g_pMissionTrace = NULL;
    if(g_logNow)
    {
        void* pl = FindPlayerPed(-1);
        void* vh = pl ? At<void*>(pl, OFF_PED_VEHICLE) : NULL;
        Dbg("f=%u paused=%d radarVis=%d widget=%p menuBlip=%d player=%p veh=%p pedFlags=0x%x baseType=%d type=%d",
            g_frame, (int)IsGamePaused(), (int)IsRadarVisible(), aWidgets[WIDGET_RADAR], At<int>(gMobileMenu, OFF_MENU_WAYPOINT_BLIP),
            pl, vh, pl ? At<uint32_t>(pl, OFF_PED_FLAGS) : 0, vh ? At<int>(vh, OFF_VEH_BASETYPE) : -1, vh ? At<int>(vh, OFF_VEH_TYPE) : -1);
        int mb = At<int>(gMobileMenu, OFF_MENU_WAYPOINT_BLIP);
        if(mb != 0 && mb != -1 && (mb & 0xFFFF) < MAX_RADAR_TRACES)
        {
            uint8_t* t = TraceAt(mb & 0xFFFF);
            CVector tp = At<CVector>(t, OFF_TR_POS);
            Dbg("  blip: idx=%d refInTrace=%u refInHandle=%u pos=(%.1f %.1f %.1f) sprite=%d colour=%u",
                mb & 0xFFFF, (unsigned)At<uint16_t>(t, OFF_TR_REFINDEX), (unsigned)((uint32_t)mb >> 16), tp.x, tp.y, tp.z,
                (int)At<int8_t>(t, OFF_TR_SPRITE), (unsigned)At<uint32_t>(t, OFF_TR_COLOUR));
        }
    }
    if(IsGamePaused()) return; // pause-map drawing is not ported yet (radar-visible check dropped: widget slot is NULL in 2.10)

    // Player-set map waypoint
    int menuBlip = At<int>(gMobileMenu, OFF_MENU_WAYPOINT_BLIP);
    int idx = menuBlip & 0xFFFF;
    bool blipOk = false;
    if(menuBlip != 0 && menuBlip != -1 && idx < MAX_RADAR_TRACES)
    {
        CVector tp = At<CVector>(TraceAt(idx), OFF_TR_POS);
        bool refOk = At<uint16_t>(TraceAt(idx), OFF_TR_REFINDEX) == (uint16_t)((uint32_t)menuBlip >> 16);
        blipOk = refOk || tp.x != 0.0f || tp.y != 0.0f; // accept if the trace has a position even if the ref index differs
    }
    if(blipOk)
    {
        if(g_targetBlip != menuBlip)
        {
            g_targetBlip = menuBlip;
            logger->Info("GPS: target blip %d (index %d) picked up", menuBlip, idx);
        }
        CVector& bpos = At<CVector>(TraceAt(idx), OFF_TR_POS);
        if(bpos.z == 0) bpos.z = FindGroundZForCoord(bpos.x, bpos.y) + 5.0f;
        DoPathDraw(bpos, gpsLineColor, true, &gpsDistance);
    }
    else g_targetBlip = 0;

    if(bAllowMission && IsOnAMission())
    {
        void* player = FindPlayerPed(-1);
        unsigned short count = 0, maxi = 0;
        float distances[MAX_RADAR_TRACES];
        uint8_t* traces[MAX_RADAR_TRACES];
        CVector ppos = GetEntityPos(player);

        for(int i = 0; i < MAX_RADAR_TRACES; ++i)
        {
            uint8_t* tr = TraceAt(i);
            if(At<int8_t>(tr, OFF_TR_SPRITE) == 0 && (At<uint8_t>(tr, OFF_TR_FLAGS2) & 3) > 1)
            {
                CVector p = At<CVector>(tr, OFF_TR_POS);
                traces[count] = tr;
                distances[count] = At<uint32_t>(tr, OFF_TR_COLOUR) == BLIP_COLOUR_DESTINATION ? FLT_MAX :
                                   sqrtf((p.x - ppos.x) * (p.x - ppos.x) + (p.y - ppos.y) * (p.y - ppos.y) + (p.z - ppos.z) * (p.z - ppos.z));
                ++count;
            }
        }

        if(count > 0)
        {
            float maxdist = distances[0];
            for(unsigned short i = 1; i < count; ++i)
            {
                if(distances[i] > maxdist) maxi = i; // (same behaviour as the original code)
                if(distances[i] == FLT_MAX) break;
            }
            uint8_t* tr = traces[maxi];
            g_pMissionTrace = tr;
            RGBA col = TraceRGBA(At<uint32_t>(tr, OFF_TR_COLOUR), (At<uint8_t>(tr, OFF_TR_FLAGS1) >> 3) & 1);
            float* d = !g_targetBlip ? &gpsDistance : NULL;
            int handle = At<int>(tr, OFF_TR_POOLINDEX);
            void* ent = NULL;
            switch((At<uint8_t>(tr, OFF_TR_FLAGS2) >> 2) & 0xF)
            {
                case BLIP_CAR:    ent = GetPoolVeh(handle); if(ent) DoPathDraw(GetEntityPos(ent), col, false, d); break;
                case BLIP_CHAR:   ent = GetPoolPed(handle); if(ent) DoPathDraw(GetEntityPos(ent), col, false, d); break;
                case BLIP_OBJECT: ent = GetPoolObj(handle); if(ent) DoPathDraw(GetEntityPos(ent), col, false, d); break;
                case BLIP_PICKUP:
                {
                    int pi = handle & 0xFFFF; // (UNVERIFIED) index of the pickup
                    if(pi < MAX_PICKUPS)
                    {
                        uint8_t* p = aPickUps + SIZEOF_PICKUP * pi;
                        CVector pp = { At<int16_t>(p, OFF_PICKUP_X) / 8.0f, At<int16_t>(p, OFF_PICKUP_Y) / 8.0f, At<int16_t>(p, OFF_PICKUP_Z) / 8.0f };
                        DoPathDraw(pp, col, false, d);
                    }
                    break;
                }
                case BLIP_COORD:
                case BLIP_CONTACT_POINT:
                    DoPathDraw(At<CVector>(tr, OFF_TR_POS), col, false, d);
                    break;
                default: break;
            }
        }
    }

    DrawDistanceText();
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */
static int g_missing = 0;
static uintptr_t Sym(const char* name)
{
    uintptr_t a = aml->GetSym(hGTASA, name);
    if(!a) { ++g_missing; logger->Error("GPS: missing symbol %s", name); }
    return a;
}

extern "C" void OnModLoad()
{
    logger->SetTag("GPS AML");
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = dlopen("libGTASA.so", RTLD_LAZY);

    pCfgClosestMaxGPSDistance = cfg->Bind("ClosestMaxGPSDistance", 50.0f);
    if(pCfgClosestMaxGPSDistance->GetFloat() < 0) { pCfgClosestMaxGPSDistance->SetFloat(0.0f); cfg->Save(); }
    else if(pCfgClosestMaxGPSDistance->GetFloat() > 200) { pCfgClosestMaxGPSDistance->SetFloat(200.0f); cfg->Save(); }

    pCfgGPSLineColorRGB = cfg->Bind("GPSLineColorRGB", STRINGIFY(GPS_LINE_R) " " STRINGIFY(GPS_LINE_G) " " STRINGIFY(GPS_LINE_B) " " STRINGIFY(GPS_LINE_A));
    pCfgGPSLineWidth = cfg->Bind("GPSLineWidth", 4.0f); lineWidth = pCfgGPSLineWidth->GetFloat();
    pCfgGPSDrawDistance = cfg->Bind("GPSDrawDistance", true);
    pCfgGPSDrawDistancePosition = cfg->Bind("GPSDrawDistancePos", 0); // 0 under, 1 above, 2 left, 3 right, 4 custom
    pCfgGPSDrawDistanceTextScale = cfg->Bind("GPSDrawDistanceTextScale", 1.0f);
    pCfgGPSDrawDistanceTextOffset = cfg->Bind("GPSDrawDistanceTextOffset", "0.0 0.0");
    bAllowBMX = cfg->Bind("AllowBMX", false)->GetBool();
    bAllowBoat = cfg->Bind("AllowBoatNavi", true)->GetBool();
    bAllowMission = cfg->Bind("MissionRoutes", true)->GetBool();
    bImperialUnits = cfg->Bind("UseImperialUnits", false)->GetBool();

    int r, g, b, a, sscanfed = sscanf(pCfgGPSLineColorRGB->GetString(), "%d %d %d %d", &r, &g, &b, &a);
    if(sscanfed == 4 && IsRGBValue(r) && IsRGBValue(g) && IsRGBValue(b) && IsRGBValue(a))
        gpsLineColor = {(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a};
    else if(sscanfed == 3 && IsRGBValue(r) && IsRGBValue(g) && IsRGBValue(b))
        gpsLineColor = {(uint8_t)r, (uint8_t)g, (uint8_t)b, 255};
    else
    {
        pCfgGPSLineColorRGB->SetString(STRINGIFY(GPS_LINE_R) " " STRINGIFY(GPS_LINE_G) " " STRINGIFY(GPS_LINE_B) " " STRINGIFY(GPS_LINE_A));
        cfg->Save();
    }

    cfg->Bind("Author", "", "About")->SetString("[-=KILL MAN=-]");
    cfg->Bind("IdeasFrom", "", "About")->SetString("DK22Pac, JuniorDjjr, juicermv");
    cfg->Bind("Discord", "", "About")->SetString("https://discord.gg/2MY7W39kBg");
    cfg->Bind("GitHub", "", "About")->SetString("https://github.com/AndroidModLoader/GTASA_GPS");
    cfg->Save();

    SET_TO(ThePaths,        Sym("ThePaths"));
    SET_TO(gMobileMenu,     Sym("gMobileMenu"));
    SET_TO(RsGlobal,        Sym("RsGlobal"));
    SET_TO(NearScreenZ,     Sym("_ZN9CSprite2d11NearScreenZE"));
    SET_TO(RecipNearClip,   Sym("_ZN9CSprite2d13RecipNearClipE"));
    SET_TO(pRadarTrace,     Sym("_ZN6CRadar13ms_RadarTraceE"));
    SET_TO(m_UserPause,     Sym("_ZN6CTimer11m_UserPauseE"));
    SET_TO(m_CodePause,     Sym("_ZN6CTimer11m_CodePauseE"));
    SET_TO(aWidgets,        Sym("_ZN15CTouchInterface10m_pWidgetsE"));
    SET_TO(aPickUps,        Sym("_ZN8CPickups8aPickUpsE"));

    SET_TO(FindPlayerPed,                       Sym("_Z13FindPlayerPedi"));
    SET_TO(FindGroundZForCoord,                 Sym("_ZN6CWorld19FindGroundZForCoordEff"));
    SET_TO(DoPathSearch,                        Sym("_ZN9CPathFind12DoPathSearchEh7CVector12CNodeAddressS0_PS1_PsiPffS2_fbS1_bb"));
    SET_TO(TransformRealWorldPointToRadarSpace, Sym("_ZN6CRadar35TransformRealWorldPointToRadarSpaceER9CVector2DRKS0_"));
    SET_TO(TransformRadarPointToScreenSpace,    Sym("_ZN6CRadar32TransformRadarPointToScreenSpaceER9CVector2DRKS0_"));
    SET_TO(LimitRadarPoint,                     Sym("_ZN6CRadar15LimitRadarPointER9CVector2D"));
    SET_TO(RwRenderStateSet,                    Sym("_Z16RwRenderStateSet13RwRenderStatePv"));
    SET_TO(RwIm2DRenderPrimitive,               Sym("_Z28RwIm2DRenderPrimitive_BUGFIX15RwPrimitiveTypeP14RwOpenGLVertexi"));
    SET_TO(SetScissorRect,                      Sym("_ZN7CWidget10SetScissorER5CRect"));
    SET_TO(ClearRadarBlip,                      Sym("_ZN6CRadar9ClearBlipEi"));
    SET_TO(FontSetOrientation,                  Sym("_ZN5CFont14SetOrientationEh"));
    SET_TO(FontSetColor,                        Sym("_ZN5CFont8SetColorE5CRGBA"));
    SET_TO(FontSetBackground,                   Sym("_ZN5CFont13SetBackgroundEhh"));
    SET_TO(FontSetWrapx,                        Sym("_ZN5CFont8SetWrapxEf"));
    SET_TO(FontSetStyle,                        Sym("_ZN5CFont12SetFontStyleEh"));
    SET_TO(FontSetScale,                        Sym("_ZN5CFont8SetScaleEf"));
    SET_TO(FontSetProportional,                 Sym("_ZN5CFont15SetProportionalEh"));
    SET_TO(FontSetDropShadowPosition,           Sym("_ZN5CFont21SetDropShadowPositionEa"));
    SET_TO(FontPrintString,                     Sym("_ZN5CFont11PrintStringEffPt"));
    SET_TO(AsciiToGxtChar,                      Sym("_Z14AsciiToGxtCharPKcPt"));
    SET_TO(RenderFontBuffer,                    Sym("_ZN5CFont16RenderFontBufferEv"));
    SET_TO(IsOnAMission,                        Sym("_ZN11CTheScripts18IsPlayerOnAMissionEv"));
    SET_TO(GetPoolPed,                          Sym("_ZN6CPools6GetPedEi"));
    SET_TO(GetPoolVeh,                          Sym("_ZN6CPools10GetVehicleEi"));
    SET_TO(GetPoolObj,                          Sym("_ZN6CPools9GetObjectEi"));

    uintptr_t radarHud = Sym("_ZN4CHud9DrawRadarEv");
    if(g_missing)
    {
        logger->Error("GPS: %d symbol(s) missing, the mod stays disabled", g_missing);
        Dbg("%d symbol(s) missing, mod disabled", g_missing);
        return;
    }
    HOOK(DrawRadarHook, radarHud);
    Dbg("loaded: ThePaths=%p gMobileMenu=%p RsGlobal=%p trace=%p widgets=%p pickups=%p DoPathSearch=%p hook=%p",
        ThePaths, gMobileMenu, RsGlobal, pRadarTrace, aWidgets, aPickUps, (void*)DoPathSearch, (void*)radarHud);

    // TODO (phase 2): port the path-node limit / streaming-radius patches of DoPathSearch
    //                 and CPathFind::UpdateStreaming (Thumb-2 patches in the 32-bit version).
    logger->Info("GPS 1.2.1-arm64 loaded (milestone 1, no node-limit patches)");
}
