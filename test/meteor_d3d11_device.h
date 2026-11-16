#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstring>
#include <cstdint>

// Match the producer's adapter exactly; never silently copy between GPUs.
static Microsoft::WRL::ComPtr<ID3D11Device> meteor_d3d11_on(const uint8_t *luid,unsigned vendor=0x1002) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for(UINT i=0;;++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if(FAILED(adapter->GetDesc1(&desc)) || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if(luid ? std::memcmp(&desc.AdapterLuid,luid,sizeof(LUID))!=0 : desc.VendorId!=vendor) continue;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        if(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,2,D3D11_SDK_VERSION,&device,nullptr,nullptr))) return device;
        return nullptr;
    }
    return nullptr;
}
