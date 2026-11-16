#pragma once
#include "meteor_cipher.h"
#include "pcvr_audio_wire.h"
#include <algorithm>
#include <string>

// Uses the same MeteorCipher implementation as the streaming extension.
// Separate class/labels avoid changing the existing video/tracking relay ABI.
class PCVRAudioCipher {
    godot::MeteorCipher cipher;
    uint64_t sequence=0;
    uint8_t kind=0;
public:
    static constexpr size_t header_size=24;
    bool init(const std::array<uint8_t,32> &host,const std::array<uint8_t,32> &identity,
              const std::array<uint8_t,32> &server_nonce,const std::array<uint8_t,32> &client_nonce,
              const std::array<uint8_t,16> &session,uint8_t packet_kind,const std::array<uint8_t,32> &challenge) {
        if(packet_kind<7 || packet_kind>9 || std::all_of(session.begin(),session.end(),[](uint8_t b){return !b;}))return false;
        kind=packet_kind;sequence=0;
        std::string info="nightfall-xr-data-v1/";info.push_back(char(kind));
        auto append=[&](const auto &bytes){info.append(reinterpret_cast<const char *>(bytes.data()),bytes.size());};
        append(session);append(server_nonce);append(client_nonce);append(challenge);
        return cipher.init_bytes(host.data(),reinterpret_cast<const uint8_t *>(info.data()),info.size(),identity.data()).empty();
    }
    bool length(const std::array<uint8_t,header_size> &header,size_t &bytes) const {
        if(kind!=7 || sequence==UINT64_MAX || std::memcmp(header.data(),"NFXD",4) || header[4]!=1 || header[5]!=kind ||
           header[6] || header[7] || MeteorPcmPacket::get(header.data()+8,8)!=sequence || MeteorPcmPacket::get(header.data()+20,4))return false;
        bytes=size_t(MeteorPcmPacket::get(header.data()+16,4));
        return bytes>=16 && bytes<=MeteorPcmPacket::header_size+MeteorPcmPacket::max_payload+16;
    }
    bool open(const std::array<uint8_t,header_size> &header,std::vector<uint8_t> body,std::vector<uint8_t> &plain) {
        size_t bytes=0;
        if(!length(header,bytes) || bytes!=body.size() || !cipher.open(sequence,header.data(),header.size(),body.data(),body.size()))return false;
        ++sequence;body.resize(body.size()-16);plain=std::move(body);return true;
    }
    bool seal(const std::vector<uint8_t> &plain,std::vector<uint8_t> &packet) {
        if((kind!=8 && kind!=9) || sequence==UINT64_MAX || plain.size()!=(kind==8?32:12))return false;
        std::array<uint8_t,header_size> header{};std::memcpy(header.data(),"NFXD",4);header[4]=1;header[5]=kind;
        MeteorPcmPacket::put(header.data()+8,sequence,8);MeteorPcmPacket::put(header.data()+16,plain.size()+16,4);
        auto body=plain;body.resize(plain.size()+16);
        if(!cipher.seal(sequence,header.data(),header.size(),body.data(),plain.size()))return false;
        ++sequence;packet.assign(header.begin(),header.end());packet.insert(packet.end(),body.begin(),body.end());return true;
    }
};
