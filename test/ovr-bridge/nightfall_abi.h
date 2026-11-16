// Copyright (c) 2026 Jonathan Eid.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

// Local, versioned ABI between OVRNull and Nightfall's optional streaming DLL.
// All pointers are borrowed for a synchronous call; no graphics objects cross GPUs.
struct NightfallOvrConfigV1 {
    uint32_t size,version,eye_width,eye_height,refresh_hz;
    uint8_t adapter_luid[8];
    float optics[22]; // VIEW-relative eye position xyz, quaternion xyzw, FOV left/right/up/down.
};
struct NightfallOvrStateV1 {
    uint32_t size,head_valid,connected,reserved;
    float head[7],hands[2][14],controls[2][4]; // hands: grip then aim, each xyz/xyzw.
    uint32_t buttons[2],flags[2];
    uint64_t sample_ns;
};
typedef void* (*NightfallOvrOpenV1)(NightfallOvrConfigV1*);
typedef int (*NightfallOvrSampleV1)(void*,int64_t,NightfallOvrStateV1*);
typedef int (*NightfallOvrSubmitV1)(void*,void*,void*,uint64_t,uint64_t,const float*);
typedef int (*NightfallOvrHapticV1)(void*,uint32_t,float,int64_t);
typedef void (*NightfallOvrCloseV1)(void*);
