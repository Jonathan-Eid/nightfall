// Intel oneVPL hardware encoding. Conversion, NV12 import and encoding stay on the
// producer's D3D11 device; CPU access is only to the compressed bitstream.
#pragma once
#include "meteor_d3d11_device.h"
#include "qsv/qsv_encoder.h"
#include "qsv/gpu_nv12_converter.h"
#include <d3d10.h>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

struct MeteorQsvEncoder {
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    std::unique_ptr<GpuNv12Converter> converter;
    void *encoder=nullptr;
    uint32_t width=0,height=0,fps=0;
    DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM;
    std::string error;
    bool fail(const std::string &message) {error=message;return false;}
    void attach(ID3D11Device *device,ID3D11Texture2D *frame,uint32_t w,uint32_t h,
                DXGI_FORMAT input_format=DXGI_FORMAT_R8G8B8A8_UNORM) {
        d3d=device;texture=frame;width=w;height=h;format=input_format;
    }
    void wait_gpu() {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;d3d->GetImmediateContext(&context);
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        gpu_check(d3d->CreateQuery(&desc,&query),"Create QSV producer completion query");
        context->End(query.Get());context->Flush();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(;;) {
            const auto result=context->GetData(query.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(result==S_OK) return;
            gpu_check(result,"Wait for QSV input");
            if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error("QSV producer completion timed out");
            std::this_thread::yield();
        }
    }
    bool start(bool hevc,uint32_t rate,uint32_t bitrate,bool srgb) {
        try {
            if(!d3d || !texture || !width || !height || width%16 || height%16 || !rate)
                return fail("QSV requires a D3D11 texture with dimensions aligned to 16 pixels");
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            if(desc.Width!=width || desc.Height!=height || desc.Format!=format || desc.ArraySize!=1 || desc.SampleDesc.Count!=1)
                return fail("QSV input texture does not match the negotiated frame");
            Microsoft::WRL::ComPtr<ID3D11Device> producer;texture->GetDevice(&producer);
            if(producer.Get()!=d3d.Get()) return fail("QSV texture belongs to another device");
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;d3d->GetImmediateContext(&context);
            Microsoft::WRL::ComPtr<ID3D10Multithread> multi;
            if(SUCCEEDED(context.As(&multi))) multi->SetMultithreadProtected(TRUE);
            wait_gpu();fps=rate;
            ArcQsvConfig config{sizeof(config),width,height,rate,1,(bitrate+999)/1000,hevc?2u:1u,1,0,rate,1,srgb?13u:1u};
            encoder=arc_qsv_create(&config,d3d.Get());
            if(!encoder) return fail(arc_qsv_last_error());
            converter=std::make_unique<GpuNv12Converter>(d3d.Get(),width,height,format,rate,1);
            return true;
        }catch(const std::exception &e){destroy();return fail(e.what());}
    }
    bool set_bitrate(uint32_t) {return fail("Live bitrate changes are unsupported by QSV; reconnect with the new bitrate");}
    template<class Deliver> bool encode(bool idr,uint64_t frame,Deliver deliver) {
        try {
            if(!encoder || !converter) return fail("QSV is not initialized");
            auto *nv12=converter->convert(texture.Get(),input_view);
            // Keep the completion wait that worked in the Link tests. A blind Flush
            // reduced native callback time but made live tail timing/GPU use worse.
            wait_gpu();ArcQsvPacket packet{};
            const int result=arc_qsv_encode_texture(encoder,nv12,frame*90000/fps,idr,&packet);
            if(result!=0) return fail(result<0?arc_qsv_last_error():"QSV buffered a frame in the synchronous path");
            if(!packet.size)return fail("QSV returned an empty packet");
            return deliver(packet.data,packet.size);
        }catch(const std::exception &e){return fail(e.what());}
    }
    void destroy() {
        if(encoder){arc_qsv_destroy(encoder);encoder=nullptr;}
        input_view.Reset();converter.reset();texture.Reset();d3d.Reset();
    }
    ~MeteorQsvEncoder(){destroy();}
};
