#include "pcvr_meteor_audio.h"
#ifdef __ANDROID__
#include "pcvr_audio_cipher.h"
#include <android/log.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>

namespace {
int64_t audio_millis() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
}
PCVRMeteorAudio::~PCVRMeteorAudio() {request_stop();if(worker.joinable())worker.join();}
std::string PCVRMeteorAudio::message() const {std::lock_guard<std::mutex> lock(mutex);return error;}
void PCVRMeteorAudio::fail(const char *why) {
    {std::lock_guard<std::mutex> lock(mutex);error=why;}
    if(!stop.load())failed=true;
    __android_log_print(ANDROID_LOG_WARN,"NightfallPCVRAudio","%s",why);
}
bool PCVRMeteorAudio::start(const Config &value) {
    if(!done || value.address.empty() || !value.port)return false;
    if(worker.joinable())worker.join();
    config=value;stop=false;done=false;ready=false;failed=false;enabled=false;rate=0;received_frames=0;played_frames=0;ring.reset();
    {std::lock_guard<std::mutex> lock(mutex);error.clear();}
    worker=std::thread(&PCVRMeteorAudio::run,this);return true;
}
void PCVRMeteorAudio::request_stop() {
    enabled=false;stop=true;
    std::lock_guard<std::mutex> lock(mutex);if(peer>=0)shutdown(peer,SHUT_RDWR);
}
bool PCVRMeteorAudio::connect_peer() {
    addrinfo hints{};hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;
    addrinfo *resolved=nullptr;
    if(getaddrinfo(config.address.c_str(),std::to_string(config.port).c_str(),&hints,&resolved))return false;
    int fd=socket(resolved->ai_family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd<0) {freeaddrinfo(resolved);return false;}
    {std::lock_guard<std::mutex> lock(mutex);peer=fd;}
    int result=::connect(fd,resolved->ai_addr,resolved->ai_addrlen);freeaddrinfo(resolved);
    if(result && errno!=EINPROGRESS)return false;
    const auto deadline=audio_millis()+5000;
    while(result && !stop && audio_millis()<deadline) {
        pollfd pending{fd,POLLOUT,0};
        if(poll(&pending,1,50)>0) {
            int error=0;socklen_t length=sizeof(error);
            if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length) || error)return false;
            result=0;
        }
    }
    int enabled=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&enabled,sizeof(enabled));
    return !result && !stop;
}
bool PCVRMeteorAudio::io(void *data,size_t size,bool writing,int timeout_ms) {
    auto *bytes=static_cast<uint8_t *>(data);const auto deadline=audio_millis()+timeout_ms;
    while(size && !stop && audio_millis()<deadline) {
        pollfd pending{peer,short(writing?POLLOUT:POLLIN),0};
        const auto result=poll(&pending,1,50);
        if(result<0 && errno==EINTR)continue;
        if(result<0)return false;
        if(!result)continue;
        const auto count=writing?send(peer,bytes,size,MSG_NOSIGNAL):recv(peer,bytes,size,0);
        if(count<0 && (errno==EAGAIN || errno==EINTR))continue;
        if(count<=0)return false;
        bytes+=count;size-=size_t(count);
    }
    return !size && !stop;
}
bool PCVRMeteorAudio::open_playback(uint32_t sample_rate) {
    AAudioStreamBuilder *builder=nullptr;
    if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK)return false;
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder,2);
    AAudioStreamBuilder_setSampleRate(builder,int32_t(sample_rate));
    AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder,AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(builder,[](AAudioStream *,void *owner,void *data,int32_t frames) -> aaudio_data_callback_result_t {
        auto &audio=*static_cast<PCVRMeteorAudio *>(owner);
        if(audio.stop.load())return AAUDIO_CALLBACK_RESULT_STOP;
        audio.ring.read(static_cast<int16_t *>(data),uint32_t(frames));audio.played_frames.fetch_add(uint32_t(frames));
        if(!audio.enabled.load())std::memset(data,0,size_t(frames)*4);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    },this);
    AAudioStreamBuilder_setErrorCallback(builder,[](AAudioStream *,void *owner,aaudio_result_t) {
        auto &audio=*static_cast<PCVRMeteorAudio *>(owner);audio.failed=true;audio.stop=true;
    },this);
    const auto result=AAudioStreamBuilder_openStream(builder,&stream);AAudioStreamBuilder_delete(builder);
    if(result!=AAUDIO_OK || !stream)return false;
    if(AAudioStream_getFormat(stream)!=AAUDIO_FORMAT_PCM_I16 || AAudioStream_getChannelCount(stream)!=2 ||
       AAudioStream_getSampleRate(stream)!=int32_t(sample_rate))return false;
    if(AAudioStream_requestStart(stream)!=AAUDIO_OK)return false;
    rate=sample_rate;ready=true;
    __android_log_print(ANDROID_LOG_INFO,"NightfallPCVRAudio","AAudio ready rate=%u stereo PCM16",sample_rate);
    return true;
}
void PCVRMeteorAudio::run() {
    bool success=false;
    do {
        if(!connect_peer()) {fail("Cannot connect authenticated XR audio lane");break;}
        std::array<uint8_t,32> challenge{};
        if(!io(challenge.data(),challenge.size(),false))break;
        PCVRAudioCipher attach,receive,feedback;
        auto init=[&](PCVRAudioCipher &cipher,uint8_t kind) {return cipher.init(config.host,config.identity,config.server_nonce,config.client_nonce,config.session,kind,challenge);};
        if(!init(attach,8) || !init(receive,7) || !init(feedback,9)) {fail("XR audio key agreement failed");break;}
        std::vector<uint8_t> proof(32),encrypted;std::memcpy(proof.data(),"NFXB",4);proof[4]=3;
        std::copy(config.session.begin(),config.session.end(),proof.begin()+8);
        if(!attach.seal(proof,encrypted) || !io(encrypted.data(),encrypted.size(),true))break;
        uint64_t sequence=0;
        while(!stop) {
            std::array<uint8_t,PCVRAudioCipher::header_size> header{};size_t bytes=0;
            if(!io(header.data(),header.size(),false,sequence?5000:45000) || !receive.length(header,bytes))break;
            std::vector<uint8_t> body(bytes),plain;
            if(!io(body.data(),body.size(),false) || !receive.open(header,std::move(body),plain))break;
            MeteorPcmPacket packet;
            if(!MeteorPcmPacket::parse(plain.data(),plain.size(),packet) || packet.sequence!=sequence ||
               (!sequence && packet.frames) || (sequence && packet.rate!=rate.load())) {fail("XR audio packet validation failed");break;}
            if(!sequence && !open_playback(packet.rate)) {fail("Cannot start independent headset audio output");break;}
            if(packet.frames) {ring.push(plain.data()+MeteorPcmPacket::header_size,packet.frames,packet.rate);received_frames.fetch_add(packet.frames);}
            std::vector<uint8_t> ack(12);std::memcpy(ack.data(),"NFAC",4);MeteorPcmPacket::put(ack.data()+4,sequence,8);
            if(!feedback.seal(ack,encrypted) || !io(encrypted.data(),encrypted.size(),true))break;
            if(sequence==UINT64_MAX)break;
            ++sequence;
        }
        success=stop.load();
    } while(false);
    if(!success && !stop)fail("Independent XR audio channel ended; restore desktop fallback");
    ready=false;
    if(stream) {AAudioStream_requestStop(stream);AAudioStream_close(stream);stream=nullptr;}
    {std::lock_guard<std::mutex> lock(mutex);if(peer>=0)::close(peer);peer=-1;}
    std::fill(config.identity.begin(),config.identity.end(),0);done=true;
}
#endif
