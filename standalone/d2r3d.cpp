// d2r-3d.dll - perspective 3D camera for Diablo II: Resurrected.
//
// Load via d2r-3d.exe, press F12 to enable it.
// Mouse wheel = zoom, middle mouse drag = yaw/pitch.
//
// Only works for D2R 3.3.93847 (D2R.exe TimeDateStamp 0x6A6B96A4).
//
// Camera struct S (the `this` of the game's camera functions):
//   +0x000 view   +0x040 inverse view   +0x080 proj   +0x100 position   +0x10C look-at
//   +0x118 valid  +0x138/13C view box   +0x140/144 viewport px   +0x148 near
//   +0x158 projection type (1 = ortho)  +0x160 proj dirty   +0x161 view dirty
//
// Hooks:
//   GetProj     0x8CE660  after the game builds its ortho proj, write our perspective one
//   ViewRebuild 0x8CE830  after the game builds its view, fold in orbit + distance
//   RayBuild    0x8CEAC0  mouse ray from our matrices (targeting); calls from the shadow
//                          fit 0x952A70..0x95397E get their downward slope clamped
//   ActTest     0x5203A0  room activation test: every candidate room activates
//   SightAdd    0x2365D0  client "room in sight": trigger for building extra room rings
// Game functions called: RoomInit 0x2C2DF0, ReleaseRoomData 0x2F51A0, RoomLink 0x2C3C60,
//   TLSF block_insert 0xBB2960. Data: model visibility radius^2 0x18131B8 (.rdata),
//   candidate walk depth mgr+0xF6C, renderer pool object 0x1CFBD38, entity pool
//   object 0x2053810 (same wrapper class).
//
// Log: d2r-3d.log next to the DLL.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"

#pragma intrinsic(_ReturnAddress)

namespace {

// Settings
constexpr float kFov = 60.0f;           // perspective vertical FOV (deg)
constexpr float kDist = 45.0f;          // default camera-to-pivot distance (world units); the wheel changes it
constexpr float kPitch = 22.0f;         // default pitch (deg)
constexpr float kHeight = 6.0f;         // pivot lift above the ground look-at point (world up)
constexpr float kMvRadius = 3000.0f;    // model visibility radius (vanilla 150)
constexpr float kShadowSlope = 0.5f;    // min downward slope of shadow-fit rays
constexpr int   kBuildRings = 12;       // extra rings of rooms built around the player
constexpr int   kBuildRate = 16;        // max rooms built per pass
constexpr int   kRenderPoolMB = 64;     // renderer pool size (vanilla 12)
constexpr int   kEntityPoolMB = 280;    // entity (EnTT) pool size (vanilla 70)
constexpr bool  kWheelSwallow = true;   // don't pass the wheel to the game while on

// Logging buffer
HMODULE g_self = nullptr;
wchar_t g_logPath[MAX_PATH];
constexpr int kRing = 512, kLine = 256;
struct { SRWLOCK lock = SRWLOCK_INIT; char lines[kRing][kLine]; uint32_t head = 0, tail = 0; } g_ring;

void Log(const char* fmt, ...) {
    char line[kLine]; SYSTEMTIME st; GetLocalTime(&st);
    int n = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vsnprintf(line + n, sizeof line - n, fmt, ap); va_end(ap);
    AcquireSRWLockExclusive(&g_ring.lock);
    if (g_ring.head - g_ring.tail < (uint32_t)kRing) memcpy(g_ring.lines[g_ring.head++ % kRing], line, kLine);
    ReleaseSRWLockExclusive(&g_ring.lock);
}

void FlushLog() {
    static char out[kRing * (kLine + 2)];
    size_t len = 0;
    AcquireSRWLockExclusive(&g_ring.lock);
    while (g_ring.tail != g_ring.head) {
        const char* s = g_ring.lines[g_ring.tail++ % kRing];
        const size_t l = strnlen(s, kLine - 1);
        memcpy(out + len, s, l); len += l; out[len++] = '\r'; out[len++] = '\n';
    }
    ReleaseSRWLockExclusive(&g_ring.lock);
    if (!len) return;
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, out, (DWORD)len, &wr, nullptr); CloseHandle(f); }
}

