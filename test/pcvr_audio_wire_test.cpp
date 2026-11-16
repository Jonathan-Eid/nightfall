#include "pcvr_audio_wire.h"
#include <cassert>
#include <thread>
int main() {
    MeteorPcmPacket config{48000,0,0}, parsed;
    auto header=config.header();
    assert(MeteorPcmPacket::parse(header.data(),header.size(),parsed));
    assert(parsed.rate==48000 && !parsed.frames && !parsed.sequence);
    auto bad=header;bad[12]=1;assert(!MeteorPcmPacket::parse(bad.data(),bad.size(),parsed));
    bad=header;bad[24]=1;assert(!MeteorPcmPacket::parse(bad.data(),bad.size(),parsed));
    bad=header;bad[8]=0;bad[9]=0;assert(!MeteorPcmPacket::parse(bad.data(),bad.size(),parsed));
    MeteorPcmPacket packet{48000,480,1};header=packet.header();
    std::vector<uint8_t> bytes(header.begin(),header.end());bytes.resize(32+480*4);
    assert(MeteorPcmPacket::parse(bytes.data(),bytes.size(),parsed));
    assert(!MeteorPcmPacket::parse(bytes.data(),bytes.size()-1,parsed));
    bytes[20]^=1;assert(!MeteorPcmPacket::parse(bytes.data(),bytes.size(),parsed));bytes[20]^=1;
    MeteorPcmRing ring;
    std::vector<uint8_t> pcm(480*4);
    for(size_t i=0;i<480;++i) { MeteorPcmPacket::put(pcm.data()+i*4,0x80007fffu,4); }
    ring.push(pcm.data(),480,48000);assert(ring.queued()==480);
    std::array<int16_t,960> output{};ring.read(output.data(),480);
    assert(!ring.queued() && !ring.underflows);
    for(size_t i=0;i<480;++i)assert(output[i*2]==32767 && output[i*2+1]==-32768);
    ring.read(output.data(),480);assert(ring.underflows==480);
    for(auto v:output)assert(v==0);
    for(unsigned i=0;i<10;++i)ring.push(pcm.data(),480,48000);
    assert(ring.queued()<=1920 && ring.dropped>0);
    ring.reset();
    std::thread producer([&] { for(unsigned i=0;i<1000;++i)ring.push(pcm.data(),480,48000); });
    for(unsigned i=0;i<1000;++i)ring.read(output.data(),480);
    producer.join();
    assert(ring.queued()<=1920);
}
