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
#include <vector>
#include <queue>
#include <algorithm>

#define MAX_NODE_POINTS 2000
#define MAX_RADAR_TRACES 250

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#define GPS_LINE_R      235
#define GPS_LINE_G      212
#define GPS_LINE_B      0
#define GPS_LINE_A      255

#define GPS_BUILD "r8"
MYMODCFG(net.dk22pac.rusjj.gps, GTA:SA GPS, 1.2.1-arm64-r8, DK22Pac & juicermv & RusJJ)
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
// CPathFind extra tables (72 entries each, only the first 64 are outdoor regions)
#define PATH_REGIONS            64
#define OFF_PATHS_ADJ           5256 // CNodeAddress* pAdjacentNodes[region]
#define OFF_PATHS_NUMNODES      8136 // int NumNodes[region]
#define OFF_PATHS_NUMCAR        8424 // int NumNodesCarNodes[region] (car nodes come first)
#define OFF_NODE_ADJIDX         24   // int16 index into pAdjacentNodes[region]
#define OFF_NODE_FLAGS          32   // low nibble = number of adjacent nodes, bit5 = switched off, bit7 = water node
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
#define rwPRIMTYPETRILIST 3
#define rwPRIMTYPETRISTRIP 4

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */
uintptr_t pGTASA;
void* hGTASA;

CNodeAddress resultNodes[MAX_NODE_POINTS];
CVector2D nodePoints[MAX_NODE_POINTS];
RwOpenGLVertex lineVerts[MAX_NODE_POINTS * 6];
CVector2D radarPts[MAX_NODE_POINTS];
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
bool g_mapMode = false;   // true while drawing on the pause map
float mapScale = 1.0f;

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
// Path-node streaming (members of CPathFind: first argument is the ThePaths object)
void (*PathMakeRequest)(uintptr_t, float, float, float, float);   // minX, maxX, minY, maxY
void (*PathUpdateStreaming)(uintptr_t, bool);
void (*PathReleaseRequested)(uintptr_t);
int  (*PathFindRegion)(uintptr_t, float, float);
void (*PathLoadScene)(uintptr_t, CVector);
void (*StreamLoadAll)(bool);   // CStreaming::LoadAllRequestedModels(bool): synchronous, works on the pause map
bool bLongRoutes = true, bPauseForceLoad = true;
unsigned g_routeLogs = 0;
void (*ClearRadarBlip)(int);
bool (*IsOnAMission)();
void* (*GetPoolPed)(int);
void* (*GetPoolVeh)(int);
void* (*GetPoolObj)(int);

void (*FontSetOrientation)(unsigned char);
void (*FontSetColor)(RGBA*);            // CRGBA has a destructor => the ARM64 ABI passes it by reference (crash proved it)
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
    mapScale = 0.00223214285f * h; // same factor the 32-bit mod used on the pause map
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
    static RGBA s_textColor;
    s_textColor = col;
    FontSetColor(&s_textColor);
    FontSetBackground(false, false);
    FontSetWrapx(500.0f);
    FontSetScale(textScale);
    FontSetStyle(FONT_SUBTITLES);
    FontSetProportional(true);
    FontSetDropShadowPosition(1);
    FontPrintString(gpsDistanceTextPos.x, gpsDistanceTextPos.y, textGxt);
    RenderFontBuffer();
}

/* ------------------------------------------------------------------ */
/* Own route search over the game's path-node data                    */
/* ------------------------------------------------------------------ */
static inline uint8_t*       RegionNodes(int r)  { return At<uint8_t*>(ThePaths, OFF_PATHS_NODES + 8 * r); }
static inline int            RegionCount(int r)  { return At<int>(ThePaths, OFF_PATHS_NUMNODES + 4 * r); }
static inline int            RegionCars(int r)   { return At<int>(ThePaths, OFF_PATHS_NUMCAR + 4 * r); }
static inline CNodeAddress*  RegionAdj(int r)    { return At<CNodeAddress*>(ThePaths, OFF_PATHS_ADJ + 8 * r); }
static inline uint8_t*       NodeAt(int r, int i){ return RegionNodes(r) + SIZEOF_PATHNODE * i; }
static inline CVector NodePos3(uint8_t* n)
{
    return { At<int16_t>(n, OFF_NODE_X) / 8.0f, At<int16_t>(n, OFF_NODE_Y) / 8.0f, At<int16_t>(n, 20) / 8.0f };
}
static inline bool NodeUsable(uint8_t* n, bool boat)
{
    uint8_t f = At<uint8_t>(n, OFF_NODE_FLAGS);
    if(f & (1 << 5)) return false;                 // switched off
    bool water = (f >> 7) & 1;
    return boat ? water : !water;
}

