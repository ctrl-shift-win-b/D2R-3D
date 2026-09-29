#include "eye_math.h"

#include <cmath>
#include <cstdio>

static int g_fails = 0;

static void Check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL %s\n", what);
    ++g_fails;
}

static bool Near(float a, float b, float eps = 1e-4f) { return fabsf(a - b) <= eps; }

static EyeFov Fov60_16x9() {
    const float tanY = tanf(30.0f * 3.14159265f / 180.0f);
    const float aspect = 16.0f / 9.0f;
    EyeFov f;
    f.tanUp = tanY;
    f.tanDown = -tanY;
    f.tanRight = tanY * aspect;
    f.tanLeft = -f.tanRight;
    f.nearZ = 0.05f;
    return f;
}

static void TestProjMatchesSymmetricBuilder() {
    const EyeFov fov = Fov60_16x9();
    float p[16];
    BuildEyeProj(fov, p);
    const float f = 1.0f / tanf(60.0f * 3.14159265f / 360.0f);
    const float aspect = 16.0f / 9.0f;
    Check(Near(p[0], f / aspect), "proj[0] matches BuildProj f/aspect");
    Check(Near(p[5], f), "proj[5] matches BuildProj f");
    Check(p[10] == 0.0f && p[11] == -1.0f && Near(p[14], 0.05f), "reverse-Z slots");
    Check(Near(p[8], 0.0f) && Near(p[9], 0.0f), "symmetric fov has no offset");
}

static void TestCenterAndRaisedPoint() {
    const V3 eye{0, 8, 8};
    const V3 at{0, 0, 0};
    const V3 forward{at.x - eye.x, at.y - eye.y, at.z - eye.z};
    const V3 up{0, 1, 0};
    float view[16], proj[16];
    Check(BuildView(eye, forward, up, view), "build view");
    BuildEyeProj(Fov60_16x9(), proj);

    float nx, ny, nz;
    Check(Project(view, proj, at, nx, ny, nz), "origin in front");
    Check(Near(nx, 0.0f) && Near(ny, 0.0f), "origin at NDC center");

    float rx, ry, rz;
    const V3 raised{0, 1, 0};
    Check(Project(view, proj, raised, rx, ry, rz), "raised point in front");
    Check(ry > ny + 0.01f, "point one unit above the origin lands above it");
    Check(Near(rx, 0.0f, 1e-3f), "raised point stays on the vertical center line");

    const V3 behind{0, 20, 20};
    Check(!Project(view, proj, behind, nx, ny, nz), "point behind the eye is rejected");
}

static void TestRightEyeShiftsOrigin() {
    const V3 forward{0, -8, -8};
    const V3 up{0, 1, 0};
    const V3 origin{0, 0, 0};
    float proj[16];
    BuildEyeProj(Fov60_16x9(), proj);

    float left[16], right[16];
    Check(BuildView(V3{-0.4f, 8, 8}, forward, up, left), "left view");
    Check(BuildView(V3{0.4f, 8, 8}, forward, up, right), "right view");

    float lx, ly, lz, rx, ry, rz;
    Check(Project(left, proj, origin, lx, ly, lz), "left sees origin");
    Check(Project(right, proj, origin, rx, ry, rz), "right sees origin");
    Check(lx > 0.02f, "left eye: origin shifts right");
    Check(rx < -0.02f, "right eye: origin shifts left");
    Check(fabsf(lx + rx) < 1e-3f, "the horizontal shifts are opposite");
}

