// Hardware smoke test of the exact GPU conversion/import path used by PCVR.
// The moving RGBA/BGRA input is generated on the CPU only for this test fixture.
#include "meteor_qsv_encoder.h"
#include <fstream>
#include <cstdlib>
int main(int argc,char **argv) {
    try {
        if(argc!=7) throw std::runtime_error("usage: meteor-qsv-encode-test <h264|h265> <rgba|bgra> <width> <height> <frames> <output>");
        if(std::strcmp(argv[1],"h264") && std::strcmp(argv[1],"h265")) throw std::runtime_error("Invalid codec");
        if(std::strcmp(argv[2],"rgba") && std::strcmp(argv[2],"bgra")) throw std::runtime_error("Invalid pixel format");
        const unsigned w=std::stoul(argv[3]),h=std::stoul(argv[4]),frames=std::stoul(argv[5]);
        if(w<16 || h<16 || w>8192 || h>8192 || !frames || frames>10000) throw std::runtime_error("Invalid test dimensions/frame count");
        auto device=meteor_d3d11_on(nullptr,0x8086);
        if(!device) throw std::runtime_error("Intel D3D11 device unavailable");
        ArcQsvCapabilities caps{};
        if(arc_qsv_capabilities(device.Get(),&caps)!=0) throw std::runtime_error(arc_qsv_last_error());
        std::printf("QSV_CAPABILITIES codecs=%u max_frame=%ux%u\n",caps.codecs,caps.max_width,caps.max_height);
        const bool bgra=!std::strcmp(argv[2],"bgra");
        D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        gpu_check(device->CreateTexture2D(&desc,nullptr,&texture),"Create test input");
        MeteorQsvEncoder encoder;encoder.attach(device.Get(),texture.Get(),w,h,desc.Format);
        if(!encoder.start(!std::strcmp(argv[1],"h265"),72,32000000,true)) throw std::runtime_error(encoder.error);
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        std::vector<uint8_t> pixels(size_t(w)*h*4);
        std::ofstream output(argv[6],std::ios::binary);
        if(!output) throw std::runtime_error("Cannot create test output");
        for(unsigned frame=0;frame<frames;++frame) {
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                auto *p=pixels.data()+(size_t(y)*w+x)*4;
                const unsigned stripe=(x*3/w+frame/12)%3;
                p[bgra?2:0]=stripe==0?220:20;p[1]=stripe==1?220:20;p[bgra?0:2]=stripe==2?220:20;p[3]=255;
            }
            context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),w*4,0);
            if(!encoder.encode(frame==0 || frame==frames/2,frame,[&](const uint8_t *data,uint32_t length){
                output.write(reinterpret_cast<const char*>(data),length);return bool(output);
            })) throw std::runtime_error(encoder.error);
        }
        if(encoder.set_bitrate(20000000)) throw std::runtime_error("Unsupported live bitrate change accepted");
        std::printf("PASS QSV frames=%u dimensions=%ux%u codec=%s order=%s\n",frames,w,h,argv[1],argv[2]);
        return 0;
    }catch(const std::exception &e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