struct RouteCache
{
    int startId = -1, goalId = -1;
    uint64_t sig = 0;
    bool boat = false, ok = false, valid = false;
    int n = 0;
    float len = 0.0f;
    CVector2D pts[MAX_NODE_POINTS];
};
static RouteCache g_routeCache[2];

static std::vector<int>   s_base(PATH_REGIONS + 1);
static std::vector<float> s_g;
static std::vector<int>   s_parent;
static std::vector<uint8_t> s_closed;

static uint64_t LoadedSignature()
{
    uint64_t sig = 0;
    for(int r = 0; r < PATH_REGIONS; ++r) if(RegionNodes(r)) sig |= (1ULL << r);
    return sig;
}

// Finds the nearest usable car node among all loaded regions. Returns a global id or -1.
static int NearestNode(CVector p, bool boat, int* outR = NULL, int* outI = NULL)
{
    int best = -1; float bestD = 1e30f;
    for(int r = 0; r < PATH_REGIONS; ++r)
    {
        if(!RegionNodes(r)) continue;
        int cars = RegionCars(r), total = RegionCount(r);
        if(cars <= 0 || cars > total || total > 20000) continue;
        for(int i = 0; i < cars; ++i)
        {
            uint8_t* n = NodeAt(r, i);
            if(!NodeUsable(n, boat)) continue;
            CVector q = NodePos3(n);
            float dx = q.x - p.x, dy = q.y - p.y, dz = (q.z - p.z) * 2.0f;
            float d = dx * dx + dy * dy + dz * dz;
            if(d < bestD) { bestD = d; best = s_base[r] + i; if(outR) *outR = r; if(outI) *outI = i; }
        }
    }
    return (bestD < 4000.0f * 4000.0f) ? best : -1;
}

static void IdToNode(int id, int& r, int& i)
{
    r = 0;
    while(r + 1 < PATH_REGIONS && s_base[r + 1] <= id) ++r;
    i = id - s_base[r];
}