#pragma optimize("", off)
bool SafeRead(void* d, const void* s, size_t n) noexcept { __try { memcpy(d, s, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
#pragma optimize("", on)

// Game memory
uintptr_t g_base = 0;
constexpr uint64_t RVA_CAMCOPY = 0x5F0710, RVA_COPY_CALL = 0x5F16D7;   // camera operator=
constexpr uint64_t RVA_GETPROJ = 0x8CE660, RVA_VIEWREBUILD = 0x8CE830, RVA_RAYBUILD = 0x8CEAC0,
                   RVA_SHADOWFIT = 0x952A70, RVA_SHADOWFIT_END = 0x95397F, RVA_ACTTEST = 0x5203A0,
                   RVA_SIGHTADD = 0x2365D0, RVA_ROOMINIT = 0x2C2DF0, RVA_ROOMRELEASE = 0x2F51A0,
                   RVA_ROOMLINK = 0x2C3C60, RVA_ACTDEPTH_CMP = 0x522662, RVA_MVRADIUS_SQ = 0x18131B8,
                   RVA_MVRADIUS_LD = 0x523E34, RVA_RPOOL_OBJ = 0x1CFBD38, RVA_RPOOL_VTBL = 0x172D358,
                   RVA_TLSF_INSERT = 0xBB2960, RVA_IAT_LOCK = 0x15CDA08, RVA_IAT_UNLOCK = 0x15CDA10,
                   RVA_EPOOL_OBJ = 0x2053810;

struct Sig { const char* name; uint64_t rva; uint8_t bytes[16]; size_t n; };
const Sig kSigs[] = {
    {"CamCopy",     RVA_CAMCOPY,      {0x48,0x83,0xEC,0x28,0x48,0x8B,0x05,0x5D,0x9A,0x38,0x01,0x48,0x33,0xC4,0x48,0x89}, 16},
    {"CopyCall",    RVA_COPY_CALL,    {0xE8,0x34,0xF0,0xFF,0xFF}, 5},
    {"GetProj",     RVA_GETPROJ,      {0x40,0x53,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x80,0xB9,0x60,0x01,0x00,0x00,0x00}, 16},
    {"ViewRebuild", RVA_VIEWREBUILD,  {0x48,0x8B,0xC4,0x48,0x81,0xEC,0xE8,0x00,0x00,0x00,0x0F,0x29,0x78,0xE8,0x44,0x0F}, 16},
    {"RayBuild",    RVA_RAYBUILD,     {0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x55,0x56,0x57,0x48,0x8D,0x68,0xA1,0x48,0x81}, 16},
    {"ShadowFit",   RVA_SHADOWFIT,    {0x48,0x8B,0xC4,0x55,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0x28}, 16},
    {"ActTest",     RVA_ACTTEST,      {0x48,0x8B,0xC4,0x48,0x89,0x58,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56}, 16},
    {"SightAdd",    RVA_SIGHTADD,     {0x40,0x53,0x55,0x41,0x56,0x48,0x83,0xEC,0x30,0x45,0x8B,0xF1,0x41,0x8B,0xD8,0x0F}, 16},
    {"RoomInit",    RVA_ROOMINIT,     {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x8B,0x42,0x50,0x48,0x8B,0xDA}, 16},
    {"RoomRelease", RVA_ROOMRELEASE,  {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57}, 16},
    {"RoomLink",    RVA_ROOMLINK,     {0x40,0x53,0x55,0x48,0x83,0xEC,0x28,0x48,0x8B,0x82,0x90,0x00,0x00,0x00,0x33,0xED}, 16},
    {"ActDepthCmp", RVA_ACTDEPTH_CMP, {0x41,0x39,0x9E,0x6C,0x0F,0x00,0x00}, 7},
    {"MvRadiusLd",  RVA_MVRADIUS_LD,  {0xF3,0x0F,0x10,0x05,0x7C,0xF3,0x2E,0x01}, 8},
    {"TlsfInsert",  RVA_TLSF_INSERT,  {0x4C,0x8B,0x42,0x08,0x4C,0x8B,0xCA,0x49,0x83,0xE0,0xFC,0x4C,0x8B,0xD9,0x49,0x81}, 16},
};

bool Verify(const char* name) {
    for (const auto& s : kSigs) {
        if (strcmp(s.name, name)) continue;
        uint8_t live[16] = {};
        const bool ok = SafeRead(live, (void*)(g_base + s.rva), s.n) && !memcmp(live, s.bytes, s.n);
        if (!ok) Log("  %s at rva 0x%llX does not match this build - skipped", name, (unsigned long long)s.rva);
        return ok;
    }
    return false;
}

template <class T> inline T& F(uintptr_t a, size_t off) { return *(T*)(a + off); }

constexpr size_t S_VIEW = 0x00, S_INVVIEW = 0x40, S_PROJ = 0x80, S_LOOKAT = 0x10C, S_LOOKAT_OK = 0x118,
                 S_EXTW = 0x138, S_EXTH = 0x13C, S_VPW = 0x140, S_VPH = 0x144, S_NEAR = 0x148,
                 S_TYPE = 0x158, S_PDIRTY = 0x160, S_VDIRTY = 0x161;

// Matrix math helpers (row-major)
void Mul4(const float* A, const float* B, float* C) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        C[r*4+c] = A[r*4]*B[c] + A[r*4+1]*B[4+c] + A[r*4+2]*B[8+c] + A[r*4+3]*B[12+c];
}
void Vec4Mat(const float* v, const float* M, float* o) {
    for (int j = 0; j < 4; ++j) o[j] = v[0]*M[j] + v[1]*M[4+j] + v[2]*M[8+j] + v[3]*M[12+j];
}
bool Invert4(const float* m, float* out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { a[i][j] = m[i*4+j]; a[i][j+4] = i == j; }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r) if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-12) return false;
        for (int j = 0; j < 8; ++j) std::swap(a[col][j], a[piv][j]);
        const double d = a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != col) { const double f = a[r][col]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i*4+j] = (float)a[i][j+4];
    return true;
}
void AxisRot4(float* M, float ux, float uy, float uz, float t) {   // rotation about unit axis u
    const float c = cosf(t), s = sinf(t), k = 1.0f - c;
    const float R[16] = {c + ux*ux*k, uy*ux*k + uz*s, uz*ux*k - uy*s, 0,
                         ux*uy*k - uz*s, c + uy*uy*k, uz*uy*k + ux*s, 0,
                         ux*uz*k + uy*s, uy*uz*k - ux*s, c + uz*uz*k, 0,
                         0, 0, 0, 1};
    memcpy(M, R, sizeof R);
}

