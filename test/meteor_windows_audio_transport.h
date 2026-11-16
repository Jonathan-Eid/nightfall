#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include "meteor_windows_audio.h"
#include "../extensions/nightfall-xr/include/pcvr_audio_wire.h"

// The managed runtime supplies only a loopback publisher port. Meteor owns the
// encrypted headset channel, peer admission, lease lifetime and cancellation.
class MeteorWindowsAudioTransport {
    SOCKET socket=INVALID_SOCKET;
    bool winsock=false;
    uint64_t sequence=0;
    MeteorWindowsAudio audio;
    bool io(void *raw,size_t length,bool writing) {
        auto *bytes=static_cast<char *>(raw);
        while(length) {
            const int count=writing ? send(socket,bytes,int(length),0) : recv(socket,bytes,int(length),0);
            if(count<=0)return false;
            bytes+=count;length-=count;
        }
        return true;
    }
public:
    ~MeteorWindowsAudioTransport() { stop(); }
    bool start(uint16_t port) {
        stop();
        WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return false;winsock=true;
        socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(socket==INVALID_SOCKET)return false;
        DWORD timeout=500;setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char *>(&timeout),sizeof(timeout));
        setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char *>(&timeout),sizeof(timeout));
        int enabled=1;setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char *>(&enabled),sizeof(enabled));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
        if(connect(socket,reinterpret_cast<sockaddr *>(&address),sizeof(address)))return false;
        sequence=0;
        return audio.start([this](uint32_t rate,const int16_t *samples,uint32_t frames) {
            MeteorPcmPacket packet{rate,frames,sequence};auto header=packet.header();
            if(!packet.valid() || !io(header.data(),header.size(),true) ||
               (frames && !io(const_cast<int16_t *>(samples),size_t(frames)*4,true)))return false;
            std::array<uint8_t,12> ack{};
            if(!io(ack.data(),ack.size(),false) || std::memcmp(ack.data(),"NFAC",4) ||
               MeteorPcmPacket::get(ack.data()+4,8)!=sequence)return false;
            ++sequence;return true;
        });
    }
    void stop() {
        if(socket!=INVALID_SOCKET)shutdown(socket,SD_BOTH);
        audio.stop();
        if(socket!=INVALID_SOCKET)closesocket(socket);
        socket=INVALID_SOCKET;
        if(winsock)WSACleanup();winsock=false;
    }
};