static void TestDisc() {
    Check(DiscAlpha(0, 4, 8) == 1.0f, "center of the disc is opaque");
    Check(DiscAlpha(4, 4, 8) == 1.0f, "inner edge is opaque");
    Check(DiscAlpha(8, 4, 8) == 0.0f, "outer edge is clear");
    Check(Near(DiscAlpha(6, 4, 8), 0.5f), "mid feather is half");

    const V3 eye{0, 8, 8};
    const V3 forward{-eye.x, -eye.y, -eye.z};
    float view[16], proj[16];
    Check(BuildView(eye, forward, V3{0, 1, 0}, view), "disc view");
    BuildEyeProj(Fov60_16x9(), proj);

    float alpha = -1;
    Check(DiscAlphaAt(view, proj, 0, 0, 0, 0, 0, 4, 8, alpha), "center ray hits the ground");
    Check(Near(alpha, 1.0f), "center ray is inside the disc");

    // A steep ndc x leaves the outer radius. The exact hit is checked by alpha, not by a guessed ndc.
    Check(DiscAlphaAt(view, proj, 0.85f, 0, 0, 0, 0, 4, 8, alpha), "edge ray hits the ground");
    Check(alpha < 0.05f, "edge ray is outside the disc");

    float flat[16];
    Check(BuildView(V3{0, 2, 10}, V3{0, 0, -1}, V3{0, 1, 0}, flat), "horizontal view");
    Check(!DiscAlphaAt(flat, proj, 0, 0.5f, 0, 0, 0, 4, 8, alpha), "ray aimed above the horizon misses the ground");
    Check(DiscAlphaAt(flat, proj, 0, -0.5f, 0, 0, 0, 4, 8, alpha), "ray aimed below the horizon hits the ground");
}

static void TestEyeOnTheTable() {
    // Disc center at the stage origin. Head is 45 cm up and 35 cm back.
    // 1 cm per world unit, so the eye sits at world (0, 45, 35) above the player.
    const V3 eye = EyeWorldFromStage(V3{10, 0, -4}, V3{0, 0.45f, 0.35f}, V3{0, 0, 0}, 0, 0.01f);
    Check(Near(eye.x, 10.0f) && Near(eye.y, 45.0f) && Near(eye.z, 31.0f), "eye sits above the player on the table");

    const V3 right = EyeWorldFromStage(V3{0, 0, 0}, V3{0.032f, 0.45f, 0.35f}, V3{0, 0, 0}, 0, 0.01f);
    Check(Near(right.x, 3.2f), "right eye is a half-IPD to world +X");
}

static void TestCenterRayHitsPlayer() {
    const V3 eye{0, 8, 8};
    float view[16], proj[16];
    Check(BuildView(eye, V3{-eye.x, -eye.y, -eye.z}, V3{0, 1, 0}, view), "ray view");
    BuildEyeProj(Fov60_16x9(), proj);
    V3 origin, dir;
    Check(EyeRay(view, proj, 0, 0, origin, dir), "center ray");
    Check(Near(origin.x, eye.x) && Near(origin.y, eye.y) && Near(origin.z, eye.z), "ray starts at the eye");
    Check(fabsf(dir.y) > 1e-4f, "center ray is not parallel to the ground");
    const float t = (0 - origin.y) / dir.y;
    Check(t > 0 && Near(origin.x + dir.x * t, 0, 1e-3f) && Near(origin.z + dir.z * t, 0, 1e-3f),
          "center ray hits the player on the ground");
}

static void TestStageBaseline() {
    // 6.3 cm of stage +X, 1 cm per world unit, yaw 0 -> +6.3 world X. That is the stereo baseline.
    const V3 w = StageDeltaToWorld(V3{0.063f, 0, 0}, 0, 0.01f);
    Check(Near(w.x, 6.3f) && Near(w.y, 0) && Near(w.z, 0), "IPD maps onto world X");

    const float half = 3.14159265f / 2.0f;
    const V3 turned = StageDeltaToWorld(V3{0.01f, 0.02f, 0}, half, 0.01f);
    Check(Near(turned.x, 0) && Near(turned.y, 2.0f) && Near(turned.z, 1.0f), "yaw 90 takes stage +X to world +Z");
}

int main() {
    TestProjMatchesSymmetricBuilder();
    TestCenterAndRaisedPoint();
    TestRightEyeShiftsOrigin();
    TestDisc();
    TestEyeOnTheTable();
    TestCenterRayHitsPlayer();
    TestStageBaseline();
    if (g_fails) {
        std::printf("%d failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