// State
std::atomic<bool> g_hooked{false}, g_enabled{false};
std::atomic<float> g_dist{kDist}, g_pitch{kPitch}, g_yaw{0.0f};
std::atomic<uint32_t> g_gen{1};
std::atomic<uintptr_t> g_roomMgr{0};
std::atomic<uintptr_t> g_master{0};
bool g_shadowOk = false;

struct Cam { uintptr_t ptr; float proj0[16]; float extW, extH, nearZ; int type; bool haveBase, written; uint32_t lastGen; float lookSign; };
SRWLOCK g_camLock = SRWLOCK_INIT;
Cam g_cams[8];
int g_camCount = 0;

Cam* FindCam(uintptr_t s) { for (int i = 0; i < g_camCount; ++i) if (g_cams[i].ptr == s) return &g_cams[i]; return nullptr; }

// Camera-to-pivot distance.
float Radius(const Cam&) { return g_dist.load(); }

// Infinite reverse-Z perspective, same form as the game's own perspective branch
// (proj[10] = 0, proj[11] = -1, proj[14] = near), keeping the ortho view's screen offset.
void BuildProj(const Cam& c, float* M) {
    const float aspect = c.extH > 0 && c.extW / c.extH > 0.1f && c.extW / c.extH < 10.0f ? c.extW / c.extH : 16.0f / 9.0f;
    const float f = 1.0f / tanf(kFov * 3.14159265f / 360.0f);
    const float radius = Radius(c);
    const float nearZ = c.nearZ > 0.0f && c.nearZ < radius * 0.5f ? c.nearZ : radius * 0.05f;
    memset(M, 0, 64);
    M[0] = f / aspect; M[5] = f;
    M[8] = -c.proj0[12]; M[9] = -c.proj0[13];
    M[11] = -1.0f; M[14] = nearZ;
}

// Camera hooks
using GetProjFn = uintptr_t (*)(uintptr_t);
using ViewRebuildFn = void (*)(uintptr_t);
using RayBuildFn = void (*)(uintptr_t, float*, float*, float*);
GetProjFn OrigGetProj; ViewRebuildFn OrigViewRebuild; RayBuildFn OrigRayBuild;

// Camera operator=. The per-frame camera update 0x5F0F90 calls it once as
// copy(second, MASTER); that call identifies the rendered world camera.
using CamCopyFn = uintptr_t (*)(uintptr_t dst, uintptr_t src);
CamCopyFn OrigCamCopy;

uintptr_t HookCamCopy(uintptr_t dst, uintptr_t src) {
    const uintptr_t r = OrigCamCopy(dst, src);
    if ((uintptr_t)_ReturnAddress() == g_base + RVA_COPY_CALL + 5 && g_master.exchange(src) != src)
        Log("world camera %p (projection type %d, fov %.3f)", (void*)src, F<int32_t>(src, S_TYPE), F<float>(src, 0x12C));
    return r;
}

