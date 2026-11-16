// Opt-in Nightfall transport backend for the standalone VirtualDesktop-OpenXR fork.
// No Meta binaries, injection, global runtime selection or Virtual Desktop service.
#include <windows.h>
#include <filesystem>
#include <cstdlib>
#include <mutex>
#include <fstream>
#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <thread>
#include "nightfall_abi.h"
#include "../meteor_qsv_encoder.h"
#include "../meteor_encoder_bitrate.h"
#include "../meteor_projection_margin.h"
#include "../steamvr-driver/transport.h"
#include "../meteor_windows_audio_transport.h"

namespace {
const char *env(const char *key){auto *s=std::getenv(key);return s?s:"";}
unsigned number(const char *key,unsigned fallback,unsigned lo,unsigned hi,unsigned step=1){
    const char *value=env(key);if(!*value)return fallback;
    char *end=nullptr;const auto n=std::strtoul(value,&end,10);
    if(*end || n<lo || n>hi || n%step)throw std::runtime_error(std::string("Invalid ")+key);
    return unsigned(n);
}
std::filesystem::path absolute_library(const char *key){
    auto path=std::filesystem::u8path(env(key));
    if(!path.is_absolute() || !std::filesystem::is_regular_file(path))throw std::runtime_error(std::string("Missing absolute ")+key);
    return path;
}
struct Session {
    HMODULE tracking=nullptr;void *client=nullptr;
    NightfallOvrConfigV1 config{};
    void *(*start)(const char*)=nullptr;void (*stop)(void*)=nullptr;
    int (*head)(void*,int64_t,float*,uint64_t*)=nullptr;
    int (*controller)(unsigned,int64_t,float*,float*,uint32_t*,uint32_t*,uint64_t*)=nullptr;
    int (*optics)(void*,float*)=nullptr;int (*haptic)(unsigned,float,int64_t)=nullptr;
    int (*connected)(void*)=nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> producer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input;
    std::unique_ptr<MeteorQsvEncoder> encoder;
    MeteorWindowsAudioTransport audio;
    std::mutex mutex;
    std::atomic<bool> delivered{false};
    // At 90 Hz, let the game render while the previous image is encoding.
    // Each slot owns both the GPU snapshot and its exact render-pose metadata.
    // Only one pending image is retained. Backpressure bounds GPU submissions
    // as well as CPU storage: replacing pending snapshots would still enqueue
    // discarded rendering/copy work and starve encoding on an integrated GPU.
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
        uint64_t id=0,display_ns=0;
        std::array<float,22> views{};
        bool busy=false;
    };
    std::array<Frame,2> frames;
    Frame *pending=nullptr;
    std::mutex queue_mutex;
    std::condition_variable queue_changed;
    std::thread worker;
    bool stopping=false;
    std::atomic<bool> failed{false};
    uint64_t encoded=0;
    void encode(Frame &frame){
        const auto encode_start=std::chrono::steady_clock::now();
        encoder->texture=frame.texture;encoder->input_view=frame.view;
        std::vector<uint8_t> bytes;
        const auto capture=[&](const uint8_t *p,uint32_t n){bytes.assign(p,p+n);return true;};
        if(!encoder->encode(!delivered,frame.id,capture))
            throw std::runtime_error(encoder->error);
        frame.view=encoder->input_view;
        if(frame.id%72==0){
            const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-encode_start).count();
            std::fprintf(stderr,"METEOR_OVR_ENCODE_TIMING frame=%llu copy_convert_encode_us=%lld\n",(unsigned long long)frame.id,(long long)us);std::fflush(stderr);
        }
        if(!meteor_steamvr_deliver(frame.id+1,frame.display_ns,config.eye_width,config.eye_height,frame.views.data(),bytes.data(),uint32_t(bytes.size())))
            throw std::runtime_error("Meteor video delivery failed");
        delivered=true;++encoded;
        if(*env("METEOR_OVR_CAPTURE_ACK") && *env("METEOR_PROBE_OUTPUT")){
            std::ofstream ack(std::filesystem::u8path(env("METEOR_PROBE_OUTPUT"))/"capture-complete");ack<<frame.display_ns;
        }
        if(encoded==1 && *env("METEOR_WINDOWS_SESSION_MARKER")){
            FILE *file=nullptr;if(fopen_s(&file,env("METEOR_WINDOWS_SESSION_MARKER"),"wb")==0)std::fclose(file);
        }
    }
    void run(){
        for(;;){
            Frame *frame;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_changed.wait(lock,[&]{return stopping || pending;});
                if(!pending)break;
                frame=pending;pending=nullptr;
                queue_changed.notify_all();
            }
            try{encode(*frame);}
            catch(const std::exception &e){
                std::fprintf(stderr,"METEOR_OVR_FAILURE operation=async_encode error=%s\n",e.what());std::fflush(stderr);
                failed=true;
            }
            {
                std::lock_guard<std::mutex> lock(queue_mutex);frame->busy=false;
                if(failed){if(pending)pending->busy=false;pending=nullptr;queue_changed.notify_all();break;}
            }
        }
    }
    template<class T> void symbol(T& f,const char *name){f=reinterpret_cast<T>(GetProcAddress(tracking,name));if(!f)throw std::runtime_error(std::string("Missing tracking export ")+name);}
    void initialize(){
        config.size=sizeof(config);config.version=1;
        config.eye_width=number("METEOR_XR_EYE_SIZE",1280,256,4096,16);
        config.eye_height=number("METEOR_XR_EYE_HEIGHT",1344,256,4096,16);
        config.refresh_hz=number("XRT_COMPOSITOR_NULL_FPS",72,20,90);
        if(config.refresh_hz!=20 && config.refresh_hz!=60 && config.refresh_hz!=72 && config.refresh_hz!=90)throw std::runtime_error("Unsupported refresh target");
        number("METEOR_XR_DIRECT_PORT",0,1,65535);
        if(!*env("METEOR_XR_DIRECT_PORT"))throw std::runtime_error("A local Meteor video relay is required");
        auto intel=meteor_d3d11_on(nullptr,0x8086);
        if(!intel)throw std::runtime_error("Intel D3D11 adapter unavailable");
        ArcQsvCapabilities caps{};
        if(arc_qsv_capabilities(intel.Get(),&caps)!=0)throw std::runtime_error("QSV hardware capabilities unavailable");
        if(config.eye_width*2>caps.max_width || config.eye_height>caps.max_height)throw std::runtime_error("Stereo frame exceeds QSV capabilities");
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
        gpu_check(intel.As(&dxgi),"QSV DXGI device");gpu_check(dxgi->GetAdapter(&adapter),"QSV adapter");gpu_check(adapter->GetDesc(&desc),"QSV adapter properties");
        std::memcpy(config.adapter_luid,&desc.AdapterLuid,8);
        auto path=absolute_library("METEOR_TRACKING_BRIDGE");
        tracking=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!tracking)throw std::runtime_error("Cannot load Meteor tracking DLL");
        symbol(start,"meteor_tracking_start_v1");symbol(stop,"meteor_tracking_stop_v1");symbol(head,"meteor_tracking_pose_v1");
        symbol(controller,"meteor_tracking_controller_v2");symbol(optics,"meteor_tracking_optics_v1");symbol(haptic,"meteor_tracking_haptic_v1");
        symbol(connected,"meteor_tracking_connected_v1");
        client=start(env("METEOR_TRACKING_PORT"));
        if(!client || !optics(client,config.optics))throw std::runtime_error("Meteor tracking/optics are not ready");
        float fov_scale=1;
        if(!meteor_projection_margin(env("METEOR_XR_FOV_SCALE"),config.optics,&fov_scale))
            throw std::runtime_error("Invalid METEOR_XR_FOV_SCALE (expected 1..1.4) or headset FOV");
        std::fprintf(stderr,"METEOR_RENDER_FOV scale=%.3f left=%.6f,%.6f,%.6f,%.6f right=%.6f,%.6f,%.6f,%.6f\n",
            fov_scale,config.optics[7],config.optics[8],config.optics[9],config.optics[10],
            config.optics[18],config.optics[19],config.optics[20],config.optics[21]);std::fflush(stderr);
    }
    ~Session(){
        {
            std::lock_guard<std::mutex> lock(queue_mutex);stopping=true;
        }
        queue_changed.notify_one();
        if(worker.joinable())worker.join();
        if(config.refresh_hz==90){
            std::fprintf(stderr,"METEOR_OVR_ASYNC_DRAIN encoded=%llu failed=%u\n",
                (unsigned long long)encoded,unsigned(failed.load()));std::fflush(stderr);
        }
        audio.stop();
        encoder.reset();
        const bool ended=!delivered || meteor_steamvr_finish()!=0;
        if(client && stop)stop(client);if(tracking)FreeLibrary(tracking);
        if(ended && delivered && *env("METEOR_WINDOWS_SESSION_MARKER")){
            auto path=std::filesystem::u8path(env("METEOR_WINDOWS_SESSION_MARKER"));path+=L".ended";
            FILE *file=nullptr;if(_wfopen_s(&file,path.c_str(),L"wb")==0)std::fclose(file);
        }
    }
};
void failure(const char *operation,const std::exception &e){std::fprintf(stderr,"METEOR_OVR_FAILURE operation=%s error=%s\n",operation,e.what());std::fflush(stderr);}
// Unity may create/destroy an instance while detecting devices, before its
// actual game instance. Preserve only a never-submitted backend across that
// bootstrap gap. It has no encoder, submitted GPU texture or video epoch yet.
// A temporary DLL reference keeps the worker/code alive between OVR drivers.
// The raw pending pointer is deliberately not destroyed under Windows loader
// lock at process detach; process exit owns cleanup if startup never completes.
std::mutex bootstrap_mutex;
Session *bootstrap_session=nullptr;
HMODULE bootstrap_module=nullptr;
}
extern "C" __declspec(dllexport) void *nightfall_ovr_open_v1(NightfallOvrConfigV1 *config){
    try{
        if(!config || config->size!=sizeof(*config))return nullptr;
        std::unique_ptr<Session> s;
        HMODULE hold=nullptr;
        {
            std::lock_guard<std::mutex> lock(bootstrap_mutex);
            s.reset(bootstrap_session);bootstrap_session=nullptr;
            hold=bootstrap_module;bootstrap_module=nullptr;
        }
        // The caller owns its ordinary LoadLibrary reference during this call.
        if(hold)FreeLibrary(hold);
        if(s && !s->connected(s->client))s.reset();
        if(!s){s=std::make_unique<Session>();s->initialize();}
        else{std::fprintf(stderr,"METEOR_OVR_BOOTSTRAP_REUSED\n");std::fflush(stderr);}
        *config=s->config;return s.release();
    }
    catch(const std::exception &e){failure("open",e);return nullptr;}
}
extern "C" __declspec(dllexport) int nightfall_ovr_sample_v1(void *raw,int64_t at,NightfallOvrStateV1 *state){
    try{
        if(!raw || !state || state->size!=sizeof(*state))return 0;
        auto &s=*static_cast<Session*>(raw);*state={};state->size=sizeof(*state);
        state->head_valid=s.head(s.client,at,state->head,&state->sample_ns)!=0;
        state->connected=s.connected(s.client)!=0;
        for(unsigned side=0;side<2;++side){uint64_t stamp=0;s.controller(side,at,state->hands[side],state->controls[side],&state->buttons[side],&state->flags[side],&stamp);}
        return state->head_valid?1:0;
    }catch(const std::exception &e){failure("sample",e);return 0;}
}
extern "C" __declspec(dllexport) int nightfall_ovr_submit_v1(void *raw,void *device,void *texture,uint64_t frame,uint64_t display_ns,const float *views){
    try{
        if(!raw || !device || !texture || !views)return 0;
        auto &s=*static_cast<Session*>(raw);std::lock_guard<std::mutex> lock(s.mutex);
        auto *d3d=static_cast<ID3D11Device*>(device);auto *source=static_cast<ID3D11Texture2D*>(texture);
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.Width!=s.config.eye_width*2 || desc.Height!=s.config.eye_height || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 ||
            (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))throw std::runtime_error("Invalid packed OVR frame");
        if(!s.encoder){
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_desc{};
            gpu_check(d3d->QueryInterface(IID_PPV_ARGS(&dxgi)),"Producer DXGI device");gpu_check(dxgi->GetAdapter(&adapter),"Producer adapter");gpu_check(adapter->GetDesc(&adapter_desc),"Producer adapter properties");
            if(std::memcmp(&adapter_desc.AdapterLuid,s.config.adapter_luid,8))throw std::runtime_error("OVR producer is on a different GPU");
            s.producer=d3d;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.MiscFlags=0;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            gpu_check(d3d->CreateTexture2D(&desc,nullptr,&s.input),"Create QSV input copy");
            const char *codec=env("METEOR_XR_CODEC");if(*codec && std::strcmp(codec,"h264") && std::strcmp(codec,"h265"))throw std::runtime_error("Unsupported OVR codec");
            uint32_t bitrate=0;if(!meteor_encoder_bitrate(env("METEOR_XR_BITRATE_MBPS"),32000000,&bitrate))throw std::runtime_error("Invalid OVR bitrate");
            s.encoder=std::make_unique<MeteorQsvEncoder>();s.encoder->attach(d3d,s.input.Get(),desc.Width,desc.Height);
            if(!s.encoder->start(!std::strcmp(codec,"h265"),s.config.refresh_hz,bitrate,true))throw std::runtime_error(s.encoder->error);
            if(s.config.refresh_hz==90){
                for(auto &slot:s.frames){
                    gpu_check(d3d->CreateTexture2D(&desc,nullptr,&slot.texture),"Create asynchronous QSV snapshot");
                }
                s.worker=std::thread([&s]{s.run();});
            }
            if(*env("METEOR_XR_AUDIO_PORT") && !s.audio.start(uint16_t(number("METEOR_XR_AUDIO_PORT",0,1,65535)))) {
                std::fprintf(stderr,"METEOR_OVR_AUDIO startup_failed=1; headset will retain desktop fallback\n");std::fflush(stderr);
            }
        }
        if(s.producer.Get()!=d3d)throw std::runtime_error("OVR producer device changed");
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;d3d->GetImmediateContext(&context);
        if(s.config.refresh_hz==90){
            std::unique_lock<std::mutex> lock(s.queue_mutex);
            if(!s.queue_changed.wait_for(lock,std::chrono::seconds(5),[&]{return !s.pending || s.failed || s.stopping;}))
                throw std::runtime_error("Asynchronous encoder queue timed out");
            if(s.failed || s.stopping)throw std::runtime_error("Asynchronous encoder unavailable");
            Session::Frame *slot=nullptr;
            for(auto &candidate:s.frames)if(!candidate.busy){slot=&candidate;break;}
            if(!slot)throw std::runtime_error("Asynchronous frame pool exhausted");
            // This copy is submitted before the app may reuse its packed image.
            context->CopyResource(slot->texture.Get(),source);context->Flush();
            slot->id=frame;slot->display_ns=display_ns;std::copy_n(views,22,slot->views.begin());slot->busy=true;
            s.pending=slot;s.queue_changed.notify_one();
        }else{
            context->CopyResource(s.input.Get(),source);
            Session::Frame slot;slot.texture=s.input;slot.view=s.encoder->input_view;
            slot.id=frame;slot.display_ns=display_ns;std::copy_n(views,22,slot.views.begin());s.encode(slot);
        }
        return 1;
    }catch(const std::exception &e){failure("submit",e);return 0;}
}
extern "C" __declspec(dllexport) int nightfall_ovr_haptic_v1(void *raw,uint32_t hand,float amplitude,int64_t duration){
    if(!raw || hand>1)return 0;return static_cast<Session*>(raw)->haptic(hand,amplitude,duration);
}
extern "C" __declspec(dllexport) void nightfall_ovr_close_v1(void *raw){
    auto *s=static_cast<Session*>(raw);
    if(!s)return;
    if(!s->delivered && !s->encoder && !s->producer && !s->input && s->client && s->connected(s->client)){
        std::lock_guard<std::mutex> lock(bootstrap_mutex);
        HMODULE hold=nullptr;
        if(!bootstrap_session && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&nightfall_ovr_close_v1),&hold)){
            bootstrap_session=s;bootstrap_module=hold;
            return;
        }
    }
    delete s;
}
extern "C" __declspec(dllexport) uint32_t meteor_encoder_capabilities_v1(char *json,uint32_t size){
    auto device=meteor_d3d11_on(nullptr,0x8086);ArcQsvCapabilities caps{};
    if(!device || arc_qsv_capabilities(device.Get(),&caps)!=0 || !json)return 0;
    const int n=std::snprintf(json,size,"{\"backend\":\"qsv\",\"refresh_rates\":[20,60,72,90],\"audio_pcm\":true,\"codecs\":[%s%s%s],\"max_frame\":[%u,%u]}",
        caps.codecs&1?"\"h264\"":"",caps.codecs==3?",":"",caps.codecs&2?"\"h265\"":"",caps.max_width,caps.max_height);
    return n>0 && uint32_t(n)<size?uint32_t(n):0;
}