// A* over the loaded car nodes. Fills cache.pts (world X/Y) and cache.len.
static bool AStarRoute(CVector from, CVector to, bool boat, RouteCache& rc)
{
    int total = 0;
    for(int r = 0; r < PATH_REGIONS; ++r)
    {
        s_base[r] = total;
        if(RegionNodes(r)) { int c = RegionCars(r); if(c > 0 && c <= RegionCount(r) && c <= 20000) total += c; }
    }
    s_base[PATH_REGIONS] = total;
    if(total <= 0) return false;

    int sr = 0, si = 0, gr = 0, gi = 0;
    int startId = NearestNode(from, boat, &sr, &si);
    int goalId  = NearestNode(to, boat, &gr, &gi);
    if(startId < 0 || goalId < 0) return false;

    s_g.assign(total, 1e30f);
    s_parent.assign(total, -1);
    s_closed.assign(total, 0);

    CVector goalPos = NodePos3(NodeAt(gr, gi));
    typedef std::pair<float, int> QE;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE> > open;
    s_g[startId] = 0.0f;
    { CVector sp = NodePos3(NodeAt(sr, si)); float h = sqrtf((sp.x-goalPos.x)*(sp.x-goalPos.x) + (sp.y-goalPos.y)*(sp.y-goalPos.y)); open.push(QE(h, startId)); }

    bool found = false;
    while(!open.empty())
    {
        int cur = open.top().second; open.pop();
        if(s_closed[cur]) continue;
        s_closed[cur] = 1;
        if(cur == goalId) { found = true; break; }

        int r, i; IdToNode(cur, r, i);
        uint8_t* n = NodeAt(r, i);
        CVector cp = NodePos3(n);
        CNodeAddress* adj = RegionAdj(r);
        if(!adj) continue;
        int first = At<int16_t>(n, OFF_NODE_ADJIDX);
        int cnt = At<uint8_t>(n, OFF_NODE_FLAGS) & 0xF;
        if(first < 0) continue;
        for(int k = 0; k < cnt; ++k)
        {
            CNodeAddress a = adj[first + k];
            if(a.region >= PATH_REGIONS || !RegionNodes(a.region)) continue;
            if((int)a.index >= RegionCars(a.region) || (int)a.index >= RegionCount(a.region)) continue;
            uint8_t* nb = NodeAt(a.region, a.index);
            if(!NodeUsable(nb, boat)) continue;
            int nid = s_base[a.region] + a.index;
            if(s_closed[nid]) continue;
            CVector np = NodePos3(nb);
            float dx = np.x - cp.x, dy = np.y - cp.y, dz = np.z - cp.z;
            float ng = s_g[cur] + sqrtf(dx * dx + dy * dy + dz * dz);
            if(ng < s_g[nid])
            {
                s_g[nid] = ng; s_parent[nid] = cur;
                float hx = np.x - goalPos.x, hy = np.y - goalPos.y;
                open.push(QE(ng + sqrtf(hx * hx + hy * hy), nid));
            }
        }
    }
    if(!found) return false;

    // rebuild the path (goal -> start) and reverse it
    static std::vector<int> rev; rev.clear();
    for(int c = goalId; c != -1 && (int)rev.size() < 100000; c = s_parent[c]) rev.push_back(c);
    int n = (int)rev.size();
    if(n < 2) return false;
    int step = n > MAX_NODE_POINTS ? (n + MAX_NODE_POINTS - 1) / MAX_NODE_POINTS : 1; // thin out absurdly long routes
    int out = 0;
    for(int k = n - 1; k >= 0 && out < MAX_NODE_POINTS; k -= step)
    {
        int r, i; IdToNode(rev[k], r, i);
        CVector q = NodePos3(NodeAt(r, i));
        rc.pts[out].x = q.x; rc.pts[out].y = q.y; ++out;
    }
    rc.n = out;
    rc.len = s_g[goalId];
    rc.startId = startId; rc.goalId = goalId;
    return true;
}

/* ------------------------------------------------------------------ */
/* Asking the game to keep the needed path-node regions loaded        */
/* ------------------------------------------------------------------ */
static inline int RegionIdx(float v)
{
    int i = (int)floorf((v + 3000.0f) / 750.0f);
    return i < 0 ? 0 : (i > 7 ? 7 : i);
}

// Number of outdoor regions overlapping the rectangle that are not in memory; also returns the centre of the first one.
static int CountMissingRegions(float minX, float maxX, float minY, float maxY, float* cx, float* cy)
{
    int missing = 0;
    for(int iy = RegionIdx(minY); iy <= RegionIdx(maxY); ++iy)
        for(int ix = RegionIdx(minX); ix <= RegionIdx(maxX); ++ix)
            if(!RegionNodes(iy * 8 + ix))
            {
                if(missing == 0) { *cx = -3000.0f + 750.0f * (ix + 0.5f); *cy = -3000.0f + 750.0f * (iy + 0.5f); }
                ++missing;
            }
    return missing;
}