uintptr_t HookGetProj(uintptr_t s) {
    if (!s) return OrigGetProj(s);
    const bool wasDirty = F<uint8_t>(s, S_PDIRTY) != 0;
    const uintptr_t ret = OrigGetProj(s);
    AcquireSRWLockExclusive(&g_camLock);
    Cam* c = FindCam(s);
    if (!c && g_camCount < 8) {
        c = &g_cams[g_camCount++]; memset(c, 0, sizeof *c); c->ptr = s; c->lookSign = -1.0f;
        Log("camera %p seen (type %d)", (void*)s, F<int32_t>(s, S_TYPE));
    }
    if (c) {
        if (wasDirty || !c->haveBase) {
            memcpy(c->proj0, (void*)(s + S_PROJ), 64); c->haveBase = true;
            c->type = F<int32_t>(s, S_TYPE); c->extW = F<float>(s, S_EXTW); c->extH = F<float>(s, S_EXTH); c->nearZ = F<float>(s, S_NEAR);
        }
        const uint32_t gen = g_gen.load();
        if (c->lastGen != gen) { F<uint8_t>(s, S_VDIRTY) = 1; c->lastGen = gen; }
        if (g_enabled.load() && c->ptr == g_master.load()) {
            float M[16]; BuildProj(*c, M);
            memcpy((void*)(s + S_PROJ), M, 64);
            c->written = true;
        } else if (c->written) {
            F<uint8_t>(s, S_PDIRTY) = 1; c->written = false;
        }
    }
    ReleaseSRWLockExclusive(&g_camLock);
    return ret;
}

void HookViewRebuild(uintptr_t s) {
    OrigViewRebuild(s);
    if (!s || !g_enabled.load()) return;
    AcquireSRWLockExclusive(&g_camLock);
    Cam* c = FindCam(s);
    if (c && c->haveBase && c->ptr == g_master.load()) {
        float view[16]; memcpy(view, (void*)(s + S_VIEW), 64);
        float P[4] = {0, 0, -Radius(*c), 1};
        if (F<uint8_t>(s, S_LOOKAT_OK)) {   // pivot = look-at point lifted to body height (world y-up)
            const float L[4] = {F<float>(s, S_LOOKAT), F<float>(s, S_LOOKAT + 4) + kHeight, F<float>(s, S_LOOKAT + 8), 1};
            Vec4Mat(L, view, P);
        }
        const float sgn = P[2] <= 0.0f ? -1.0f : 1.0f;
        const float d2r = 3.14159265f / 180.0f;
        float R[16], Ry[16], RR[16];
        AxisRot4(R, 1, 0, 0, g_pitch.load() * d2r);
        const float un = sqrtf(view[4]*view[4] + view[5]*view[5] + view[6]*view[6]);   // world up in view space
        if (g_yaw.load() != 0.0f && un > 1e-6f) { AxisRot4(Ry, view[4]/un, view[5]/un, view[6]/un, g_yaw.load() * d2r); Mul4(Ry, R, RR); memcpy(R, RR, 64); }
        float A[16]; memcpy(A, R, 64);
        A[12] = -(P[0]*R[0] + P[1]*R[4] + P[2]*R[8]);
        A[13] = -(P[0]*R[1] + P[1]*R[5] + P[2]*R[9]);
        A[14] = sgn * Radius(*c) - (P[0]*R[2] + P[1]*R[6] + P[2]*R[10]);
        float folded[16], inv[16];
        Mul4(view, A, folded);
        memcpy((void*)(s + S_VIEW), folded, 64);
        if (Invert4(folded, inv)) memcpy((void*)(s + S_INVVIEW), inv, 64);
        c->lookSign = sgn;
    }
    ReleaseSRWLockExclusive(&g_camLock);
}

// Mouse ray through the camera's live (our) matrices: origin at the eye, unit direction.
bool UnprojectRay(uintptr_t s, const float* mouse, float lookSign, float* origin, float* dir) {
    float V[16], P[16], invV[16], invP[16];
    memcpy(V, (void*)(s + S_VIEW), 64); memcpy(P, (void*)(s + S_PROJ), 64);
    const float W = F<float>(s, S_VPW), H = F<float>(s, S_VPH);
    if (!(W > 0 && H > 0) || !Invert4(V, invV) || !Invert4(P, invP)) return false;
    const float nx = 2.0f * mouse[0] / W - 1.0f, ny = 1.0f - 2.0f * mouse[1] / H;
    float a[4], b[4];
    const float ca[4] = {nx, ny, 1.0f, 1}, cb[4] = {nx, ny, 0.25f, 1};   // two depths on the ray
    Vec4Mat(ca, invP, a); Vec4Mat(cb, invP, b);
    if (fabsf(a[3]) < 1e-12f || fabsf(b[3]) < 1e-12f) return false;
    float d[4] = {b[0]/b[3] - a[0]/a[3], b[1]/b[3] - a[1]/a[3], b[2]/b[3] - a[2]/a[3], 0};
    if (d[2] * lookSign < 0) { d[0] = -d[0]; d[1] = -d[1]; d[2] = -d[2]; }
    const float eye[4] = {0, 0, 0, 1};
    float ow[4], dw[4]; Vec4Mat(eye, invV, ow); Vec4Mat(d, invV, dw);
    const float n = sqrtf(dw[0]*dw[0] + dw[1]*dw[1] + dw[2]*dw[2]);
    if (!(n > 1e-9f) || fabsf(ow[3]) < 1e-12f) return false;
    for (int i = 0; i < 3; ++i) { origin[i] = ow[i] / ow[3]; dir[i] = dw[i] / n; }
    return true;
}

