#pragma once

// Row-major matrices, row vectors: p_out = p_in * M.
// Same multiply as plugin/3dcam.cpp Vec4Mat.
//
// View space is +X right, +Y up, -Z forward. A point in front of the eye has
// view z < 0, and BuildEyeProj gives it a positive clip w.
// Projection matches the infinite reverse-Z form BuildProj writes:
// proj[10] = 0, proj[11] = -1, proj[14] = near.

struct V3 {
    float x, y, z;
};

// OpenXR tangent half-angles. Left and down are negative.
struct EyeFov {
    float tanLeft;
    float tanRight;
    float tanDown;
    float tanUp;
    float nearZ;
};

void Vec4Mat(const float v[4], const float m[16], float o[4]);
bool Invert4(const float m[16], float out[16]);

// forward is the look direction. up is re-orthogonalized. Either may be non-unit.
bool BuildView(V3 eye, V3 forward, V3 up, float view[16]);

void BuildEyeProj(const EyeFov& fov, float proj[16]);

// False when the point is not in front of the eye (clip w <= 0).
bool Project(const float view[16], const float proj[16], V3 world, float& ndcX, float& ndcY, float& ndcZ);

// World-space ray through an NDC point. Origin is the eye.
bool EyeRay(const float view[16], const float proj[16], float ndcX, float ndcY, V3& origin, V3& dir);

// Stage pose of one eye to D2R world. The anchor is the disc center on the stage.
// Yaw 0 keeps stage axes aligned with world axes.
V3 EyeWorldFromStage(V3 playerGround, V3 eyeStage, V3 anchorStage, float yawRadians, float metersPerUnit);

// 1 at and inside inner, 0 at and outside outer, smoothstep between.
float DiscAlpha(float distance, float inner, float outer);

// NDC ray against the plane y = groundY. Alpha is the XZ disc around the player.
// False when the ray does not hit that plane in front of the eye.
bool DiscAlphaAt(const float view[16], const float proj[16], float ndcX, float ndcY, float groundY,
                 float playerX, float playerZ, float inner, float outer, float& alpha);

// Meters on the stage to world units. Yaw is about world Y.
// Yaw 0 keeps the axes aligned. Positive yaw takes stage +X toward world +Z.
V3 StageDeltaToWorld(V3 deltaStageMeters, float yawRadians, float metersPerUnit);
