// Copyright (c) 2026 Jonathan Eid. MIT License; see LICENSE in this directory.
#define ARC_QSV_BUILD
#define ONEVPL_EXPERIMENTAL
#include "qsv_encoder.h"
#include "qsv_vpl.h"
#include <vpl/mfxmemory.h>
#include <cstring>
#include <memory>
#include <limits>
#include <d3d10.h>

static thread_local std::string last_error;
static void require(mfxStatus s,const char* message){if(s<0||s==MFX_WRN_PARTIAL_ACCELERATION)throw std::runtime_error(std::string(message)+": "+std::to_string(s));}
struct Encoder {
    Vpl vpl; ArcQsvConfig cfg{}; ArcQsvGpuInfo gpu{}; mfxFrameInfo frame{};
    mfxFrameSurface1 surface{}; mfxMemoryInterface* memory{}; ID3D11Device* device{};
    std::vector<uint8_t> pixels, bits; uint32_t frame_order{};
    Encoder(const ArcQsvConfig& c,ID3D11Device* ext):cfg(c){
        if(c.struct_size!=sizeof(c)||c.width<16||c.height<16||c.width>8192||c.height>8192||(c.width&1)||(c.height&1)||!c.fps_num||!c.fps_den||!c.bitrate_kbps||c.bitrate_kbps>1000000||c.slice_count>256||(c.codec!=1&&c.codec!=2)||c.input_mode>1)throw std::runtime_error("Invalid encoder configuration");
        if(ext){
            ID3D11DeviceContext* context=nullptr;ext->GetImmediateContext(&context);
            ID3D10Multithread* multi=nullptr;const HRESULT hr=context->QueryInterface(IID_PPV_ARGS(&multi));context->Release();
            if(FAILED(hr))throw std::runtime_error("QSV requires D3D11 multithread protection");
            multi->SetMultithreadProtected(TRUE);multi->Release();
        }
        vpl.filter("mfxImplDescription.Impl",MFX_IMPL_TYPE_HARDWARE);
        vpl.filter("mfxImplDescription.AccelerationMode",MFX_ACCEL_MODE_VIA_D3D11);
        vpl.filter("mfxImplDescription.mfxEncoderDescription.encoder.CodecID",c.codec==2?MFX_CODEC_HEVC:MFX_CODEC_AVC);
        require(vpl.MFXCreateSession_p(vpl.loader,0,&vpl.session),"Hardware session");
        if(ext){ext->AddRef();vpl.external_device=ext;require(vpl.MFXVideoCORE_SetHandle_p(vpl.session,MFX_HANDLE_D3D11_DEVICE,ext),"Bind D3D11 device");}
        mfxVideoParam p{};p.AsyncDepth=1;p.IOPattern=c.input_mode?MFX_IOPATTERN_IN_VIDEO_MEMORY:MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        p.mfx.CodecId=c.codec==2?MFX_CODEC_HEVC:MFX_CODEC_AVC;p.mfx.CodecProfile=mfxU16(c.codec==2?MFX_PROFILE_HEVC_MAIN:MFX_PROFILE_AVC_HIGH);
        p.mfx.TargetUsage=MFX_TARGETUSAGE_BEST_SPEED;p.mfx.BRCParamMultiplier=mfxU16((c.bitrate_kbps+65534)/65535);p.mfx.TargetKbps=mfxU16((c.bitrate_kbps+p.mfx.BRCParamMultiplier-1)/p.mfx.BRCParamMultiplier);p.mfx.RateControlMethod=MFX_RATECONTROL_CBR;p.mfx.NumSlice=mfxU16(c.slice_count?c.slice_count:1);
        p.mfx.GopRefDist=1;p.mfx.NumRefFrame=1;p.mfx.GopPicSize=mfxU16(std::min(c.gop_frames?c.gop_frames:72u,65535u));p.mfx.LowPower=MFX_CODINGOPTION_ON;
        auto& f=p.mfx.FrameInfo;f.FourCC=MFX_FOURCC_NV12;f.ChromaFormat=MFX_CHROMAFORMAT_YUV420;f.PicStruct=MFX_PICSTRUCT_PROGRESSIVE;
        f.Width=mfxU16((c.width+15)&~15u);f.Height=mfxU16((c.height+15)&~15u);f.CropW=mfxU16(c.width);f.CropH=mfxU16(c.height);f.FrameRateExtN=c.fps_num;f.FrameRateExtD=c.fps_den;
        mfxExtCodingOption2 o2{};o2.Header.BufferId=MFX_EXTBUFF_CODING_OPTION2;o2.Header.BufferSz=sizeof(o2);o2.RepeatPPS=MFX_CODINGOPTION_ON;o2.BRefType=MFX_B_REF_OFF;
        mfxExtCodingOption3 o3{};o3.Header.BufferId=MFX_EXTBUFF_CODING_OPTION3;o3.Header.BufferSz=sizeof(o3);o3.ScenarioInfo=MFX_SCENARIO_REMOTE_GAMING;
        if(c.codec==2)o3.GPB=MFX_CODINGOPTION_OFF;
        // Match the GPU converter's limited-range BT.709 output so decoders
        // do not guess a different YUV matrix from resolution or defaults.
        mfxExtVideoSignalInfo signal{};signal.Header.BufferId=MFX_EXTBUFF_VIDEO_SIGNAL_INFO;signal.Header.BufferSz=sizeof(signal);signal.VideoFormat=5;signal.VideoFullRange=0;signal.ColourDescriptionPresent=1;signal.ColourPrimaries=1;signal.TransferCharacteristics=mfxU16(c.transfer_characteristics?c.transfer_characteristics:1);signal.MatrixCoefficients=1;
        mfxExtBuffer* exts[]={reinterpret_cast<mfxExtBuffer*>(&o2),reinterpret_cast<mfxExtBuffer*>(&o3),reinterpret_cast<mfxExtBuffer*>(&signal)};p.ExtParam=exts;p.NumExtParam=3;
        mfxVideoParam queried=p;require(vpl.MFXVideoENCODE_Query_p(vpl.session,&p,&queried),"Query encoder");require(vpl.MFXVideoENCODE_Init_p(vpl.session,&queried),"Initialize encoder");vpl.initialized=true;
        require(vpl.MFXVideoENCODE_GetVideoParam_p(vpl.session,&queried),"Read encoder parameters");frame=queried.mfx.FrameInfo;
        if(queried.mfx.GopRefDist!=1||queried.AsyncDepth!=1)throw std::runtime_error("Runtime changed low-latency settings");
        mfxHDL h{};require(vpl.MFXVideoCORE_GetHandle_p(vpl.session,MFX_HANDLE_D3D11_DEVICE,&h),"Get D3D11 device");device=static_cast<ID3D11Device*>(h);
        if(ext && device!=ext)throw std::runtime_error("oneVPL did not bind the producer's D3D11 device");
        if(frame.CropW!=c.width || frame.CropH!=c.height)throw std::runtime_error("oneVPL changed the negotiated visible dimensions");
        IDXGIDevice* dx{};IDXGIAdapter* adapter{};if(!device||FAILED(device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dx))))throw std::runtime_error("Get DXGI device");
        HRESULT hr=dx->GetAdapter(&adapter);dx->Release();if(FAILED(hr))throw std::runtime_error("Get adapter");DXGI_ADAPTER_DESC desc{};hr=adapter->GetDesc(&desc);adapter->Release();if(FAILED(hr))throw std::runtime_error("Get adapter description");
        gpu.vendor_id=desc.VendorId;gpu.device_id=desc.DeviceId;WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,gpu.name,sizeof(gpu.name),nullptr,nullptr);
        mfxVersion version{};require(vpl.MFXQueryVersion_p(vpl.session,&version),"Query API version");gpu.api_major=version.Major;gpu.api_minor=version.Minor;
        if(gpu.vendor_id!=0x8086||(c.expected_device_id&&gpu.device_id!=c.expected_device_id))throw std::runtime_error("Unexpected hardware adapter");
        size_t required_bits=size_t(queried.mfx.BufferSizeInKB)*std::max<unsigned>(queried.mfx.BRCParamMultiplier,1)*1000;
        if(required_bits>256*1024*1024)throw std::runtime_error("Encoder requests excessive bitstream buffer");
        bits.resize(std::max({size_t(4*1024*1024),size_t(c.width)*c.height*2,required_bits}));
        if(c.input_mode){require(vpl.MFXVideoCORE_GetHandle_p(vpl.session,MFX_HANDLE_MEMORY_INTERFACE,reinterpret_cast<mfxHDL*>(&memory)),"Get texture-import interface");if(!memory||!memory->ImportFrameSurface)throw std::runtime_error("Texture import unavailable");}
        else{pixels.resize(size_t(frame.Width)*frame.Height*3/2,128);surface.Info=frame;surface.Data.Pitch=frame.Width;surface.Data.Y=pixels.data();surface.Data.UV=pixels.data()+size_t(frame.Width)*frame.Height;}
    }
    int submit(mfxFrameSurface1* input,bool idr,ArcQsvPacket* output){
        if(!output)throw std::runtime_error("Packet pointer is null");*output={};mfxBitstream bs{};bs.Data=bits.data();bs.MaxLength=mfxU32(bits.size());mfxSyncPoint sync{};mfxEncodeCtrl ctrl{};
        if(idr)ctrl.FrameType=MFX_FRAMETYPE_I|MFX_FRAMETYPE_IDR|MFX_FRAMETYPE_REF;
        mfxStatus status{};int retries=0;
        do{status=vpl.MFXVideoENCODE_EncodeFrameAsync_p(vpl.session,idr?&ctrl:nullptr,input,&bs,&sync);if(status==MFX_WRN_DEVICE_BUSY)std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(status==MFX_WRN_DEVICE_BUSY&&++retries<1000);
        if(status==MFX_ERR_MORE_DATA)return 1;require(status,"Encode frame");if(!sync)throw std::runtime_error("Encoder returned no sync point");
        require(vpl.MFXVideoCORE_SyncOperation_p(vpl.session,sync,10000),"Synchronize encoded frame");
        output->data=bs.Data+bs.DataOffset;output->size=bs.DataLength;output->timestamp_90k=bs.TimeStamp;output->frame_type=bs.FrameType;return 0;
    }
    int cpu(const uint8_t* y,uint32_t yp,const uint8_t* uv,uint32_t uvp,uint64_t ts,bool idr,ArcQsvPacket* packet){
        if(cfg.input_mode||!y||!uv||yp<cfg.width||uvp<cfg.width||!packet)throw std::runtime_error("Invalid CPU NV12 input");if(surface.Data.Locked)throw std::runtime_error("Previous input surface still in use");
        for(uint32_t row=0;row<cfg.height;++row)memcpy(surface.Data.Y+size_t(row)*frame.Width,y+size_t(row)*yp,cfg.width);
        for(uint32_t row=0;row<cfg.height/2;++row)memcpy(surface.Data.UV+size_t(row)*frame.Width,uv+size_t(row)*uvp,cfg.width);
        surface.Data.TimeStamp=ts;surface.Data.FrameOrder=frame_order++;return submit(&surface,idr,packet);
    }
    int texture(ID3D11Texture2D* tex,uint64_t ts,bool idr,ArcQsvPacket* packet){
        if(!cfg.input_mode||!tex||!packet)throw std::runtime_error("Invalid GPU texture input");D3D11_TEXTURE2D_DESC d{};tex->GetDesc(&d);
        if(d.Format!=DXGI_FORMAT_NV12||d.Width!=frame.Width||d.Height!=frame.Height||d.ArraySize!=1||d.SampleDesc.Count!=1)throw std::runtime_error("Expected one aligned NV12 texture");
        ID3D11Device* tex_device{};tex->GetDevice(&tex_device);const bool same=tex_device==device;tex_device->Release();if(!same)throw std::runtime_error("Texture belongs to another D3D11 device");
        mfxSurfaceD3D11Tex2D external{};external.SurfaceInterface.Header.SurfaceType=MFX_SURFACE_TYPE_D3D11_TEX2D;external.SurfaceInterface.Header.SurfaceFlags=MFX_SURFACE_FLAG_IMPORT_SHARED|MFX_SURFACE_FLAG_IMPORT_COPY;external.SurfaceInterface.Header.StructSize=sizeof(external);external.texture2D=tex;
        mfxFrameSurface1* imported{};require(memory->ImportFrameSurface(memory,MFX_SURFACE_COMPONENT_ENCODE,&external.SurfaceInterface.Header,&imported),"Import D3D11 texture");
        if(!imported||!imported->FrameInterface)throw std::runtime_error("Import returned no surface interface");
        struct Release{mfxFrameSurface1* s;~Release(){s->FrameInterface->Release(s);}} release{imported};
        imported->Data.TimeStamp=ts;imported->Data.FrameOrder=frame_order++;return submit(imported,idr,packet);
    }
};
extern "C" {
int arc_qsv_capabilities(void* raw,ArcQsvCapabilities* caps){
    try{
        if(!raw||!caps)throw std::runtime_error("Missing QSV capability probe device/output");
        *caps={};Vpl vpl;vpl.match_device(static_cast<ID3D11Device*>(raw));
        vpl.filter("mfxImplDescription.Impl",MFX_IMPL_TYPE_HARDWARE);
        vpl.filter("mfxImplDescription.AccelerationMode",MFX_ACCEL_MODE_VIA_D3D11);
        mfxHDL handle{};require(vpl.MFXEnumImplementations_p(vpl.loader,0,MFX_IMPLCAPS_IMPLDESCSTRUCTURE,&handle),"QSV implementation description");
        struct Release{Vpl& v;mfxHDL h;~Release(){v.MFXDispReleaseImplDescription_p(v.loader,h);}} release{vpl,handle};
        auto* description=static_cast<mfxImplDescription*>(handle);
        uint32_t widths[2]{},heights[2]{};
        for(unsigned i=0;i<description->Enc.NumCodecs;++i){
            const auto& codec=description->Enc.Codecs[i];
            int index=codec.CodecID==MFX_CODEC_AVC?0:codec.CodecID==MFX_CODEC_HEVC?1:-1;
            if(index<0)continue;
            for(unsigned p=0;p<codec.NumProfiles;++p){
                const auto& profile=codec.Profiles[p];
                if(profile.Profile!=(index?MFX_PROFILE_HEVC_MAIN:MFX_PROFILE_AVC_HIGH))continue;
                for(unsigned m=0;m<profile.NumMemTypes;++m){
                    const auto& mem=profile.MemDesc[m];
                    if(mem.MemHandleType!=MFX_RESOURCE_DX11_TEXTURE)continue;
                    for(unsigned f=0;f<mem.NumColorFormats;++f)if(mem.ColorFormats[f]==MFX_FOURCC_NV12){
                        widths[index]=std::max(widths[index],mem.Width.Max);heights[index]=std::max(heights[index],mem.Height.Max);
                    }
                }
            }
        }
        for(unsigned i=0;i<2;++i)if(widths[i]&&heights[i]){
            // Metadata alone does not prove that this driver supports low-latency
            // texture import. Initialize the exact path before advertising a codec.
            ArcQsvConfig config{sizeof(config),320,192,72,1,10000,i+1,1,0,72,1,1};
            caps->codecs|=1u<<i;
            caps->max_width=caps->max_width?std::min(caps->max_width,widths[i]):widths[i];
            caps->max_height=caps->max_height?std::min(caps->max_height,heights[i]):heights[i];
        }
        if(!caps->codecs)throw std::runtime_error("No hardware H.264/HEVC encoder with low-latency NV12 import");
        return 0;
    }catch(const std::exception& e){last_error=e.what();return -1;}
}
void* arc_qsv_create(const ArcQsvConfig* c,void* device){try{last_error.clear();if(!c)throw std::runtime_error("Null configuration");return new Encoder(*c,static_cast<ID3D11Device*>(device));}catch(const std::exception& e){last_error=e.what();return nullptr;}}
void arc_qsv_destroy(void* ptr){delete static_cast<Encoder*>(ptr);}
int arc_qsv_gpu_info(void* ptr,ArcQsvGpuInfo* out){if(!ptr||!out){last_error="Null encoder/info";return -1;}*out=static_cast<Encoder*>(ptr)->gpu;return 0;}
int arc_qsv_encode_nv12(void* ptr,const uint8_t* y,uint32_t yp,const uint8_t* uv,uint32_t uvp,uint64_t ts,int idr,ArcQsvPacket* packet){try{if(!ptr)throw std::runtime_error("Null encoder");return static_cast<Encoder*>(ptr)->cpu(y,yp,uv,uvp,ts,idr!=0,packet);}catch(const std::exception& e){last_error=e.what();return -1;}}
int arc_qsv_encode_texture(void* ptr,void* tex,uint64_t ts,int idr,ArcQsvPacket* packet){try{if(!ptr)throw std::runtime_error("Null encoder");return static_cast<Encoder*>(ptr)->texture(static_cast<ID3D11Texture2D*>(tex),ts,idr!=0,packet);}catch(const std::exception& e){last_error=e.what();return -1;}}
int arc_qsv_flush(void* ptr,ArcQsvPacket* packet){try{if(!ptr)throw std::runtime_error("Null encoder");return static_cast<Encoder*>(ptr)->submit(nullptr,false,packet);}catch(const std::exception& e){last_error=e.what();return -1;}}
const char* arc_qsv_last_error(){return last_error.c_str();}
}