void HookRayBuild(uintptr_t s, float* mouse, float* origin, float* dir) {
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    OrigRayBuild(s, mouse, origin, dir);
    if (!s || !mouse || !origin || !dir || !g_enabled.load()) return;
    bool world = false; float lookSign = -1.0f;
    AcquireSRWLockShared(&g_camLock);
    if (const Cam* c = FindCam(s)) { world = c->ptr == g_master.load(); lookSign = c->lookSign; }
    ReleaseSRWLockShared(&g_camLock);
    float o[3], d[3];
    if (world && UnprojectRay(s, mouse, lookSign, o, d)) { memcpy(origin, o, 12); memcpy(dir, d, 12); }
    // The directional shadow fits its box by intersecting screen-corner rays with the ground;
    // at shallow pitch those rays run nearly flat and the box explodes. Clamp their slope.
    if (g_shadowOk && ra >= g_base + RVA_SHADOWFIT && ra < g_base + RVA_SHADOWFIT_END && dir[1] > -kShadowSlope) {
        float hx = dir[0], hz = dir[2], hl = sqrtf(hx*hx + hz*hz);
        if (hl < 1e-6f) { hx = 0; hz = 1; hl = 1; }
        const float k = sqrtf(1.0f - kShadowSlope * kShadowSlope);
        dir[0] = hx / hl * k; dir[1] = -kShadowSlope; dir[2] = hz / hl * k;
    }
}

// Room loading
using ActTestFn = char (*)(uintptr_t mgr, uintptr_t room);
ActTestFn OrigActTest;

char HookActTest(uintptr_t mgr, uintptr_t room) {
    const char r = OrigActTest(mgr, room);
    g_roomMgr.store(mgr);
    return g_enabled.load() ? 1 : r;
}

// Builds rings 2..2+kBuildRings of client rooms
constexpr size_t R_NEAR = 0x10, R_NEARN = 0x18, R_SNEXT = 0x38, R_LNEXT = 0x48, R_FLAGS = 0x50,
                 R_ACTIVE = 0x58, R_STATUS = 0x70;
constexpr size_t L_FIRST = 0x10, L_NEXT = 0x1B8;
constexpr size_t D_FLAGS = 0x110, D_BUILT = 0x124, D_LIST0 = 0x130, D_LISTSTRIDE = 0x1C0, D_LEVELS = 0x868;
constexpr uint32_t HAS_ROOM = 0x100000;
constexpr int kNativeRings = 2; // the game itself builds rings 0..2
template <class T> T rd(uintptr_t a) { return *(const T*)a; }

using RoomInitFn = uintptr_t (*)(uint8_t ctx, uintptr_t room);
using RoomReleaseFn = void (*)(uintptr_t room, int keepRoom);
using RoomLinkFn = void (*)(uint8_t ctx, uintptr_t room);
std::atomic<bool> g_buildOk{false}, g_buildFault{false};
uintptr_t g_ownedDrlg = 0;
std::unordered_set<uintptr_t> g_owned;
uint64_t g_passes = 0;