static void RequestNodesFor(CVector a, CVector b)
{
    (void)a; (void)b;
    if(!PathMakeRequest) return;
    static unsigned s_tick = 0, s_lastReq = 0, s_lastForce = 0, s_noProgress = 0, s_forceLogs = 0, s_idleUntil = 0;
    static bool s_reqActive = false;
    ++s_tick;
    // Always ask for the whole map: a long route can leave the box between player and target
    // (detours), and the node data of the whole world is only a few megabytes.
    const float minX = -3000.0f, maxX = 3000.0f, minY = -3000.0f, maxY = 3000.0f;
    if(!s_reqActive || s_tick - s_lastReq > 240)
    {
        PathMakeRequest((uintptr_t)ThePaths, minX, maxX, minY, maxY);
        s_lastReq = s_tick; s_reqActive = true;
        if(g_routeLogs < 40 || s_forceLogs < 5)
            Dbg("STREAM request whole map, mapMode=%d loaded=0x%llx | stored: active=%d x=[%.0f..%.0f] y=[%.0f..%.0f]",
                (int)g_mapMode, (unsigned long long)LoadedSignature(),
                (int)At<bool>(ThePaths, 19600), At<float>(ThePaths, 19604), At<float>(ThePaths, 19608), At<float>(ThePaths, 19612), At<float>(ThePaths, 19616));
    }
    if(!g_mapMode && (s_tick % 300) == 1)
        Dbg("STREAM in-game: loaded=0x%llx (%d regions)", (unsigned long long)LoadedSignature(), __builtin_popcountll(LoadedSignature()));

    // On the pause map the game loop does not run its own streaming, so load the missing regions by hand:
    // UpdateStreaming only queues the requests, LoadAllRequestedModels runs the queue synchronously.
    if(g_mapMode && bPauseForceLoad && StreamLoadAll && PathUpdateStreaming && s_tick >= s_idleUntil && s_tick - s_lastForce > 3)
    {
        float cx = 0, cy = 0;
        int missing = CountMissingRegions(minX, maxX, minY, maxY, &cx, &cy);
        if(missing > 0)
        {
            s_lastForce = s_tick;
            uint64_t before = LoadedSignature();
            PathUpdateStreaming((uintptr_t)ThePaths, false);
            StreamLoadAll(false);
            uint64_t after = LoadedSignature();
            if(after == before)
            {
                if(++s_noProgress >= 3) { s_idleUntil = s_tick + 1500; s_noProgress = 0; } // the rest has no node data (sea), stop pushing
            }
            else s_noProgress = 0;
            if(s_forceLogs < 80)
            {
                ++s_forceLogs;
                Dbg("STREAM force: missing=%d, loaded 0x%llx -> 0x%llx (%d regions)", missing, (unsigned long long)before, (unsigned long long)after, __builtin_popcountll(after));
            }
        }
    }
}

