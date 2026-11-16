#pragma once
#include "meteor_amf_encoder.h"
#include "meteor_d3d11_bridge.h"
struct MeteorAmfBridgeEncoder : MeteorAmfEncoder {
    void attach(ID3D11Device *device,ID3D11Texture2D *texture,uint32_t w,uint32_t h) {
        MeteorAmfEncoder::attach(device,texture,w,h,amf::AMF_SURFACE_RGBA);
    }
};
static void amf_bridge_loop(meteor_stereo_encode_v1 request) {
    meteor_d3d11_bridge_loop<MeteorAmfBridgeEncoder>(request,"amf");
}
