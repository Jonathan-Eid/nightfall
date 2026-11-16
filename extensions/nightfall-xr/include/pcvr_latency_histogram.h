#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// Local monotonic durations only. No host/headset clock subtraction and no
// claim that a selected decoder image has reached the physical display.
struct PCVRLatencyHistogram {
    static constexpr unsigned bins = 128;
    std::array<std::atomic<uint64_t>, bins> values{};
    std::atomic<uint64_t> count{0}, sum_us{0}, maximum_us{0};
    void reset() {
        for (auto &value : values) value.store(0);
        count=0; sum_us=0; maximum_us=0;
    }
    void observe(uint64_t us) {
        unsigned bin=us/1000 >= bins ? bins-1 : unsigned(us/1000);
        values[bin].fetch_add(1, std::memory_order_relaxed);
        sum_us.fetch_add(us, std::memory_order_relaxed);
        uint64_t previous=maximum_us.load(std::memory_order_relaxed);
        while(previous<us && !maximum_us.compare_exchange_weak(previous,us,std::memory_order_relaxed)) {}
        count.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t percentile_upper_us(unsigned percent) const {
        const uint64_t n=count.load(std::memory_order_relaxed);
        if(!n || !percent || percent>100) return 0;
        const uint64_t rank=(n*percent+99)/100;
        uint64_t total=0;
        for(unsigned i=0;i<bins;++i) {
            total+=values[i].load(std::memory_order_relaxed);
            if(total>=rank) return i==bins-1 ? maximum_us.load() : uint64_t(i+1)*1000;
        }
        return maximum_us.load(); // A concurrent writer may be mid-update.
    }
};