static void DoPathDraw(CVector to, RGBA color, bool isTargetBlip = false, float* dist = NULL)
{
    void* player = FindPlayerPed(-1);
    void* veh = GetSupportedVehicle(player);
    if(!veh) { if(g_logNow) Dbg("DoPathDraw: no supported vehicle (player=%p)", player); return; }

    CVector from = GetEntityPos(player);
    bool isBoat = VehIs(veh, VEH_BOAT);
    bool boatNavi = isBoat && bAllowBoat;
    RouteCache& rc = g_routeCache[isTargetBlip ? 0 : 1];
    float trashVar;

    if(bLongRoutes) RequestNodesFor(from, to);

    // 1) our own search (no node limits, works over every loaded region)
    bool haveRoute = false;
    if(bLongRoutes)
    {
        uint64_t sig = LoadedSignature();
        int sId = -2, gId = -2;
        {
            // cheap check whether the cached result can still be used: same loaded regions, start and goal still map to the same nodes
            int total = 0;
            for(int r = 0; r < PATH_REGIONS; ++r) { s_base[r] = total; if(RegionNodes(r)) { int c = RegionCars(r); if(c > 0 && c <= RegionCount(r) && c <= 20000) total += c; } }
            s_base[PATH_REGIONS] = total;
            sId = NearestNode(from, boatNavi);
            gId = NearestNode(to, boatNavi);
        }
        if(rc.valid && rc.sig == sig && rc.startId == sId && rc.goalId == gId && rc.boat == boatNavi)
        {
            haveRoute = rc.ok;
        }
        else
        {
            unsigned t0 = 0; (void)t0;
            bool ok = AStarRoute(from, to, boatNavi, rc);
            rc.valid = true; rc.sig = sig; rc.boat = boatNavi; rc.ok = ok;
            if(!ok) { rc.startId = sId; rc.goalId = gId; }
            haveRoute = ok;

            if(g_routeLogs < 40)
            {
                ++g_routeLogs;
                // compare with the game's own search, for the log only
                short gn = 0; float gd = 0;
                CNodeAddress inv = {0xFFFF, 0xFFFF};
                DoPathSearch((uintptr_t)ThePaths, false, from, inv, to, resultNodes, &gn, MAX_NODE_POINTS, &gd, 1000000.0f, NULL, 1000000.0f, false, inv, false, boatNavi);
                float straight = sqrtf((from.x - to.x) * (from.x - to.x) + (from.y - to.y) * (from.y - to.y));
                Dbg("ROUTE#%u mapMode=%d straight=%.0f | own: ok=%d nodes=%d len=%.0f | game: nodes=%d dist=%.0f | loadedRegions=%d",
                    g_routeLogs, (int)g_mapMode, straight, (int)ok, ok ? rc.n : 0, ok ? rc.len : 0.0f, (int)gn, gd, __builtin_popcountll(sig));
            }
        }
    }

    // 2) fall back to the game's own search
    short nodesCount = 0;
    CVector2D* pts = rc.pts;
    int good = 0;
    if(haveRoute)
    {
        nodesCount = (short)rc.n;
        good = rc.n;
        if(dist) *dist = rc.len;
        gpsDistance = dist ? *dist : gpsDistance;
        // connect the ends: player -> first node and last node -> target (drawing only, the cache stays untouched)
        static CVector2D s_draw[MAX_NODE_POINTS + 2];
        int m = 0;
        float d0 = sqrtf((rc.pts[0].x - from.x) * (rc.pts[0].x - from.x) + (rc.pts[0].y - from.y) * (rc.pts[0].y - from.y));
        float d1 = sqrtf((rc.pts[rc.n - 1].x - to.x) * (rc.pts[rc.n - 1].x - to.x) + (rc.pts[rc.n - 1].y - to.y) * (rc.pts[rc.n - 1].y - to.y));
        if(d0 > 15.0f && d0 < 1500.0f) { s_draw[m].x = from.x; s_draw[m].y = from.y; ++m; }
        int cnt = rc.n < MAX_NODE_POINTS ? rc.n : MAX_NODE_POINTS;
        for(int i = 0; i < cnt; ++i) s_draw[m++] = rc.pts[i];
        if(d1 > 15.0f && d1 < 1500.0f) { s_draw[m].x = to.x; s_draw[m].y = to.y; ++m; }
        pts = s_draw;
        good = m;
    }
    else
    {
        CNodeAddress invalid = {0xFFFF, 0xFFFF};
        DoPathSearch((uintptr_t)ThePaths, false, from, invalid, to, resultNodes, &nodesCount,
                     MAX_NODE_POINTS, dist ? dist : &trashVar, 1000000.0f, NULL, 1000000.0f, false,
                     invalid, false, boatNavi);
        for(short i = 0; i < nodesCount; ++i)
        {
            uint8_t* table = At<uint8_t*>(ThePaths, OFF_PATHS_NODES + 8 * resultNodes[i].region);
            if(!table) continue;
            uint8_t* node = table + SIZEOF_PATHNODE * resultNodes[i].index;
            pts[good].x = At<int16_t>(node, OFF_NODE_X) / 8.0f;
            pts[good].y = At<int16_t>(node, OFF_NODE_Y) / 8.0f;
            ++good;
        }
    }

    static unsigned s_calls = 0;
    static int s_lastNodes = -2;
    ++s_calls;
    if(s_calls <= 10 || (s_calls % 240) == 0 || nodesCount != s_lastNodes)
        Dbg("DoPathDraw#%u: %s nodes=%d dist=%.1f player=(%.1f %.1f) target=(%.1f %.1f)", s_calls, haveRoute ? "own" : "game", (int)nodesCount,
            dist ? *dist : trashVar, from.x, from.y, to.x, to.y);
    s_lastNodes = nodesCount;
    if(nodesCount <= 0 || good <= 0) return;

    if(isTargetBlip && !g_mapMode && gpsDistance < pCfgClosestMaxGPSDistance->GetFloat())
    {
        ClearRadarBlip(g_targetBlip);
        At<int>(gMobileMenu, OFF_MENU_WAYPOINT_BLIP) = 0;
        g_targetBlip = 0;
        if(PathReleaseRequested && bLongRoutes) PathReleaseRequested((uintptr_t)ThePaths);
        return;
    }

    for(int i = 0; i < good; ++i)
    {
        CVector2D nodePos = pts[i];
        if(g_mapMode)
        {
            CVector2D rp;
            TransformRealWorldPointToRadarSpace(rp, nodePos);
            LimitRadarPoint(rp);
            TransformRadarPointToScreenSpace(radarPts[i], rp);
            radarPts[i].x *= mapScale;
            radarPts[i].y *= mapScale;
        }
        else TransformRealWorldPointToRadarSpace(radarPts[i], nodePos);
    }
    if(g_logNow) Dbg("DoPathDraw: good nodes=%d", (int)good);
    if(g_mapMode)
    {
        static unsigned s_mapCalls = 0;
        if(good > 0 && (s_mapCalls < 12 || (s_mapCalls % 90) == 0))
            Dbg("MAP#%u nodes=%d first=(%.1f %.1f) last=(%.1f %.1f) | scale=%.3f off=(%.1f %.1f) area=(%.1f %.1f) disp=%d mapMode=%d mapScale=%.3f",
                s_mapCalls, (int)good, radarPts[0].x, radarPts[0].y, radarPts[good - 1].x, radarPts[good - 1].y,
                At<float>(gMobileMenu, 120), At<float>(gMobileMenu, 124), At<float>(gMobileMenu, 128),
                At<float>(gMobileMenu, 132), At<float>(gMobileMenu, 136),
                (int)At<bool>(gMobileMenu, 140), (int)At<bool>(gMobileMenu, 141), mapScale);
        ++s_mapCalls;
    }
    if(good < 2) return;

    if(!g_bTextInited) { g_bTextInited = true; SetDistanceTextValues(); }
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, NULL);

    // The scissor rect does not clip in 2.10, so every segment is clipped to the radar disc here
    // (radar space: the disc has radius 1).
    const float R = 0.97f;
    unsigned int vi = 0;
    for(short i = 0; i < good - 1; i++)
    {
        CVector2D P0 = radarPts[i], P1 = radarPts[i + 1];
        CVector2D S0, S1;
        if(g_mapMode)
        {
            S0 = P0; S1 = P1;
            if(fabsf(S1.x - S0.x) + fabsf(S1.y - S0.y) < 1e-4f) continue;
        }
        else
        {
            CVector2D d = { P1.x - P0.x, P1.y - P0.y };
            float qa = d.x * d.x + d.y * d.y;
            if(qa < 1e-9f) continue;
            float qb = P0.x * d.x + P0.y * d.y;
            float qc = P0.x * P0.x + P0.y * P0.y - R * R;
            float disc = qb * qb - qa * qc;
            if(disc <= 0.0f) continue;
            float sq = sqrtf(disc);
            float t0 = (-qb - sq) / qa, t1 = (-qb + sq) / qa;
            if(t0 < 0.0f) t0 = 0.0f;
            if(t1 > 1.0f) t1 = 1.0f;
            if(t0 >= t1) continue;
            CVector2D Q0 = { P0.x + d.x * t0, P0.y + d.y * t0 }, Q1 = { P0.x + d.x * t1, P0.y + d.y * t1 };
            TransformRadarPointToScreenSpace(S0, Q0);
            TransformRadarPointToScreenSpace(S1, Q1);
        }
        float angle = atan2f(S1.y - S0.y, S1.x - S0.x);
        CVector2D o0 = { cosf(angle - 1.5707963f) * lineWidth, sinf(angle - 1.5707963f) * lineWidth };
        CVector2D o1 = { cosf(angle + 1.5707963f) * lineWidth, sinf(angle + 1.5707963f) * lineWidth };
        RwOpenGLVertex* v = &lineVerts[vi];
        Setup2DVertex(v[0], S0.x + o0.x, S0.y + o0.y, color);
        Setup2DVertex(v[1], S1.x + o0.x, S1.y + o0.y, color);
        Setup2DVertex(v[2], S0.x + o1.x, S0.y + o1.y, color);
        v[3] = v[1];
        Setup2DVertex(v[4], S1.x + o1.x, S1.y + o1.y, color);
        v[5] = v[2];
        vi += 6;
    }
    for(unsigned int off = 0; off < vi; off += 600)
    {
        unsigned int n = vi - off < 600 ? vi - off : 600;
        RwIm2DRenderPrimitive(rwPRIMTYPETRILIST, &lineVerts[off], (int)n);
    }
    if(g_logNow) Dbg("DoPathDraw: drew %u vertices", vi);
}

