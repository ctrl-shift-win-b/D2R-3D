#pragma once

// One-writer mapping shared by the game-side recorder and vrhost.
// The writer stores seq as odd while the fields are in flux and even after
// they are consistent. A reader retries while seq is odd or changes mid-copy.

#include <stdint.h>

static const uint32_t kBridgeMagic = 0x33523244u;  // 'D2R3'
static const uint32_t kBridgeVersion = 1u;
// Local\D2R3D.VR

struct BridgeEye {
    float view[16];
    float proj[16];
    float position[3];
    float pad;
};

struct FrameBridge {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t recordingEye;  // 0 = left, 1 = right
    BridgeEye eyes[2];
    float player[3];
    float groundY;
    float innerRadius;
    float outerRadius;
    float anchor[3];
    float anchorYaw;
    float metersPerUnit;
    uint32_t mode;  // 0 off, 1 table, 2 floor
    uint32_t width;
    uint32_t height;
    uint32_t format;
};