void RoomBuilderPass(uint8_t ctx, uintptr_t drlg) {
    if (drlg != g_ownedDrlg) { g_owned.clear(); g_ownedDrlg = drlg; }
    const int extra = g_enabled.load() ? kBuildRings : 0;
    if (extra <= 0 && g_owned.empty()) return;
    const int maxBuild = extra > 0 ? kNativeRings + extra : -1;
    const int keepRing = extra > 0 ? maxBuild + 2 : -1; // hysteresis before release

    // BFS over the room graph from the status-0 rooms (the player's), else status 1.
    static std::unordered_map<uintptr_t, int> depth;
    static std::vector<uintptr_t> order;
    depth.clear(); order.clear();
    for (int s = 0; s < 2 && order.empty(); ++s) {
        const uintptr_t head = drlg + D_LIST0 + s * D_LISTSTRIDE;
        for (uintptr_t r = rd<uintptr_t>(head + R_SNEXT); r && r != head && order.size() < 64; r = rd<uintptr_t>(r + R_SNEXT))
            if (depth.emplace(r, 0).second) order.push_back(r);
    }
    for (size_t i = 0; i < order.size() && order.size() < 8192; ++i) {
        const uintptr_t r = order[i];
        const int d = depth[r];
        if (d >= keepRing) continue;
        if (rd<uint64_t>(r + R_NEARN) == 0) ((RoomLinkFn)(g_base + RVA_ROOMLINK))(ctx, r);   // near list is built lazily
        const uintptr_t data = rd<uintptr_t>(r + R_NEAR);
        const uint64_t n = rd<uint64_t>(r + R_NEARN);
        if (!data || n == 0 || n > 64) continue;
        for (uint64_t k = 0; k < n; ++k)
            if (const uintptr_t nb = rd<uintptr_t>(data + k * 8); nb && depth.emplace(nb, d + 1).second) order.push_back(nb);
    }

    int built = 0, released = 0;
    if (maxBuild >= kNativeRings) {
        int budget = kBuildRate;
        const uint8_t savedBuilt = rd<uint8_t>(drlg + D_BUILT); // don't throttle the game's own builder
        for (const uintptr_t r : order) {
            const int d = depth[r];
            if (d < kNativeRings || d > maxBuild || rd<uintptr_t>(r + R_ACTIVE) || (rd<uint32_t>(r + R_FLAGS) & HAS_ROOM)) continue;
            if (budget-- <= 0) break;
            ((RoomInitFn)(g_base + RVA_ROOMINIT))(ctx, r);
            if (rd<uintptr_t>(r + R_ACTIVE)) { g_owned.insert(r); ++built; }
        }
        *(uint8_t*)(drlg + D_BUILT) = savedBuilt;
    }
    if (!g_owned.empty()) {
        static std::unordered_set<uintptr_t> all; // guard against freed rooms
        all.clear();
        for (uintptr_t lv = rd<uintptr_t>(drlg + D_LEVELS); lv && all.size() < 65536; lv = rd<uintptr_t>(lv + L_NEXT))
            for (uintptr_t r = rd<uintptr_t>(lv + L_FIRST); r && all.size() < 65536; r = rd<uintptr_t>(r + R_LNEXT)) all.insert(r);
        int budget = 16;
        for (auto it = g_owned.begin(); it != g_owned.end();) {
            const uintptr_t r = *it;
            if (!all.count(r) || !rd<uintptr_t>(r + R_ACTIVE)) { it = g_owned.erase(it); continue; }
            const auto dIt = depth.find(r);
            if ((dIt != depth.end() && dIt->second <= keepRing) || rd<uint8_t>(r + R_STATUS) < 4 || budget <= 0) { ++it; continue; }
            ((RoomReleaseFn)(g_base + RVA_ROOMRELEASE))(r, 0); --budget; ++released;
            it = g_owned.erase(it);
        }
    }
    if (g_passes++ < 5 || built || released)
        Log("rooms: pass %llu, %zu rooms in reach, built %d, released %d, own %zu", (unsigned long long)g_passes, order.size(), built, released, g_owned.size());
}