static bool g_lineDone = false;

static bool EnsureInit()
{
    if(g_bInited) return true;
    if(At<int>(RsGlobal, OFF_RS_WIDTH) <= 0) return false;
    g_bInited = true;
    InitializeConfigValues();
    return true;
}

static void DoGpsFrame(bool map)
{
    if(!map && IsGamePaused()) return;
    g_mapMode = map;
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
    g_mapMode = false;
}

// Drawn from inside the radar drawing (after the map), like the original mod did.
DECL_HOOKv(DrawRadarGangHook, bool b)
{
    DrawRadarGangHook(b);
    if(!EnsureInit()) return;
    if(b) { DoGpsFrame(true); return; } // pause map
    if(g_lineDone) return;
    g_lineDone = true;
    DoGpsFrame(false);
}

// Diagnostics only: where the game itself draws the player arrow on the pause map.
DECL_HOOKv(YouAreHereHook, float x, float y)
{
    YouAreHereHook(x, y);
    static unsigned n = 0;
    static float lx = -1e9f, ly = -1e9f;
    if(n < 60 && (fabsf(x - lx) > 1.0f || fabsf(y - ly) > 1.0f))
    {
        ++n; lx = x; ly = y;
        CVector pp = GetEntityPos(FindPlayerPed(-1));
        CVector2D w = {pp.x, pp.y}, rp, lim, sp = {0, 0};
        TransformRealWorldPointToRadarSpace(rp, w);
        lim = rp; LimitRadarPoint(lim);
        TransformRadarPointToScreenSpace(sp, lim);
        Dbg("ARROW args=(%.1f %.1f) | pipeline radar=(%.3f %.3f) screen=(%.1f %.1f) scaled=(%.1f %.1f) | menu scale=%.3f off=(%.1f %.1f) area=(%.1f %.1f) disp=%d mapMode=%d",
            x, y, rp.x, rp.y, sp.x, sp.y, sp.x * mapScale, sp.y * mapScale,
            At<float>(gMobileMenu, 120), At<float>(gMobileMenu, 124), At<float>(gMobileMenu, 128),
            At<float>(gMobileMenu, 132), At<float>(gMobileMenu, 136), (int)At<bool>(gMobileMenu, 140), (int)At<bool>(gMobileMenu, 141));
    }
}

