// Intel oneVPL hardware encoding. Conversion, NV12 import and encoding stay on the
// producer's D3D11 device; CPU access is only to the compressed bitstream.
#pragma once
#include "meteor_d3d11_device.h"
#include "qsv/qsv_encoder.h"
#include "qsv/gpu_nv12_converter.h"
#include <d3d10.h>
#include <d3d11_4.h>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

struct MeteorQsvEncoder {
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    std::unique_ptr<GpuNv12Converter> converter;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> completion_context;
    Microsoft::WRL::ComPtr<ID3D11Fence> completion_fence;
    HANDLE completion_event=nullptr;
    uint64_t completion_value=0;
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
        if(!completion_fence){
            Microsoft::WRL::ComPtr<ID3D11Device5> device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;d3d->GetImmediateContext(&context);
            gpu_check(d3d.As(&device),"Get QSV completion fence device");
            gpu_check(context.As(&completion_context),"Get QSV completion fence context");
            gpu_check(device->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion_fence)),"Create QSV completion fence");
            completion_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!completion_event)throw std::runtime_error("Create QSV completion event failed");
        }
        const uint64_t value=++completion_value;
        gpu_check(completion_context->Signal(completion_fence.Get(),value),"Signal QSV input completion");
        completion_context->Flush();
        gpu_check(completion_fence->SetEventOnCompletion(value,completion_event),"Arm QSV input completion");
        if(WaitForSingleObject(completion_event,5000)!=WAIT_OBJECT_0)
            throw std::runtime_error("QSV producer completion wait failed or timed out");
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
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> timing_context;
            Microsoft::WRL::ComPtr<ID3D11Query> timing_disjoint,timing_start,timing_end;
            if(frame%72==0){
                d3d->GetImmediateContext(&timing_context);
                D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
                gpu_check(d3d->CreateQuery(&query,&timing_disjoint),"Create conversion timer");
                query.Query=D3D11_QUERY_TIMESTAMP;
                gpu_check(d3d->CreateQuery(&query,&timing_start),"Create conversion timestamp");
                gpu_check(d3d->CreateQuery(&query,&timing_end),"Create conversion timestamp");
                timing_context->Begin(timing_disjoint.Get());timing_context->End(timing_start.Get());
            }
            const auto convert_start=std::chrono::steady_clock::now();
            auto *nv12=converter->convert(texture.Get(),input_view);
            if(timing_context){timing_context->End(timing_end.Get());timing_context->End(timing_disjoint.Get());}
            const auto convert_end=std::chrono::steady_clock::now();
            // Import requires completed external GPU work. Keep that guarantee
            // without spinning a CPU core while the integrated GPU is rendering.
            wait_gpu();const auto gpu_ready=std::chrono::steady_clock::now();ArcQsvPacket packet{};
            const int result=arc_qsv_encode_texture(encoder,nv12,frame*90000/fps,idr,&packet);
            if(frame%72==0){
                const auto encoded=std::chrono::steady_clock::now();
                const auto us=[](auto a,auto b){return (long long)std::chrono::duration_cast<std::chrono::microseconds>(b-a).count();};
                std::fprintf(stderr,"METEOR_QSV_STAGES frame=%llu convert_cpu_us=%lld gpu_wait_us=%lld encode_us=%lld\n",
                    (unsigned long long)frame,us(convert_start,convert_end),us(convert_end,gpu_ready),us(gpu_ready,encoded));std::fflush(stderr);
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};UINT64 begin=0,end=0;
                if(timing_context->GetData(timing_disjoint.Get(),&clock,sizeof(clock),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK &&
                   !clock.Disjoint && clock.Frequency &&
                   timing_context->GetData(timing_start.Get(),&begin,sizeof(begin),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK &&
                   timing_context->GetData(timing_end.Get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK){
                    std::fprintf(stderr,"METEOR_QSV_GPU_CONVERT frame=%llu gpu_us=%llu\n",(unsigned long long)frame,
                        (unsigned long long)((end-begin)*1000000/clock.Frequency));std::fflush(stderr);
                }
            }
            if(result!=0) return fail(result<0?arc_qsv_last_error():"QSV buffered a frame in the synchronous path");
            if(!packet.size)return fail("QSV returned an empty packet");
            return deliver(packet.data,packet.size);
        }catch(const std::exception &e){return fail(e.what());}
    }
    void destroy() {
        if(encoder){arc_qsv_destroy(encoder);encoder=nullptr;}
        completion_fence.Reset();completion_context.Reset();completion_value=0;
        if(completion_event){CloseHandle(completion_event);completion_event=nullptr;}
        input_view.Reset();converter.reset();texture.Reset();d3d.Reset();
    }
    ~MeteorQsvEncoder(){destroy();}
};