int SehRoomBuilderPass(uint8_t ctx, uintptr_t drlg) {
    __try { RoomBuilderPass(ctx, drlg); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Client "room in sight" handler (ctx, obj, levelId, x, y, activeRoom); obj+0x70 = client DRLG.
using SightAddFn = void (*)(uintptr_t ctx, uintptr_t obj, int levelId, int x, int y, uintptr_t aroom);
SightAddFn OrigSightAdd;

void HookSightAdd(uintptr_t ctx, uintptr_t obj, int levelId, int x, int y, uintptr_t aroom) {
    OrigSightAdd(ctx, obj, levelId, x, y, aroom);
    if (!g_buildOk.load() || g_buildFault.load()) return;
    if (!g_enabled.load() && g_owned.empty()) return;
    static DWORD owner = GetCurrentThreadId();
    static ULONGLONG last = 0;
    uintptr_t drlg = 0; uint32_t flags = 0;
    if (GetCurrentThreadId() != owner || GetTickCount64() - last < 20) return;
    if (!obj || !SafeRead(&drlg, (void*)(obj + 0x70), 8) || !drlg || !SafeRead(&flags, (void*)(drlg + D_FLAGS), 4) || !(flags & 1)) return;
    last = GetTickCount64();
    if (!SehRoomBuilderPass((uint8_t)ctx, drlg)) { g_buildFault.store(true); Log("rooms: fault in the room builder - disabled"); }
}

// Rendering distance
bool g_mvOk = false, g_depthOk = false;
float g_mvApplied = 150.0f;
int g_nativeDepth = -1;

// Model visibility radius: game keeps it as a constant radius^2 in .rdata.
void ApplyMvRadius() {
    const float want = g_enabled.load() ? kMvRadius : 150.0f;
    if (!g_mvOk || want == g_mvApplied) return;
    float* p = (float*)(g_base + RVA_MVRADIUS_SQ);
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) return;
    *p = want * want;
    VirtualProtect(p, 4, old, &tmp);
    g_mvApplied = want;
    Log("model visibility radius = %.0f", want);
}

void ApplyActDepth() {
    const uintptr_t mgr = g_roomMgr.load();
    int cur = 0;
    if (!g_depthOk || !mgr || !SafeRead(&cur, (void*)(mgr + 0xF6C), 4)) return;
    if (g_nativeDepth < 0) g_nativeDepth = cur;
    const int want = g_enabled.load() ? std::max(g_nativeDepth, kNativeRings + kBuildRings) : g_nativeDepth;
    if (cur != want) { *(int*)(mgr + 0xF6C) = want; Log("candidate walk depth %d -> %d", cur, want); }
}

// Grow the fixed TLSF pools by adding extra pools under their own lock, the way
// tlsf_add_pool does it: size header, the game's block_insert, end sentinel.
struct PoolGrow { const char* name; uint64_t objRva; uint64_t vanilla; int targetMB; bool done = false; };
PoolGrow g_pools[] = {
    {"renderer pool", RVA_RPOOL_OBJ, 0xC00000,  kRenderPoolMB},
    {"entity pool",   RVA_EPOOL_OBJ, 0x4600000, kEntityPoolMB},
};

void GrowPool(PoolGrow& p) {
    if (p.done) return;
    const uintptr_t obj = g_base + p.objRva;
    uintptr_t vt = 0, control = 0; uint64_t size = 0;
    if (!SafeRead(&vt, (void*)obj, 8) || !SafeRead(&size, (void*)(obj + 0x18), 8) || !SafeRead(&control, (void*)(obj + 0x20), 8)) return;
    if (vt != g_base + RVA_RPOOL_VTBL || !control || size != p.vanilla) return;   // not created yet
    p.done = true;
    using LockFn = void (*)(void*);
    LockFn lock = nullptr, unlock = nullptr;
    if (!Verify("TlsfInsert") || !SafeRead(&lock, (void*)(g_base + RVA_IAT_LOCK), 8) || !SafeRead(&unlock, (void*)(g_base + RVA_IAT_UNLOCK), 8) || !lock || !unlock) {
        Log("%s: not grown (verification failed)", p.name); return;
    }
    const auto insert = (void (*)(uintptr_t, uintptr_t))(g_base + RVA_TLSF_INSERT);
    const int vanillaMB = (int)(p.vanilla >> 20);
    int addedMB = 0;
    for (int add = p.targetMB - vanillaMB; add > 0;) {
        const int mb = std::min(add, 60);
        const size_t bytes = (size_t)mb << 20;
        uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!mem) { Log("%s: VirtualAlloc failed (%lu)", p.name, GetLastError()); return; }
        const uint64_t poolBytes = (bytes - 16) & ~7ull;
        lock((void*)(obj + 0x28));
        *(uint64_t*)mem = poolBytes | 1;                       // free block, previous in use
        insert(control, (uintptr_t)mem - 8);
        *(uintptr_t*)(mem + poolBytes) = (uintptr_t)mem - 8;   // sentinel: prev = our block
        *(uint64_t*)(mem + poolBytes + 8) = 2;                 // size 0, previous free
        unlock((void*)(obj + 0x28));
        addedMB += mb; add -= mb;
    }
    Log("%s: %d MB -> %d MB", p.name, vanillaMB, vanillaMB + addedMB);
}
void GrowPools() { for (auto& p : g_pools) GrowPool(p); }

// Hooks
bool Hook(const char* name, uint64_t rva, void* detour, void** orig) {
    if (!Verify(name)) return false;
    void* t = (void*)(g_base + rva);
    if (MH_CreateHook(t, detour, orig) != MH_OK || MH_EnableHook(t) != MH_OK) { Log("  %s: hook failed", name); return false; }
    Log("  %s hooked", name);
    return true;
}

