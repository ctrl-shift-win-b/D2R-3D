#include "eye_math.h"

#include <cmath>
#include <cstring>

void Vec4Mat(const float v[4], const float m[16], float o[4]) {
    for (int j = 0; j < 4; ++j)
        o[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
}

bool Invert4(const float m[16], float out[16]) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            a[i][j] = m[i * 4 + j];
            a[i][j + 4] = i == j;
        }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-12) return false;
        for (int j = 0; j < 8; ++j) {
            const double s = a[col][j];
            a[col][j] = a[piv][j];
            a[piv][j] = s;
        }
        const double d = a[col][col];
        for (int j = 0; j < 8; ++j) a[col][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = (float)a[i][j + 4];
    return true;
}

static bool Norm(float& x, float& y, float& z) {
    const float n = sqrtf(x * x + y * y + z * z);
    if (!(n > 1e-8f)) return false;
    x /= n;
    y /= n;
    z /= n;
    return true;
}

bool BuildView(V3 eye, V3 forward, V3 up, float view[16]) {
    float fx = forward.x, fy = forward.y, fz = forward.z;
    if (!Norm(fx, fy, fz)) return false;
    float rx = fy * up.z - fz * up.y;
    float ry = fz * up.x - fx * up.z;
    float rz = fx * up.y - fy * up.x;
    if (!Norm(rx, ry, rz)) return false;
    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;
    memset(view, 0, 64);
    view[0] = rx;
    view[1] = ux;
    view[2] = -fx;
    view[4] = ry;
    view[5] = uy;
    view[6] = -fy;
    view[8] = rz;
    view[9] = uz;
    view[10] = -fz;
    view[12] = -(eye.x * rx + eye.y * ry + eye.z * rz);
    view[13] = -(eye.x * ux + eye.y * uy + eye.z * uz);
    view[14] = eye.x * fx + eye.y * fy + eye.z * fz;
    view[15] = 1.0f;
    return true;
}

void BuildEyeProj(const EyeFov& fov, float proj[16]) {
    const float rl = fov.tanRight - fov.tanLeft;
    const float tb = fov.tanUp - fov.tanDown;
    memset(proj, 0, 64);
    proj[0] = 2.0f / rl;
    proj[5] = 2.0f / tb;
    proj[8] = (fov.tanLeft + fov.tanRight) / rl;
    proj[9] = (fov.tanDown + fov.tanUp) / tb;
    proj[11] = -1.0f;
    proj[14] = fov.nearZ;
}

bool Project(const float view[16], const float proj[16], V3 world, float& ndcX, float& ndcY, float& ndcZ) {
    const float w[4] = {world.x, world.y, world.z, 1.0f};
    float viewP[4], clip[4];
    Vec4Mat(w, view, viewP);
    Vec4Mat(viewP, proj, clip);
    if (!(clip[3] > 1e-8f)) return false;
    ndcX = clip[0] / clip[3];
    ndcY = clip[1] / clip[3];
    ndcZ = clip[2] / clip[3];
    return true;
}

float DiscAlpha(float distance, float inner, float outer) {
    if (!(outer > inner)) return distance <= inner ? 1.0f : 0.0f;
    if (distance <= inner) return 1.0f;
    if (distance >= outer) return 0.0f;
    float t = (distance - inner) / (outer - inner);
    t = t * t * (3.0f - 2.0f * t);
    return 1.0f - t;
}

bool EyeRay(const float view[16], const float proj[16], float ndcX, float ndcY, V3& origin, V3& dir) {
    float invV[16], invP[16];
    if (!Invert4(view, invV) || !Invert4(proj, invP)) return false;
    // Two finite reverse-Z depths. z = 0 is the infinite far plane and its
    // unproject has w = 0, so it cannot be perspective-divided.
    const float ca[4] = {ndcX, ndcY, 1.0f, 1.0f};
    const float cb[4] = {ndcX, ndcY, 0.25f, 1.0f};
    float a[4], b[4];
    Vec4Mat(ca, invP, a);
    Vec4Mat(cb, invP, b);
    if (fabsf(a[3]) < 1e-12f || fabsf(b[3]) < 1e-12f) return false;
    float d[4] = {b[0] / b[3] - a[0] / a[3], b[1] / b[3] - a[1] / a[3], b[2] / b[3] - a[2] / a[3], 0.0f};
    // lookSign is -1: a ray that points toward +Z is flipped so it points into the scene.
    if (d[2] > 0.0f) {
        d[0] = -d[0];
        d[1] = -d[1];
        d[2] = -d[2];
    }
    const float eye[4] = {0, 0, 0, 1};
    float ow[4], dw[4];
    Vec4Mat(eye, invV, ow);
    Vec4Mat(d, invV, dw);
    const float n = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
    if (!(n > 1e-8f) || fabsf(ow[3]) < 1e-12f) return false;
    origin = {ow[0] / ow[3], ow[1] / ow[3], ow[2] / ow[3]};
    dir = {dw[0] / n, dw[1] / n, dw[2] / n};
    return true;
}

bool DiscAlphaAt(const float view[16], const float proj[16], float ndcX, float ndcY, float groundY,
                 float playerX, float playerZ, float inner, float outer, float& alpha) {
    V3 origin, dir;
    if (!EyeRay(view, proj, ndcX, ndcY, origin, dir) || fabsf(dir.y) < 1e-8f) return false;
    const float t = (groundY - origin.y) / dir.y;
    if (!(t > 0.0f)) return false;
    const float hx = origin.x + dir.x * t;
    const float hz = origin.z + dir.z * t;
    const float dist = sqrtf((hx - playerX) * (hx - playerX) + (hz - playerZ) * (hz - playerZ));
    alpha = DiscAlpha(dist, inner, outer);
    return true;
}

V3 EyeWorldFromStage(V3 playerGround, V3 eyeStage, V3 anchorStage, float yawRadians, float metersPerUnit) {
    const V3 delta{eyeStage.x - anchorStage.x, eyeStage.y - anchorStage.y, eyeStage.z - anchorStage.z};
    const V3 world = StageDeltaToWorld(delta, yawRadians, metersPerUnit);
    return {playerGround.x + world.x, playerGround.y + world.y, playerGround.z + world.z};
}

V3 StageDeltaToWorld(V3 deltaStageMeters, float yawRadians, float metersPerUnit) {
    const float c = cosf(yawRadians);
    const float s = sinf(yawRadians);
    const float inv = 1.0f / metersPerUnit;
    V3 w;
    w.x = (c * deltaStageMeters.x - s * deltaStageMeters.z) * inv;
    w.y = deltaStageMeters.y * inv;
    w.z = (s * deltaStageMeters.x + c * deltaStageMeters.z) * inv;
    return w;
}
