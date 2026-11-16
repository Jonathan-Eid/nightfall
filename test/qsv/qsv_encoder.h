// Copyright (c) 2026 Jonathan Eid. MIT License; see LICENSE in this directory.
#pragma once
#include <stdint.h>
#ifdef ARC_QSV_BUILD
#define ARC_API __declspec(dllexport)
#else
#define ARC_API __declspec(dllimport)
#endif
#ifdef __cplusplus
extern "C" {
#endif
// Private C ABI v3 (derived from quest-link-intel-arc v2). codec: 1=H.264, 2=HEVC. input_mode: 0=CPU NV12, 1=D3D11 NV12 texture.
typedef struct ArcQsvConfig {
    uint32_t struct_size, width, height, fps_num, fps_den, bitrate_kbps;
    uint32_t codec, input_mode, expected_device_id, gop_frames, slice_count;
    uint32_t transfer_characteristics; // H.273: 1=BT.709, 13=sRGB (0 defaults to 1).
} ArcQsvConfig;
typedef struct ArcQsvPacket {
    const uint8_t* data; // Owned by encoder; valid until next encode/flush/destroy.
    uint32_t size, frame_type;
    uint64_t timestamp_90k;
} ArcQsvPacket;
typedef struct ArcQsvGpuInfo { uint32_t vendor_id,device_id,api_major,api_minor; char name[256]; } ArcQsvGpuInfo;
typedef struct ArcQsvCapabilities {uint32_t codecs,max_width,max_height;} ArcQsvCapabilities;
// Probe this D3D11 adapter. Bits 1/2 mean H.264/HEVC with NV12 texture input.
ARC_API int arc_qsv_capabilities(void* d3d11_device,ArcQsvCapabilities* capabilities);
// Optional ID3D11Device* is retained until encoder destruction; pass nullptr to let VPL create it.
ARC_API void* arc_qsv_create(const ArcQsvConfig* config, void* d3d11_device);
ARC_API void arc_qsv_destroy(void* encoder);
ARC_API int arc_qsv_gpu_info(void* encoder, ArcQsvGpuInfo* info);
// Return 0 for packet, 1 for no packet yet/end of flush, negative for error. One caller per instance.
ARC_API int arc_qsv_encode_nv12(void* encoder, const uint8_t* y, uint32_t y_pitch, const uint8_t* uv, uint32_t uv_pitch, uint64_t timestamp_90k, int force_idr, ArcQsvPacket* packet);
// texture must be ID3D11Texture2D*, NV12, same device and configured visible dimensions; complete GPU writes first.
ARC_API int arc_qsv_encode_texture(void* encoder, void* texture, uint64_t timestamp_90k, int force_idr, ArcQsvPacket* packet);
ARC_API int arc_qsv_flush(void* encoder, ArcQsvPacket* packet);
ARC_API const char* arc_qsv_last_error(void);
#ifdef __cplusplus
}
#endif