DECL_HOOKv(DrawRadarHook)
{
    g_lineDone = false;
    gpsDistance = 0;
    g_pMissionTrace = NULL;
    DrawRadarHook();
    ++g_frame;
    g_logNow = (g_frame % 180) == 1;
    if(g_frame == 1) Dbg("hook called for the first time");

    if(!EnsureInit()) return;
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
    {
        static int s_lastMenuBlip = 0;
        int mb2 = At<int>(gMobileMenu, OFF_MENU_WAYPOINT_BLIP);
        if(mb2 != s_lastMenuBlip)
        {
            s_lastMenuBlip = mb2;
            Dbg("f=%u menuBlip changed -> %d (idx %d, ref %d) paused=%d", g_frame, mb2, mb2 & 0xFFFF, (int)((uint32_t)mb2 >> 16), (int)IsGamePaused());
        }
    }
    if(g_frame == 1 || g_logNow) Dbg("  line drawn from the gang-overlay hook this frame: %d", (int)g_lineDone);
    if(IsGamePaused()) return; // pause-map drawing is not ported yet (radar-visible check dropped: widget slot is NULL in 2.10)

    if(!g_lineDone) { g_lineDone = true; DoGpsFrame(false); } // fallback if the gang-overlay hook did not run
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
    Dbg("=== GPS64 build %s (%s %s) - long routes: own A* + path-node streaming ===", GPS_BUILD, __DATE__, __TIME__);
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
    bLongRoutes = cfg->Bind("LongRoutes", true)->GetBool();            // own route search + path-node streaming
    bPauseForceLoad = cfg->Bind("PauseMapForceLoad", true)->GetBool(); // push node streaming by hand on the pause map
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

    // optional: path-node streaming (long routes)
    PathMakeRequest      = (void(*)(uintptr_t, float, float, float, float))aml->GetSym(hGTASA, "_ZN9CPathFind29MakeRequestForNodesToBeLoadedEffff");
    PathUpdateStreaming  = (void(*)(uintptr_t, bool))aml->GetSym(hGTASA, "_ZN9CPathFind15UpdateStreamingEb");
    PathReleaseRequested = (void(*)(uintptr_t))aml->GetSym(hGTASA, "_ZN9CPathFind21ReleaseRequestedNodesEv");
    PathFindRegion       = (int(*)(uintptr_t, float, float))aml->GetSym(hGTASA, "_ZN9CPathFind19FindRegionForCoorsEff");
    PathLoadScene        = (void(*)(uintptr_t, CVector))aml->GetSym(hGTASA, "_ZN9CPathFind21LoadSceneForPathNodesE7CVector");
    StreamLoadAll = NULL;
    if(pGTASA)
    {
        const uint32_t expect[4] = {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
        const uint32_t* code = (const uint32_t*)(pGTASA + 0x396b28);
        bool same = true;
        for(int i = 0; i < 4; ++i) if(code[i] != expect[i]) same = false;
        if(same) StreamLoadAll = (void(*)(bool))(pGTASA + 0x396b28);
        Dbg("LoadAllRequestedModels at %p: %s", (void*)(pGTASA + 0x396b28), same ? "code matches the 2.10 arm64 build" : "CODE DOES NOT MATCH, disabled");
    }
    Dbg("streaming api: make=%p update=%p release=%p loadScene=%p", (void*)PathMakeRequest, (void*)PathUpdateStreaming, (void*)PathReleaseRequested, (void*)PathLoadScene);
    uintptr_t radarHud = Sym("_ZN4CHud9DrawRadarEv");
    uintptr_t gangOverlay = Sym("_ZN6CRadar20DrawRadarGangOverlayEb");
    if(g_missing)
    {
        logger->Error("GPS: %d symbol(s) missing, the mod stays disabled", g_missing);
        Dbg("%d symbol(s) missing, mod disabled", g_missing);
        return;
    }
    HOOK(DrawRadarHook, radarHud);
    HOOK(DrawRadarGangHook, gangOverlay);
    // Diagnostics only. The symbol is not in the dynamic table, so use its address from the 2.10 arm64 build.
    uintptr_t arrowFn = aml->GetSym(hGTASA, "_ZN6CRadar21DrawYouAreHereSpriteEff");
    if(!arrowFn && pGTASA) arrowFn = pGTASA + 0x5262A0;
    if(arrowFn) HOOK(YouAreHereHook, arrowFn);
    Dbg("loaded: ThePaths=%p gMobileMenu=%p RsGlobal=%p trace=%p widgets=%p pickups=%p DoPathSearch=%p hook=%p",
        ThePaths, gMobileMenu, RsGlobal, pRadarTrace, aWidgets, aPickUps, (void*)DoPathSearch, (void*)radarHud);

    // TODO (phase 2): port the path-node limit / streaming-radius patches of DoPathSearch
    //                 and CPathFind::UpdateStreaming (Thumb-2 patches in the 32-bit version).
    logger->Info("GPS 1.2.1-arm64 build %s loaded", GPS_BUILD);
}
