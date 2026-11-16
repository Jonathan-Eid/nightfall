#include "meteor_windows_audio.h"
#include <chrono>
#include <cstdio>
int main() {
    MeteorWindowsAudio audio;
    std::atomic<uint64_t> packets{0};
    std::atomic<unsigned> peak{0};
    if(!audio.start([&](uint32_t,const int16_t *data,uint32_t frames) {
        ++packets;
        for(uint32_t i=0;i<frames*2;++i) {
            unsigned value=unsigned(std::abs(int(data[i]))),previous=peak.load();
            while(value>previous && !peak.compare_exchange_weak(previous,value)) {}
        }
        return true;
    })) return 1;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(std::chrono::steady_clock::now()<deadline && !audio.failed)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    audio.stop();
    std::printf("LOOPBACK_AUDIO rate=%u stereo_frames=%llu packets=%llu peak=%u discontinuities=%llu error=%s\n",
        audio.sample_rate.load(),(unsigned long long)audio.captured_frames.load(),(unsigned long long)packets.load(),
        peak.load(),(unsigned long long)audio.discontinuities.load(),audio.error().c_str());
    return audio.failed || !audio.sample_rate ? 1 : 0;
}
