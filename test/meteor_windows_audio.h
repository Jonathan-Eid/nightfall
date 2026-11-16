// Windows shared-mode loopback audio for Nightfall's standalone XR backend.
// Captures the normal playback endpoint; no driver or machine-wide audio changes.
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class MeteorWindowsAudio {
public:
    using Sink = std::function<bool(uint32_t, const int16_t *, uint32_t)>;
    ~MeteorWindowsAudio() { stop(); }
    bool start(Sink sink) {
        stop();
        stop_requested=false; ready=false; failed=false; captured_frames=0; discontinuities=0;
        { std::lock_guard<std::mutex> lock(mutex); error_text.clear(); }
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event) { fail("Cannot create loopback event",HRESULT_FROM_WIN32(GetLastError())); return false; }
        worker=std::thread([this,sink=std::move(sink)] { run(sink); });
        return true;
    }
    void stop() {
        stop_requested=true;
        if(event) SetEvent(event);
        if(worker.joinable()) worker.join();
        if(event) CloseHandle(event);
        event=nullptr;
        ready=false;
    }
    std::string error() const { std::lock_guard<std::mutex> lock(mutex); return error_text; }
    std::atomic<bool> ready{false},failed{false};
    std::atomic<uint64_t> captured_frames{0},discontinuities{0};
    std::atomic<uint32_t> sample_rate{0};
private:
    std::atomic<bool> stop_requested{true};
    HANDLE event=nullptr;
    std::thread worker;
    mutable std::mutex mutex;
    std::string error_text;
    void fail(const char *what,HRESULT code) {
        char text[192]; std::snprintf(text,sizeof(text),"%s (0x%08lx)",what,static_cast<unsigned long>(code));
        { std::lock_guard<std::mutex> lock(mutex); error_text=text; }
        failed=true;
    }
    void run(const Sink &sink) {
        using Microsoft::WRL::ComPtr;
        HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(hr)) { fail("Cannot initialize loopback COM",hr); return; }
        struct ComScope { ~ComScope(){CoUninitialize();} } com_scope;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> endpoint;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
        if(FAILED(hr)) { fail("Cannot enumerate loopback device",hr); return; }
        hr=enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&endpoint);
        if(FAILED(hr)) { fail("No default playback endpoint for XR audio",hr); return; }
        hr=endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client);
        if(FAILED(hr)) { fail("Cannot activate loopback audio",hr); return; }
        WAVEFORMATEX *format=nullptr;
        hr=client->GetMixFormat(&format);
        if(FAILED(hr) || !format) { fail("Cannot read playback mix format",hr); return; }
        struct FormatScope { WAVEFORMATEX *p; ~FormatScope(){CoTaskMemFree(p);} } format_scope{format};
        WORD tag=format->wFormatTag;
        if(tag==WAVE_FORMAT_EXTENSIBLE && format->cbSize>=22) {
            const auto *extended=reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
            if(IsEqualGUID(extended->SubFormat,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag=WAVE_FORMAT_IEEE_FLOAT;
            else if(IsEqualGUID(extended->SubFormat,KSDATAFORMAT_SUBTYPE_PCM)) tag=WAVE_FORMAT_PCM;
            else tag=0;
        }
        const unsigned channels=format->nChannels,bits=format->wBitsPerSample,bytes=bits/8;
        const uint32_t rate=format->nSamplesPerSec;
        // Preserve the desktop audio fallback for multichannel endpoints. A
        // front-left/right-only conversion would discard centre-channel speech.
        if(!channels || channels>2 || rate<8000 || rate>192000 ||
           format->nBlockAlign!=channels*bytes ||
           !((tag==WAVE_FORMAT_IEEE_FLOAT && bits==32) || (tag==WAVE_FORMAT_PCM && (bits==16 || bits==24 || bits==32)))) {
            fail("Unsupported playback mix format",AUDCLNT_E_UNSUPPORTED_FORMAT); return;
        }
        hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              200000,0,format,nullptr);
        if(FAILED(hr)) { fail("Cannot initialize loopback capture",hr); return; }
        hr=client->SetEventHandle(event);
        if(FAILED(hr)) { fail("Cannot assign loopback event",hr); return; }
        hr=client->GetService(IID_PPV_ARGS(&capture));
        if(FAILED(hr)) { fail("Cannot obtain loopback buffers",hr); return; }
        hr=client->Start();
        if(FAILED(hr)) { fail("Cannot start XR loopback audio",hr); return; }
        struct ClientScope { IAudioClient *p; ~ClientScope(){p->Stop();} } client_scope{client.Get()};
        sample_rate=rate;
        if(!sink(rate,nullptr,0)) { fail("XR audio configuration was rejected",E_ABORT); return; }
        ready=true;
        std::vector<int16_t> samples;
        while(!stop_requested.load()) {
            WaitForSingleObject(event,50);
            UINT32 available=0;
            hr=capture->GetNextPacketSize(&available);
            if(FAILED(hr)) { fail("Cannot poll loopback buffers",hr); break; }
            if(!available && !stop_requested.load()) {
                // Loopback has no packets when every playback application is
                // idle. Keep the independent channel alive with bounded silence.
                samples.assign(size_t(rate/100)*2,0);
                if(!sink(rate,samples.data(),rate/100)) { fail("XR audio silence transport failed",E_ABORT); return; }
            }
            while(available && !stop_requested.load()) {
                BYTE *data=nullptr; UINT32 frames=0; DWORD flags=0;
                hr=capture->GetBuffer(&data,&frames,&flags,nullptr,nullptr);
                if(FAILED(hr)) { fail("Cannot acquire loopback buffer",hr); return; }
                if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
                samples.assign(size_t(frames)*2,0);
                if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    for(UINT32 frame=0;frame<frames;++frame) for(unsigned side=0;side<2;++side) {
                        const auto *p=data+size_t(frame)*format->nBlockAlign+std::min(side,channels-1)*bytes;
                        float sample=0;
                        if(tag==WAVE_FORMAT_IEEE_FLOAT) std::memcpy(&sample,p,4);
                        else if(bits==16) { int16_t value;std::memcpy(&value,p,2);sample=value/32768.f; }
                        else if(bits==24) { int32_t value=int32_t(uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16));if(value&0x800000)value|=~0xffffff;sample=value/8388608.f; }
                        else { int32_t value;std::memcpy(&value,p,4);sample=static_cast<float>(value/2147483648.0); }
                        if(!std::isfinite(sample)) sample=0;
                        samples[size_t(frame)*2+side]=static_cast<int16_t>(std::clamp(sample,-1.f,32767.f/32768.f)*32768.f);
                    }
                }
                hr=capture->ReleaseBuffer(frames);
                if(FAILED(hr)) { fail("Cannot release loopback buffer",hr); return; }
                captured_frames.fetch_add(frames);
                // Bound every audio packet to at most 10 ms (and 1920 frames
                // even for high-rate playback devices). No unbounded queue.
                const uint32_t chunk=std::max(1u,rate/100);
                for(uint32_t at=0;at<frames && !stop_requested.load();at+=chunk) {
                    if(!sink(rate,samples.data()+size_t(at)*2,std::min(chunk,frames-at))) {
                        fail("XR audio transport stopped accepting samples",E_ABORT); return;
                    }
                }
                hr=capture->GetNextPacketSize(&available);
                if(FAILED(hr)) { fail("Cannot advance loopback buffers",hr); return; }
            }
        }
    }
};
