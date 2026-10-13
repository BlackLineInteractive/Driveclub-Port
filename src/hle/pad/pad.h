#pragma once

#include "common/types.h"

namespace Hle::Pad {

struct OrbisPadAnalogStick {
    u8 x = 128;
    u8 y = 128;
};

struct OrbisPadAnalogButtons {
    u8 l2 = 0;
    u8 r2 = 0;
    u8 padding[2] = {0};
};

struct OrbisPadData {
    u32 buttons = 0;
    OrbisPadAnalogStick leftStick;
    OrbisPadAnalogStick rightStick;
    OrbisPadAnalogButtons analogButtons;
    float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float acceleration[3] = {0.0f, 0.0f, 9.8f};
    float angularVelocity[3] = {0.0f, 0.0f, 0.0f};
    struct {
        u8 touchNum = 0;
        u8 reserve[3] = {0};
        u32 time_held = 0;
        struct {
            u16 x = 0;
            u16 y = 0;
            u8 id = 0;
            u8 reserve[3] = {0};
        } touch[2];
    } touchData;
    bool connected = true;
    u64 timestamp = 0;
    u8 extension[16] = {0};
    u8 connectedCount = 1;
    u8 reserve[2] = {0};
    u8 deviceUniqueDataLen = 0;
    u8 deviceUniqueData[12] = {0};
};

struct OrbisPadControllerInformation {
    struct {
        float pixelDensity = 1.0f;
        u16 resX = 1920;
        u16 resY = 942;
    } touchPadInfo;
    struct {
        u8 deadZoneLeft = 0;
        u8 deadZoneRight = 0;
    } stickInfo;
    u8 connectionType = 0;
    u8 connectedCount = 1;
    bool connected = true;
    u8 deviceClass = 0;
    u8 reserve[8] = {0};
};

void RegisterPad();

} // namespace Hle::Pad
