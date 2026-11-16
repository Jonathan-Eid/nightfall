#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// PCM is carried on its own authenticated XR data channel, never on an open
// LAN listener. Stereo signed 16-bit little endian, up to 10 ms per packet.
struct MeteorPcmPacket {
    static constexpr size_t header_size=32, max_frames=1920, max_payload=max_frames*4;
    uint32_t rate=0,frames=0;
    uint64_t sequence=0;
    static uint64_t get(const uint8_t *p,unsigned bytes) {
        uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=uint64_t(p[i])<<(i*8);return value;
    }
    static void put(uint8_t *p,uint64_t value,unsigned bytes) {
        for(unsigned i=0;i<bytes;++i)p[i]=uint8_t(value>>(i*8));
    }
    bool valid() const {
        return rate>=8000 && rate<=192000 && frames<=rate/100 && frames<=max_frames && (frames || !sequence);
    }
    std::array<uint8_t,header_size> header() const {
        std::array<uint8_t,header_size> result{};
        std::memcpy(result.data(),"NFAP",4);put(result.data()+4,1,2);put(result.data()+6,header_size,2);
        put(result.data()+8,rate,4);put(result.data()+12,2,2);put(result.data()+14,1,2);
        put(result.data()+16,frames,4);put(result.data()+20,frames*4,4);put(result.data()+24,sequence,8);
        return result;
    }
    static bool parse(const uint8_t *data,size_t length,MeteorPcmPacket &result,bool complete=true) {
        if(length<header_size || std::memcmp(data,"NFAP",4) || get(data+4,2)!=1 || get(data+6,2)!=header_size ||
           get(data+12,2)!=2 || get(data+14,2)!=1) return false;
        result.rate=uint32_t(get(data+8,4));result.frames=uint32_t(get(data+16,4));result.sequence=get(data+24,8);
        return result.valid() && get(data+20,4)==result.frames*4 && (!complete || length==header_size+result.frames*4);
    }
};

// A single producer and callback consumer, with atomic packed stereo samples.
// Producer catch-up can advance read without racing the callback's sample load.
// No callback locks, allocation, network I/O or unbounded buffered audio.
class MeteorPcmRing {
    static constexpr size_t capacity=16384;
    std::array<std::atomic<uint32_t>,capacity> samples{};
    std::atomic<uint64_t> write_index{0},read_index{0};
public:
    std::atomic<uint64_t> dropped{0},underflows{0};
    void reset() { write_index=0;read_index=0;dropped=0;underflows=0; }
    uint64_t queued() const {
        const auto r=read_index.load(std::memory_order_acquire),w=write_index.load(std::memory_order_acquire);
        return w>r?w-r:0;
    }
    void push(const uint8_t *pcm,uint32_t frames,uint32_t rate) {
        const auto w=write_index.load(std::memory_order_relaxed);
        auto r=read_index.load(std::memory_order_acquire);
        if(w>=r && w-r+frames>rate/25) {
            while(r<w && !read_index.compare_exchange_weak(r,w,std::memory_order_acq_rel)) {}
            if(r<w)dropped.fetch_add(w-r,std::memory_order_relaxed);
        }
        for(uint32_t i=0;i<frames;++i) {
            const uint32_t pair=uint32_t(MeteorPcmPacket::get(pcm+size_t(i)*4,4));
            samples[(w+i)%capacity].store(pair,std::memory_order_relaxed);
        }
        write_index.store(w+frames,std::memory_order_release);
    }
    void read(int16_t *output,uint32_t frames) {
        uint64_t missing=0;
        for(uint32_t i=0;i<frames;++i) {
            auto r=read_index.load(std::memory_order_relaxed);
            const auto w=write_index.load(std::memory_order_acquire);
            uint32_t pair=0;
            if(r<w) {
                // Claim before loading; a producer resync uses atomic samples,
                // so even a late callback cannot race a recycled slot.
                if(read_index.compare_exchange_strong(r,r+1,std::memory_order_acq_rel))
                    pair=samples[r%capacity].load(std::memory_order_relaxed);
                else ++missing;
            } else ++missing;
            output[size_t(i)*2]=int16_t(pair&0xffff);output[size_t(i)*2+1]=int16_t(pair>>16);
        }
        underflows.fetch_add(missing,std::memory_order_relaxed);
    }
};