bool InstallHooks() {
    Log("installing (image base %p)", (void*)g_base);
    if (!Verify("CopyCall") || !Hook("CamCopy", RVA_CAMCOPY, (void*)&HookCamCopy, (void**)&OrigCamCopy) ||
        !Hook("GetProj", RVA_GETPROJ, (void*)&HookGetProj, (void**)&OrigGetProj) ||
        !Hook("ViewRebuild", RVA_VIEWREBUILD, (void*)&HookViewRebuild, (void**)&OrigViewRebuild)) {
        Log("camera functions not found - wrong game version, or not in a game yet");
        return false;
    }
    g_shadowOk = Verify("ShadowFit");
    Hook("RayBuild", RVA_RAYBUILD, (void*)&HookRayBuild, (void**)&OrigRayBuild);
    Hook("ActTest", RVA_ACTTEST, (void*)&HookActTest, (void**)&OrigActTest);
    if (Verify("RoomInit") && Verify("RoomRelease") && Verify("RoomLink"))
        g_buildOk.store(Hook("SightAdd", RVA_SIGHTADD, (void*)&HookSightAdd, (void**)&OrigSightAdd));
    g_depthOk = Verify("ActDepthCmp");
    float sq = 0;
    g_mvOk = Verify("MvRadiusLd") && SafeRead(&sq, (void*)(g_base + RVA_MVRADIUS_SQ), 4) && sq >= 1.0f && sq < 1e12f;
    if (g_mvOk) g_mvApplied = sqrtf(sq);
    return true;
}

HHOOK g_mouseHook = nullptr;
bool g_mmbDown = false;
POINT g_mmbLast{};

bool GameFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Wheel = zoom (camera distance)
// Middle mouse drag = yaw/pitch around the player.
LRESULT CALLBACK MouseProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && g_enabled.load() && GameFocused()) {
        const auto* ms = (const MSLLHOOKSTRUCT*)l;
        if (w == WM_MOUSEWHEEL) {
            const int steps = (short)HIWORD(ms->mouseData) / WHEEL_DELTA;
            g_dist.store(std::clamp(g_dist.load() / powf(1.12f, (float)steps), 3.5f, 1500.0f));
            g_gen.fetch_add(1);
            if (kWheelSwallow) return 1;
        } else if (w == WM_MBUTTONDOWN) { g_mmbDown = true; g_mmbLast = ms->pt; }
        else if (w == WM_MBUTTONUP) g_mmbDown = false;
        else if (w == WM_MOUSEMOVE && g_mmbDown) {
            const int dx = ms->pt.x - g_mmbLast.x, dy = ms->pt.y - g_mmbLast.y;
            g_mmbLast = ms->pt;
            float yaw = g_yaw.load() + dx * 0.25f;
            yaw -= 360.0f * floorf((yaw + 180.0f) / 360.0f);
            g_yaw.store(yaw);
            g_pitch.store(std::clamp(g_pitch.load() + dy * 0.15f, -85.0f, 60.0f));
            g_gen.fetch_add(1);
        }
    } else if (code == HC_ACTION && w == WM_MBUTTONUP) g_mmbDown = false;
    return CallNextHookEx(g_mouseHook, code, w, l);
}

DWORD WINAPI MouseThread(void*) {
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_self, 0);
    if (!g_mouseHook) Log("mouse hook failed (%lu)", GetLastError());
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}

DWORD WINAPI Worker(void*) {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    MH_Initialize();
    if (HANDLE h = CreateThread(nullptr, 0, MouseThread, nullptr, 0, nullptr)) CloseHandle(h);
    Log("d2r-3d loaded (pid %lu) - press F12 in a game", GetCurrentProcessId());
    bool f12Was = false;
    for (ULONGLONG lastApply = 0;; Sleep(8)) {
        const bool f12 = GameFocused() && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (f12 && !f12Was) {
            if (!g_hooked.load()) g_hooked.store(InstallHooks());
            if (g_hooked.load()) {
                g_enabled.store(!g_enabled.load());
                g_gen.fetch_add(1);
                Log("d2r-3d %s", g_enabled.load() ? "ON" : "OFF");
            }
        }
        f12Was = f12;
        if (GetTickCount64() - lastApply > 250) {
            lastApply = GetTickCount64();
            GrowPools(); // As early as possible
            if (g_hooked.load()) { ApplyMvRadius(); ApplyActDepth(); }
        }
        FlushLog();
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        DWORD n = GetModuleFileNameW(inst, g_logPath, MAX_PATH);
        while (n && g_logPath[n - 1] != L'\\') --n;
        g_logPath[n] = 0;
        wcscat_s(g_logPath, L"d2r-3d.log");
        DeleteFileW(g_logPath);
        if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(h);
    }
    return TRUE;
}
