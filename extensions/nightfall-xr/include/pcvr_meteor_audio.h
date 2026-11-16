#pragma once
#ifdef __ANDROID__
#include "pcvr_audio_wire.h"
#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class PCVRMeteorAudio {
public:
    struct Config {
        std::string address;
        uint16_t port=0;
        std::array<uint8_t,32> host{},identity{},server_nonce{},client_nonce{};
        std::array<uint8_t,16> session{};
    };
    ~PCVRMeteorAudio();
    bool start(const Config &value);
    void request_stop();
    bool idle() const {return done.load();}
    std::string message() const;
    std::atomic<bool> ready{false},failed{false},enabled{false};
    std::atomic<uint32_t> rate{0};
    std::atomic<uint64_t> received_frames{0},played_frames{0};
    MeteorPcmRing ring;
private:
    Config config;
    std::thread worker;
    std::atomic<bool> stop{true},done{true};
    mutable std::mutex mutex;
    std::string error;
    int peer=-1;
    AAudioStream *stream=nullptr;
    void run();
    bool connect_peer();
    bool io(void *data,size_t size,bool writing,int timeout_ms=5000);
    bool open_playback(uint32_t sample_rate);
    void fail(const char *why);
};
#endif
