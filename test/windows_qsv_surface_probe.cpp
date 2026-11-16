// Read-only probe of the installed Intel VPL driver's advertised surface interop.
// An advertised surface does not prove that a particular import is zero-copy.
#define ONEVPL_EXPERIMENTAL
#include "qsv/qsv_vpl.h"
#include "qsv/qsv_encoder.h"
#include "meteor_d3d11_device.h"
#include <vpl/mfxmemory.h>

static const char *surface_name(mfxSurfaceType type){
    switch(type){
    case MFX_SURFACE_TYPE_D3D11_TEX2D:return "d3d11";
    case MFX_SURFACE_TYPE_D3D12_TEX2D:return "d3d12";
    case MFX_SURFACE_TYPE_VULKAN_IMG2D:return "vulkan";
    case MFX_SURFACE_TYPE_OPENCL_IMG2D:return "opencl";
    case MFX_SURFACE_TYPE_VAAPI:return "vaapi";
    default:return "unknown";
    }
}
int main(int argc,char **argv){
    try{
        auto device=meteor_d3d11_on(nullptr,0x8086);
        if(!device)throw std::runtime_error("Intel adapter unavailable");
        Vpl vpl;vpl.match_device(device.Get());vpl.filter("mfxImplDescription.Impl",MFX_IMPL_TYPE_HARDWARE);
        unsigned implementations=0;
        for(mfxU32 i=0;;++i){
            mfxHDL raw=nullptr;
            auto status=vpl.MFXEnumImplementations_p(vpl.loader,i,MFX_IMPLCAPS_IMPLDESCSTRUCTURE,&raw);
            if(status==MFX_ERR_NOT_FOUND)break;
            vpl_check(status,"Enumerate hardware implementation");
            const auto *description=static_cast<mfxImplDescription*>(raw);
            std::printf("implementation=%u api=%u.%u acceleration=%u\n",i,
                unsigned(description->ApiVersion.Major),unsigned(description->ApiVersion.Minor),unsigned(description->AccelerationMode));
            vpl.MFXDispReleaseImplDescription_p(vpl.loader,raw);++implementations;
            raw=nullptr;
            status=vpl.MFXEnumImplementations_p(vpl.loader,i,MFX_IMPLCAPS_SURFACE_TYPES,&raw);
            if(status<0){std::printf("surface_capabilities_status=%d\n",int(status));continue;}
            const auto *surfaces=static_cast<mfxSurfaceTypesSupported*>(raw);
            for(unsigned s=0;s<surfaces->NumSurfaceTypes;++s){
                const auto &surface=surfaces->SurfaceTypes[s];
                for(unsigned c=0;c<surface.NumSurfaceComponents;++c){
                    const auto &component=surface.SurfaceComponents[c];
                    if(component.SurfaceComponent==MFX_SURFACE_COMPONENT_ENCODE)
                        std::printf("encode_surface=%s import_shared=%u import_copy=%u flags=0x%x\n",
                            surface_name(surface.SurfaceType),unsigned(bool(component.SurfaceFlags&MFX_SURFACE_FLAG_IMPORT_SHARED)),
                            unsigned(bool(component.SurfaceFlags&MFX_SURFACE_FLAG_IMPORT_COPY)),unsigned(component.SurfaceFlags));
                }
            }
            vpl.MFXDispReleaseImplDescription_p(vpl.loader,raw);
        }
        if(argc==2 && std::strcmp(argv[1],"--imports")==0){
            // Deterministic NV12 input; check actual import decisions for texture
            // allocation flags independently of a game or compositor.
            const unsigned bindings[]={D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,
                D3D11_BIND_RENDER_TARGET|D3D11_BIND_VIDEO_ENCODER,D3D11_BIND_RENDER_TARGET|D3D11_BIND_DECODER,
                D3D11_BIND_RENDER_TARGET};
            std::vector<uint8_t> pixels(256*256*3/2,128);
            for(auto binding:bindings)for(unsigned misc:{0u,unsigned(D3D11_RESOURCE_MISC_SHARED),
                    unsigned(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE)}){
                D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=256;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
                desc.Format=DXGI_FORMAT_NV12;desc.BindFlags=binding;desc.MiscFlags=misc;
                D3D11_SUBRESOURCE_DATA data{pixels.data(),256,256*256*3/2};
                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                auto hr=device->CreateTexture2D(&desc,&data,&texture);
                std::printf("import_trial bind=0x%x misc=0x%x create=0x%08lx\n",binding,misc,static_cast<unsigned long>(hr));std::fflush(stdout);
                if(FAILED(hr))continue;
                ArcQsvConfig config{sizeof(config),256,256,90,1,8000,1,1,0,90,1,13};
                void *encoder=arc_qsv_create(&config,device.Get());
                if(!encoder){std::printf("encoder_error=%s\n",arc_qsv_last_error());continue;}
                Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
                D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT,0};Microsoft::WRL::ComPtr<ID3D11Query> query;
                if(FAILED(device->CreateQuery(&query_desc,&query))){arc_qsv_destroy(encoder);throw std::runtime_error("Create initialization fence");}
                context->End(query.Get());context->Flush();
                unsigned waits=0;while(context->GetData(query.Get(),nullptr,0,0)==S_FALSE && ++waits<5000)Sleep(1);
                if(waits>=5000){arc_qsv_destroy(encoder);throw std::runtime_error("Input initialization timed out");}
                ArcQsvPacket packet{};const int result=arc_qsv_encode_texture(encoder,texture.Get(),0,1,&packet);
                std::printf("import_encode result=%d bytes=%u error=%s\n",result,packet.size,result<0?arc_qsv_last_error():"");
                arc_qsv_destroy(encoder);
            }
        }else if(argc!=1)throw std::runtime_error("usage: meteor-qsv-surface-probe [--imports]");
        return implementations?0:1;
    }catch(const std::exception &e){std::fprintf(stderr,"Surface capability probe failed: %s\n",e.what());return 1;}
}
