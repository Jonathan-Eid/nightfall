// Copyright (c) 2026 Jonathan Eid. MIT License; see LICENSE in this directory.
#pragma once
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstring>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#include <cstdio>
#include <vector>
#include <string>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <algorithm>

static void vpl_check(mfxStatus s, const char* name) {
    if(s<0)std::fprintf(stderr,"QSV %s: %d\n",name,int(s));
    if (s < 0 || s == MFX_WRN_PARTIAL_ACCELERATION) throw std::runtime_error(name);
}
struct Vpl {
    HMODULE dll{}; mfxLoader loader{}; mfxSession session{}; bool initialized{}; ID3D11Device* external_device{};
#define API(n) decltype(&n) n##_p{}
    API(MFXLoad); API(MFXUnload); API(MFXCreateConfig); API(MFXSetConfigFilterProperty);
    API(MFXCreateSession); API(MFXClose); API(MFXQueryIMPL); API(MFXQueryVersion);
    API(MFXEnumImplementations); API(MFXDispReleaseImplDescription);
    API(MFXVideoENCODE_Query); API(MFXVideoENCODE_Init); API(MFXVideoENCODE_GetVideoParam);
    API(MFXVideoENCODE_EncodeFrameAsync); API(MFXVideoCORE_SyncOperation);
    API(MFXVideoENCODE_Close); API(MFXVideoCORE_GetHandle); API(MFXVideoCORE_SetHandle);
#undef API
    Vpl() {
        try {
#ifdef METEOR_QSV_STATIC_DISPATCHER
#define LOAD(n) n##_p=&n
#else
        dll=LoadLibraryExW(L"libvpl.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!dll) throw std::runtime_error("Cannot load system libvpl.dll");
#define LOAD(n) n##_p=reinterpret_cast<decltype(n##_p)>(GetProcAddress(dll,#n)); if(!n##_p) throw std::runtime_error(#n)
#endif
        LOAD(MFXLoad); LOAD(MFXUnload); LOAD(MFXCreateConfig); LOAD(MFXSetConfigFilterProperty);
        LOAD(MFXCreateSession); LOAD(MFXClose); LOAD(MFXQueryIMPL); LOAD(MFXQueryVersion);
        LOAD(MFXEnumImplementations); LOAD(MFXDispReleaseImplDescription);
        LOAD(MFXVideoENCODE_Query); LOAD(MFXVideoENCODE_Init); LOAD(MFXVideoENCODE_GetVideoParam);
        LOAD(MFXVideoENCODE_EncodeFrameAsync); LOAD(MFXVideoCORE_SyncOperation);
        LOAD(MFXVideoENCODE_Close); LOAD(MFXVideoCORE_GetHandle); LOAD(MFXVideoCORE_SetHandle);
#undef LOAD
        loader=MFXLoad_p(); if(!loader) throw std::runtime_error("MFXLoad");
        }catch(...){if(loader && MFXUnload_p)MFXUnload_p(loader);if(dll)FreeLibrary(dll);loader=nullptr;dll=nullptr;throw;}
    }
    void match_device(ID3D11Device* device) {
        IDXGIDevice* dx=nullptr; IDXGIAdapter* adapter=nullptr;
        HRESULT result=device->QueryInterface(IID_PPV_ARGS(&dx));
        if(FAILED(result)) throw std::runtime_error("QSV producer has no DXGI device");
        result=dx->GetAdapter(&adapter); dx->Release();
        if(FAILED(result)) throw std::runtime_error("QSV producer has no adapter");
        DXGI_ADAPTER_DESC desc{};result=adapter->GetDesc(&desc);adapter->Release();
        if(FAILED(result) || desc.VendorId!=0x8086) throw std::runtime_error("QSV requires the Intel producer adapter");
        mfxConfig config=MFXCreateConfig_p(loader);
        if(!config) throw std::runtime_error("QSV LUID configuration failed");
        mfxVariant value{};value.Type=MFX_VARIANT_TYPE_PTR;value.Data.Ptr=&desc.AdapterLuid;
        vpl_check(MFXSetConfigFilterProperty_p(config,reinterpret_cast<const mfxU8*>("mfxExtendedDeviceId.DeviceLUID"),value),"Match QSV adapter LUID");
        filter("mfxExtendedDeviceId.LUIDDeviceNodeMask",1);
    }
    ~Vpl(){if(initialized)MFXVideoENCODE_Close_p(session);if(session)MFXClose_p(session);if(external_device)external_device->Release();if(loader)MFXUnload_p(loader);if(dll)FreeLibrary(dll);}
    void filter(const char* key,mfxU32 value){
        mfxConfig cfg=MFXCreateConfig_p(loader);if(!cfg)throw std::runtime_error("MFXCreateConfig");
        mfxVariant v{};v.Type=MFX_VARIANT_TYPE_U32;v.Data.U32=value;
        vpl_check(MFXSetConfigFilterProperty_p(cfg,reinterpret_cast<const mfxU8*>(key),v),key);
    }

};
