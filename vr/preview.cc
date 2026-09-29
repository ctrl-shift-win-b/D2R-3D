// Software stereo pair of a tabletop disc. No headset and no game.
// Writes vr/preview/left.bmp, right.bmp, and both.bmp.

#include "eye_math.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kW = 640;
constexpr int kH = 360;
constexpr float kInner = 18.0f;
constexpr float kOuter = 32.0f;
constexpr float kMetersPerUnit = 0.01f;

struct Rgb {
    uint8_t r, g, b;
};

bool WriteBmp(const char* path, const Rgb* px, int w, int h) {
    const int row = (w * 3 + 3) & ~3;
    const int pixels = row * h;
    const int size = 54 + pixels;
    std::vector<uint8_t> f((size_t)size, 0);
    f[0] = 'B';
    f[1] = 'M';
    f[2] = (uint8_t)size;
    f[3] = (uint8_t)(size >> 8);
    f[4] = (uint8_t)(size >> 16);
    f[5] = (uint8_t)(size >> 24);
    f[10] = 54;
    f[14] = 40;
    f[18] = (uint8_t)w;
    f[19] = (uint8_t)(w >> 8);
    f[22] = (uint8_t)h;
    f[23] = (uint8_t)(h >> 8);
    f[26] = 1;
    f[28] = 24;
    for (int y = 0; y < h; ++y) {
        uint8_t* dst = f.data() + 54 + (size_t)(h - 1 - y) * row;
        for (int x = 0; x < w; ++x) {
            const Rgb p = px[(size_t)y * w + x];
            dst[x * 3 + 0] = p.b;
            dst[x * 3 + 1] = p.g;
            dst[x * 3 + 2] = p.r;
        }
    }
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    const size_t n = fwrite(f.data(), 1, f.size(), fp);
    fclose(fp);
    return n == f.size();
}

bool Box(V3 o, V3 d, V3 b0, V3 b1, float& tHit) {
    float t0 = 0.001f, t1 = 1.0e6f;
    const float o1[3] = {o.x, o.y, o.z};
    const float d1[3] = {d.x, d.y, d.z};
    const float a[3] = {b0.x, b0.y, b0.z};
    const float b[3] = {b1.x, b1.y, b1.z};
    for (int i = 0; i < 3; ++i) {
        if (fabsf(d1[i]) < 1e-8f) {
            if (o1[i] < a[i] || o1[i] > b[i]) return false;
            continue;
        }
        float ta = (a[i] - o1[i]) / d1[i];
        float tb = (b[i] - o1[i]) / d1[i];
        if (ta > tb) {
            const float s = ta;
            ta = tb;
            tb = s;
        }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    tHit = t0;
    return true;
}

Rgb Mix(Rgb a, Rgb b, float t) {
    auto c = [&](uint8_t u, uint8_t v) { return (uint8_t)(u + (v - u) * t); };
    return {c(a.r, b.r), c(a.g, b.g), c(a.b, b.b)};
}

Rgb Shade(V3 hit, int id) {
    if (id == 1) return {190, 48, 36};
    if (id == 2) return {36, 92, 190};
    const int cx = (int)floorf(hit.x / 4.0f);
    const int cz = (int)floorf(hit.z / 4.0f);
    const bool dark = ((cx + cz) & 1) != 0;
    const float ring = fabsf(sqrtf(hit.x * hit.x + hit.z * hit.z) - kInner);
    if (ring < 0.35f) return {230, 210, 140};
    return dark ? Rgb{92, 78, 48} : Rgb{168, 146, 86};
}

void Render(const float view[16], const float proj[16], Rgb* px) {
    const Rgb room{18, 28, 22};
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const float nx = (x + 0.5f) / kW * 2.0f - 1.0f;
            const float ny = 1.0f - (y + 0.5f) / kH * 2.0f;
            V3 origin, dir;
            Rgb out = room;
            if (EyeRay(view, proj, nx, ny, origin, dir)) {
                float best = 1.0e6f;
                int id = 0;
                V3 hit{};
                if (fabsf(dir.y) > 1e-8f) {
                    const float t = (0 - origin.y) / dir.y;
                    if (t > 0.001f) {
                        best = t;
                        hit = {origin.x + dir.x * t, 0, origin.z + dir.z * t};
                    }
                }
                float tb;
                if (Box(origin, dir, {-1.2f, 0, -1.2f}, {1.2f, 6, 1.2f}, tb) && tb < best) {
                    best = tb;
                    id = 1;
                    hit = {origin.x + dir.x * tb, origin.y + dir.y * tb, origin.z + dir.z * tb};
                }
                if (Box(origin, dir, {10, 0, -2}, {14, 5, 2}, tb) && tb < best) {
                    best = tb;
                    id = 2;
                    hit = {origin.x + dir.x * tb, origin.y + dir.y * tb, origin.z + dir.z * tb};
                }
                if (best < 1.0e6f) {
                    const float dist = sqrtf(hit.x * hit.x + hit.z * hit.z);
                    const float a = DiscAlpha(dist, kInner, kOuter);
                    out = Mix(room, Shade(hit, id), a);
                }
            }
            px[(size_t)y * kW + x] = out;
        }
    }
}

}  // namespace

int main() {
    const V3 player{0, 0, 0};
    const V3 anchor{0, 0, 0};
    const V3 head{0, 0.45f, 0.35f};
    const float ipd = 0.064f;
    const V3 leftEye = EyeWorldFromStage(player, {head.x - ipd * 0.5f, head.y, head.z}, anchor, 0, kMetersPerUnit);
    const V3 rightEye = EyeWorldFromStage(player, {head.x + ipd * 0.5f, head.y, head.z}, anchor, 0, kMetersPerUnit);
    const V3 mid{(leftEye.x + rightEye.x) * 0.5f, (leftEye.y + rightEye.y) * 0.5f, (leftEye.z + rightEye.z) * 0.5f};
    const V3 forward{-mid.x, -mid.y, -mid.z};
    const V3 up{0, 1, 0};

    float leftV[16], rightV[16], proj[16];
    if (!BuildView(leftEye, forward, up, leftV) || !BuildView(rightEye, forward, up, rightV)) {
        std::printf("view failed\n");
        return 1;
    }
    const float tanY = tanf(30.0f * 3.14159265f / 180.0f);
    const float aspect = (float)kW / (float)kH;
    BuildEyeProj({-tanY * aspect, tanY * aspect, -tanY, tanY, 0.05f}, proj);

    std::vector<Rgb> left((size_t)kW * kH), right((size_t)kW * kH), both((size_t)kW * 2 * kH);
    Render(leftV, proj, left.data());
    Render(rightV, proj, right.data());
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            both[(size_t)y * kW * 2 + x] = left[(size_t)y * kW + x];
            both[(size_t)y * kW * 2 + kW + x] = right[(size_t)y * kW + x];
        }
    }
    if (!WriteBmp("vr/preview/left.bmp", left.data(), kW, kH) ||
        !WriteBmp("vr/preview/right.bmp", right.data(), kW, kH) ||
        !WriteBmp("vr/preview/both.bmp", both.data(), kW * 2, kH)) {
        std::printf("write failed\n");
        return 1;
    }
    std::printf("left eye world %.2f %.2f %.2f\n", leftEye.x, leftEye.y, leftEye.z);
    std::printf("right eye world %.2f %.2f %.2f\n", rightEye.x, rightEye.y, rightEye.z);
    std::printf("wrote vr/preview/left.bmp right.bmp both.bmp\n");
    return 0;
}
