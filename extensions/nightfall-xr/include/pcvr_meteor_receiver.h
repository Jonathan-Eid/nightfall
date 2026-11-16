#pragma once
#ifdef __ANDROID__
#include "pcvr_meteor_wire.h"
#include "pcvr_frame_accounting.h"
#include "pcvr_decoded_region.h"
#include "pcvr_latency_histogram.h"
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <string>

// DEV-only listener on 127.0.0.1:49090; reached through an authorized ADB forward.
class PCVRMeteorReceiver {
public:
    ~PCVRMeteorReceiver();
    bool start();
    void stop_after_gpu_drain();
    bool idle() const { return done.load(); }
    bool failed() const { return failure.load(); }
    bool listening() const { return ready.load(); }
    std::string message() const;
    bool frame(AHardwareBuffer *&buffer, MeteorXrFrame &metadata, PCVRDecodedRegion &region);
    void release_frame(); // Render thread, only after its sampling fence signals.
    PCVRFrameAccounting stats;
    PCVRLatencyHistogram receive_to_output, output_to_selection, receive_to_selection, selection_intervals;
    std::atomic<int> decoder_poll_ms{1}; // Wake to drain hardware output independently of the next network frame.
    std::atomic<bool> buffered_frames{true}; // Preserve brief bursts; consumer age is bounded to two periods.
    std::atomic<uint64_t> queue_age_trims{0};
    std::atomic<uint32_t> video_width{0}, video_height{0}, video_codec{0}, video_fps{0};
    std::atomic<uint64_t> received_bytes{0};
private:
    std::atomic<bool> done{true}, stop{false}, failure{false};
    std::atomic<bool> ready{false};
    std::thread worker;
    mutable std::mutex mutex;
    std::mutex network_mutex;
    std::string text;
    struct TimedFrame { MeteorXrFrame frame; uint64_t received_us=0, output_us=0; };
    std::map<int64_t, TimedFrame> metadata;
    uint64_t last_selected_us=0; // Render thread only.
    AImageReader *reader = nullptr;
    AImage *held = nullptr; // Render thread owns held image and acquire fence.
    int acquire_fence = -1;
    bool borrowed = false;
    uint32_t last_buffer_width=0,last_buffer_height=0;
    AMediaCodec *codec = nullptr; // Worker only.
    bool codec_started = false, eos = false;
    int peer = -1; // Close/shutdown serialized to avoid FD reuse races.
    void status(const std::string &value, bool error = false);
    void run();
    bool io(int fd, void *data, size_t size, bool writing, int64_t deadline);
    bool decoder(const MeteorXrFrame &frame);
    bool drain();
    bool input(const std::vector<uint8_t> &bytes, int64_t pts, bool end);
};
#endif
